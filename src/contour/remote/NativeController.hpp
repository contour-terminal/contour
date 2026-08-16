// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeController` — the GUI's native-protocol attach engine.
///
/// One controller = one connection to a `contour daemon`. It runs the
/// Qt-free `vthost::client::NativeClient` on its own reactor thread
/// (ReactorThread) and doubles as the app's `SessionFactory` while attached:
/// every locally created tab is backed by a `vtpty::ChannelPty` bound to one
/// remote session. Input and resize flow out through that pty's sinks onto the
/// reactor.
///
/// Remote OUTPUT does not come back through it. A per-session `ScreenMirror`
/// populates the pane's `vtbackend::Terminal` grid directly from the delta
/// stream, so the display stack never learns the session is remote — and
/// nothing is lost to an escape-sequence round trip on the way. That needs the
/// terminal, which does not exist when `createPty` hands out the pty, so
/// `bindTerminal` closes the cycle and primes the mirror.

#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>
#include <contour/session/SessionFactory.hpp>

#include <vtbackend/Settings.hpp>

#include <vtpty/ChannelPty.hpp>

#include <QtCore/QObject>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::remote
{

/// The attach-mode session factory and remote-session registry.
class NativeController final: public QObject, public contour::session::SessionFactory, public RemoteController
{
    Q_OBJECT

  public:
    /// @param endpoint How to reach the daemon: the local unix control socket, or
    ///        a TLS-encrypted, token-authenticated TCP endpoint.
    /// @param sessionSettings The emulation settings to ask the daemon for on the sessions this
    ///        client creates — this client's own resolved profile — or nullopt to take whatever the
    ///        daemon hosts with. Not defaulted, so a caller with no preference says so.
    NativeController(vthost::AttachEndpoint endpoint, std::optional<vtbackend::Settings> sessionSettings);

    /// Stops the reactor (detaching if still connected) and joins.
    ~NativeController() override;

    NativeController(NativeController const&) = delete;
    NativeController& operator=(NativeController const&) = delete;
    NativeController(NativeController&&) = delete;
    NativeController& operator=(NativeController&&) = delete;

    // connectAndWait() and stop() are inherited from RemoteController; this
    // controller supplies the hooks below.

    /// @return How many remote sessions await a local tab.
    [[nodiscard]] std::size_t pendingCount() const;

    /// @return True if @p session already has a local pane bound to it — used by
    ///         the incremental layout reconciler to skip tabs it already realized.
    [[nodiscard]] bool isBound(uint64_t session) const;

    /// @return True if @p session is claimed locally either way — bound to a pane,
    ///         or tombstoned by a user close the daemon has not acknowledged yet
    ///         (a stale layout push must not resurrect it). One lock acquisition
    ///         for the reconciler walks that check per leaf.
    [[nodiscard]] bool isClaimed(uint64_t session) const;

    /// @return The remote session bound to local pty @p pty, or nullopt. Lets the
    ///         reconciler map a GUI pane back to its daemon session (to split/close
    ///         the right pane).
    [[nodiscard]] std::optional<uint64_t> sessionForPty(vtpty::Pty const* pty) const;

    /// Asks the daemon to create a new tab (B3-Qt). The daemon honors it and
    /// re-pushes its layout, which the GUI reconciles into a new local tab. A no-op
    /// once detached.
    /// @param actingPty A pty of the WINDOW the tab belongs in — the daemon hosts more than one
    ///        and resolves the window from the session this pty is bound to. Null or unbound
    ///        falls back to the daemon's first window.
    void requestCreateTab(vtpty::Pty const* actingPty);

    /// Asks the daemon to create a new window (B4). The daemon honors it and pushes
    /// that window's LayoutState; the GUI opens a matching OS window and reconciles
    /// the window's tabs into it. A no-op once detached.
    void requestCreateWindow();

    /// Asks the daemon to split the pane hosting the remote session bound to local
    /// pty @p actingPty (@p vertical orientation). The daemon honors it and
    /// re-pushes its layout, which reconciles into a local split. A no-op if the pty
    /// is not bound or the connection is gone.
    void requestSplitPane(vtpty::Pty const* actingPty, bool vertical);

    /// Asks the daemon to close the pane hosting @p session (destroying that remote
    /// session). Sent when the user closes a mirror pane, so the session is really
    /// removed rather than left running headless.
    void requestClosePane(uint64_t session);

    /// @return The daemon window ids currently known, ascending. The daemon starts
    ///         with one window and grows via NewWindow; the GUI maps one OS window to
    ///         each (B4). A thread-safe snapshot.
    [[nodiscard]] std::vector<uint64_t> windowIds() const;

    /// @return The daemon's most recent tab/pane layout for @p daemonWindow, or
    ///         nullopt if that window is unknown. A thread-safe copy — the reactor
    ///         thread updates it. The GUI reconstructs one OS window's tree from it.
    [[nodiscard]] std::optional<vthost::proto::LayoutState> layout(uint64_t daemonWindow) const;

    /// @return The primary (lowest-id) daemon window's layout, or nullopt if none has
    ///         arrived yet. Convenience for the single-window path.
    [[nodiscard]] std::optional<vthost::proto::LayoutState> layout() const;

    /// @return The primary (lowest-id) daemon window's layout converted for
    ///         `vtworkspace::realizeLayoutTab` (an empty layout if none has arrived), plus
    ///         its leaf→remote-session map. The layout executor realizes this to
    ///         reproduce the daemon tree.
    [[nodiscard]] vthost::client::WireLayout wireLayout() const;

    /// Binds the NEXT createPty() to remote session @p session (instead of popping
    /// the FIFO pending queue). The layout executor calls this — via
    /// applyLayoutToWindow's beforeLeafSeed — right before each pane's backing
    /// session is created, so the imminent pane binds to exactly that session.
    void setNextBindSession(uint64_t session);

    /// Brackets a layout realization. While set, canCreateSession() reports true
    /// even with no pending session — during realization panes are bound by
    /// setNextBindSession, not the FIFO queue.
    void setRealizingLayout(bool realizing);

    /// @return Whether a layout realization is in progress (so a split it triggers
    ///         is built locally, not re-authored on the daemon).
    [[nodiscard]] bool isRealizingLayout() const;

    // SessionFactory: hands out a ChannelPty bound to the next pending
    // remote session; cwd/command/profile do not apply to remote sessions.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override;

    [[nodiscard]] bool canCreateSession() const noexcept override;

    /// SessionFactory: a GUI "new tab" is authored on the daemon (requestCreateTab)
    /// rather than created locally; the daemon's layout re-push reconciles it in.
    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty) override
    {
        requestCreateTab(actingPty);
        return true;
    }

    /// SessionFactory: a GUI split is authored on the daemon; the layout re-push
    /// reconciles the new pane in. A split issued BY the reconciler itself (while
    /// realizing) is not re-authored — it builds the pane locally.
    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty, bool vertical) override
    {
        if (isRealizingLayout())
            return false;
        requestSplitPane(actingPty, vertical);
        return true;
    }

    /// SessionFactory: the user moved a divider, so the daemon's model — what a re-attaching client
    /// rebuilds from — is told the new ratio. @see proto::ResizeSplit.
    void reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio) override;

    /// SessionFactory: a GUI "new window" is authored on the daemon
    /// (requestCreateWindow); the daemon's new-window layout push maps it onto a
    /// fresh OS window. A window spawned BY the reconciler itself (while realizing) is
    /// not re-authored — it is the local half of a daemon window that already exists.
    [[nodiscard]] bool requestRemoteWindow() override
    {
        if (isRealizingLayout())
            return false;
        requestCreateWindow();
        return true;
    }

    /// SessionFactory: the user ended the pane backed by @p pty, so close it on the daemon too
    /// rather than leave the session running there with nothing showing it. The counterpart of
    /// `unbind()`, which merely forgets a binding and cannot tell why the pty went away.
    void requestRemoteClose(vtpty::Pty const* pty) override;

    /// SessionFactory: the terminal built around one of our ptys arrived, so the session's mirror
    /// can be created and primed with whatever the daemon has already told us about it.
    void bindTerminal(vtpty::Pty const* pty, vtbackend::Terminal& terminal) override;

  signals:
    /// A remote session appeared that has no local tab yet (fires on the
    /// reactor thread; connect queued).
    void remoteSessionDiscovered();

    /// The connection to the daemon ended (detach, daemon exit, error).
    void connectionClosed();

    /// The daemon's tab/pane layout changed (fires on the reactor thread; connect
    /// queued). The GUI reconstructs its tab/split tree from layout() (B2).
    void layoutChanged();

  private:
    /// One discovered remote session awaiting a local tab.
    struct PendingSession
    {
        uint64_t session = 0;
        uint32_t columns = 80;
        uint32_t lines = 25;
    };

    /// One remote session's local binding.
    ///
    /// The pty stays for INPUT and resize (keystrokes and geometry travel out through its sinks);
    /// remote output no longer flows through it, because the mirror writes the terminal's grid
    /// directly. The mirror is therefore absent until `bindTerminal` announces the terminal — a pty
    /// exists before the terminal built around it does.
    struct Binding
    {
        vtpty::ChannelPty* pty = nullptr;
        std::unique_ptr<vthost::client::ScreenMirror> mirror;
    };

  protected:
    /// The reactor's whole lifetime: connect, serve, notify. Takes the loop
    /// by pointer (coroutine reference parameters can dangle).
    [[nodiscard]] coro::Task<void> runClient(net::EventLoop* loop) override;

    // RemoteController hooks: the attach-specific half of the shared connect lifecycle.
    void detachOnReactor() override
    {
        if (_client != nullptr)
            _client->detach();
    }
    void closeReactorBindings() override { closeAllBindings(); }
    [[nodiscard]] std::string connectTimeoutMessage() const override
    {
        return "timed out waiting for the daemon's snapshot";
    }
    [[nodiscard]] std::string connectClosedMessage() const override
    {
        return "connection closed during attach";
    }

  private:
    /// Reactor-side: applies @p delta through the session's mirror (if bound).
    void onUpdate(vthost::client::RemoteScreen const& screen, vthost::proto::Delta const& delta);

    /// Reactor-side: an image's pixels arrived (or it was dropped), so the mirror can place it.
    void onImage(vthost::client::RemoteScreen const& screen, uint32_t imageId);

    /// Reactor-side: reproduces a bell / desktop notification / OSC 52 clipboard write on the
    /// session's mirror terminal, so the frontend's own handling and permissions apply.
    void onSessionEvent(vthost::client::RemoteScreen const& screen,
                        vthost::proto::SessionEventPdu const& event);

    /// Reactor-side: stores the daemon's latest tab/pane layout and signals the
    /// GUI to reconcile its own tree against it.
    void onLayout(vthost::proto::LayoutState const& layout);

    /// Reactor-side: feeds a fresh binding its full replay if the session's
    /// screen is already known.
    void primeBinding(uint64_t session);

    /// GUI-side (from a pane's pty resize sink, on whichever thread resized it): records the grid
    /// @p session's pane is now rendered at and schedules ONE coalesced geometry report.
    ///
    /// Coalescing matters: a window resize or a divider drag resizes every pane in the same layout
    /// pass, and reporting per pane would make the daemon re-project (and answer with a forced full
    /// snapshot) once per pane — on a partially-updated tree, since the other panes have not
    /// reported yet.
    /// @param session The remote session whose pane resized.
    /// @param cells The pane's new grid.
    void reportPaneGeometry(uint64_t session, vtpty::PageSize cells);

    /// Sends the pending geometry report (GUI thread): the composed client area if it moved, plus
    /// every bound pane whose grid the daemon does not already have.
    void flushGeometry();

    /// The client area composed from the pane tree the daemon last pushed and the grids its leaves
    /// are rendered at (@see vthost::client::composeClientArea). Prefers the tab holding the pane
    /// that reported last — it is the one on screen — and otherwise takes the first tab that fully
    /// resolves, in daemon-window then tab order, so the choice never depends on hash order.
    /// Must be called under BOTH _mutex (for the layouts) and _geometryMutex (for the pane grids).
    /// @return The composed area, or nullopt while no tab fully resolves.
    [[nodiscard]] std::optional<vtpty::PageSize> composedClientArea() const;

    /// GUI-side (from the pty's destructor): forgets a binding, and — while the connection is still
    /// live (not `_stopped`) — tombstones the session id so a remote session that outlives its local
    /// pane cannot resurrect it through the next delta.
    ///
    /// It does NOT decide whether the remote session should be closed: a pty is destroyed by a pane
    /// close, a window close, an app quit and a lost connection alike, and only the first of those
    /// means "end this session". That intent is authored upstream by whoever holds it
    /// (@see requestRemoteClose, contour::SessionEnd).
    void unbind(uint64_t session);

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllBindings();

    // The reactor and the connect state machine (_reactor, _mutex, _connected, _state, _failure,
    // _stopped) live in RemoteController; the registry below is attach-specific.
    vthost::AttachEndpoint _endpoint;
    /// This client's profile, stated in the ClientHello and applied by the daemon to the sessions
    /// this connection creates. Fixed at construction: it is what the user launched with.
    std::optional<vtbackend::Settings> _sessionSettings;
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
    /// The daemon's latest tab/pane tree per window id (B2/B4). Ordered so windowIds()
    /// yields a stable ascending order (the primary window is the lowest id).
    std::map<uint64_t, vthost::proto::LayoutState> _layouts;
    /// The remote session the NEXT createPty() binds to, set by the layout
    /// executor before each pane's backing session is created (GUI thread).
    std::optional<uint64_t> _nextBindSession;
    bool _realizingLayout = false; ///< canCreateSession() is true while a layout is being realized.
    /// Session ids this client is done with: a pane the user closed or ended, released while still
    /// attached. Their remote sessions can outlive the local pane by a round trip — the daemon has
    /// not honored the `ClosePane` yet, or never will because the pane was only detached from — and
    /// they keep emitting deltas meanwhile; `onUpdate` ignores tombstoned ids so a released pane
    /// never reappears.
    ///
    /// Never pruned, and that is sound rather than a leak: session ids are monotonic (never reused),
    /// so a lingering tombstone can never suppress a genuinely new session, the set is bounded by
    /// this connection's lifetime at 8 bytes an entry, and a reattach is a fresh controller with an
    /// empty set. The daemon's own removal notice arrives as a re-pushed `LayoutState`, which the
    /// reconciler acts on structurally rather than per id (@see applyRemoteLayout).
    std::unordered_set<uint64_t> _closedSessions;
    /// Guards the geometry bookkeeping below, and ONLY that.
    ///
    /// A lock of its own because the resize sink is reached from inside a `_mutex`-holding frame:
    /// `onUpdate`/`primeBinding` hold `_mutex` while the mirror writes the pane's terminal, and a
    /// terminal write can resize the screen (`setStatusDisplay` does, when the status line appears or
    /// goes) — which fires the pty's resize sink straight back into `reportPaneGeometry`. With one
    /// mutex guarding both the bindings and the geometry, that is a self-deadlock on a non-recursive
    /// mutex; the reactor thread simply stops.
    ///
    /// Lock ORDER where both are needed (`flushGeometry`): `_mutex` first, then this. Never the
    /// reverse — `reportPaneGeometry` takes this one alone precisely so the callback cannot close a
    /// cycle.
    mutable std::mutex _geometryMutex;
    /// The grid each bound pane is rendered at, keyed by remote session — seeded with the pane's
    /// birth size and updated by every resize report. The client area is composed from ALL of them,
    /// so a single pane's resize still yields the whole content area (a pane's own grid is what the
    /// daemon mistook for the client area, halving it on every split).
    std::unordered_map<uint64_t, vtpty::PageSize> _paneSizes;
    /// What was last sent upstream, so an unchanged report is dropped: a window resize fires the
    /// sink once per pane, and the daemon answers every accepted client area with a forced full
    /// snapshot per followed session.
    std::optional<vtpty::PageSize> _lastReportedArea;
    std::unordered_map<uint64_t, vtpty::PageSize> _lastReportedPaneSizes;
    /// The session whose pane last reported a grid; anchors which tab the client area is composed
    /// from, @see vthost::client::composeAnchoredClientArea. Outlives the individual flush it was
    /// set for — a report is not "consumed" — but never its pane: `unbind` clears it, so it either
    /// names a live pane or nothing.
    std::optional<uint64_t> _geometryAnchor;
    bool _geometryFlushScheduled = false;            ///< A coalesced flush is already queued.
    vthost::client::NativeClient* _client = nullptr; ///< Reactor-owned; valid while serving.
};

} // namespace contour::remote
