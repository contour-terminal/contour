// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeController` — the GUI's native-protocol attach engine.
///
/// One controller = one connection to a `contour daemon`. It runs the
/// Qt-free `vthost::client::NativeClient` on its own reactor thread
/// (ReactorThread) and doubles as the app's `SessionFactory` while attached:
/// every locally created tab is backed by a `vtpty::ChannelPty` bound to one
/// remote session. Remote deltas re-serialize through a per-session
/// `ScreenMirror` and feed that pty — the session's ordinary parser thread
/// does the rest, so the display stack never learns the session is remote.
/// Input and resize flow back through the pty's sinks onto the reactor.

#include <contour/SessionFactory.h>
#include <contour/remote/ReactorThread.h>
#include <contour/remote/RemoteController.h>

#include <vtpty/ChannelPty.h>

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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vthost/Daemon.h>
#include <vthost/client/LayoutReconstruction.h>
#include <vthost/client/NativeClient.h>
#include <vthost/client/ScreenMirror.h>
#include <vtworkspace/Primitives.h>

namespace contour
{

class TerminalSessionManager;

/// The attach-mode session factory and remote-session registry.
class NativeController final: public QObject, public SessionFactory, public RemoteController
{
    Q_OBJECT

  public:
    /// @param endpoint How to reach the daemon: the local unix control socket, or
    ///        a TLS-encrypted, token-authenticated TCP endpoint.
    explicit NativeController(vthost::AttachEndpoint endpoint);

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
    void requestCreateTab();

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
    [[nodiscard]] bool requestRemoteTab() override
    {
        requestCreateTab();
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

    /// One remote session's local binding: the (terminal-owned) pty the
    /// mirror feeds, plus the mirror's reserialization state.
    struct Binding
    {
        vtpty::ChannelPty* pty = nullptr;
        vthost::client::ScreenMirror mirror;
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

    /// Reactor-side: stores the daemon's latest tab/pane layout and signals the
    /// GUI to reconcile its own tree against it.
    void onLayout(vthost::proto::LayoutState const& layout);

    /// Reactor-side: feeds a fresh binding its full replay if the session's
    /// screen is already known.
    void primeBinding(uint64_t session);

    /// GUI-side (from the pty's destructor): forgets a binding. When this is a
    /// user-initiated tab close (the connection is still live, i.e. not
    /// `_stopped`), it also tombstones the session id so a still-running remote
    /// session cannot resurrect the tab through its next delta.
    void unbind(uint64_t session);

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllBindings();

    // The reactor and the connect state machine (_reactor, _mutex, _connected, _state, _failure,
    // _stopped) live in RemoteController; the registry below is attach-specific.
    vthost::AttachEndpoint _endpoint;
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
    /// The daemon's latest tab/pane tree per window id (B2/B4). Ordered so windowIds()
    /// yields a stable ascending order (the primary window is the lowest id).
    std::map<uint64_t, vthost::proto::LayoutState> _layouts;
    /// The remote session the NEXT createPty() binds to, set by the layout
    /// executor before each pane's backing session is created (GUI thread).
    std::optional<uint64_t> _nextBindSession;
    bool _realizingLayout = false; ///< canCreateSession() is true while a layout is being realized.
    /// Session ids whose local tab the user closed while still attached. Their
    /// remote sessions live on (the native protocol has no close verb) and keep
    /// emitting deltas; `onUpdate` ignores tombstoned ids so a closed tab never
    /// reappears. Bounded by this connection's lifetime and freed with the
    /// controller: session ids are monotonic (never reused), so a lingering
    /// tombstone can never suppress a genuinely new session, and — absent any
    /// session-removed notification from the daemon — there is nothing to clear
    /// a single id against. A reattach is a fresh controller with an empty set.
    std::unordered_set<uint64_t> _closedSessions;
    vthost::client::NativeClient* _client = nullptr; ///< Reactor-owned; valid while serving.
};

} // namespace contour
