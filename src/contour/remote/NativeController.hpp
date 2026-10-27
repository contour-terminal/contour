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
#include <contour/session/HostedOperation.hpp>
#include <contour/session/SessionFactory.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/ChannelPty.hpp>

#include <QtCore/QObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/latency/StartupTimeline.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::remote
{

/// The wire operation a hosted operation travels as: the conversion at the protocol boundary, which keeps
/// the session layer free of the protocol. The two enums name the same operations by the same values, as
/// the assertion below pins, so the conversion is a cast; a value no enumerator names travels as itself,
/// for the host to refuse.
/// @param operation The operation a session asks for.
/// @return Its proto::EngineOperation.
[[nodiscard]] constexpr vthost::proto::EngineOperation toWireOperation(
    contour::session::HostedOperation operation) noexcept
{
    return static_cast<vthost::proto::EngineOperation>(std::to_underlying(operation));
}

static_assert(
    [] {
        using contour::session::HostedOperation;
        using vthost::proto::EngineOperation;
        // One pair per operation: a HostedOperation added without the EngineOperation it travels as fails the
        // count, and one whose value differs fails the comparison.
        constexpr auto Pairs = std::array {
            std::pair { HostedOperation::ClearToPrompt, EngineOperation::ClearToPrompt },
            std::pair { HostedOperation::ClearHistoryAndReset, EngineOperation::ClearHistoryAndReset },
            std::pair { HostedOperation::SoftReset, EngineOperation::SoftReset },
            std::pair { HostedOperation::WriteScreen, EngineOperation::WriteScreen },
            std::pair { HostedOperation::SetUserMark, EngineOperation::SetUserMark },
            std::pair { HostedOperation::ClearUserMark, EngineOperation::ClearUserMark },
        };
        return Pairs.size() == contour::session::HostedOperationTable.size()
               && std::ranges::all_of(Pairs, [](auto const& pair) {
                      return std::to_underlying(pair.first) == std::to_underlying(pair.second);
                  });
    }(),
    "every HostedOperation travels as the proto::EngineOperation of its name and value");

/// The attach-mode session factory and remote-session registry.
class NativeController final: public QObject, public contour::session::SessionFactory, public RemoteController
{
    Q_OBJECT

  public:
    /// Resolves this client's profile in the configuration the GUI holds now: the emulation settings
    /// to ask the daemon for on the next session this client creates, or why they cannot be resolved.
    /// Called on the GUI thread.
    using SessionSettingsSource = std::function<std::expected<vtbackend::Settings, std::string>()>;

    /// @param transport How to reach the daemon: the local unix control socket, or a TLS-encrypted,
    ///        token-authenticated TCP endpoint. Opened afresh for every reconnect; its describe() names the
    ///        daemon in every log line and failure message.
    /// @param intent Which of the daemon's windows the first connection follows; every reconnect resumes the
    ///        windows the lost connection followed.
    /// @param profileName The host profile this client's sessions run, stated in every ClientHello; empty for
    ///        the daemon's default.
    /// @param attachSettings The emulation settings to state in the ClientHello — this client's own
    ///        resolved profile — or nullopt to take whatever the daemon hosts with.
    /// @param sessionSettings Asked before every session this client creates, so a session created
    ///        after a configuration reload gets the reloaded settings; empty to keep @p attachSettings.
    ///        When it fails, the settings last stated stay in force. Neither is defaulted, so a caller
    ///        with no preference says so.
    /// @param clock What the connect budget is measured on (@see RemoteController::awaitConnected).
    ///        Borrowed: it must outlive the controller.
    /// @param timeline Where the connect, the layout and the first session are stamped. Borrowed: it must
    ///        outlive the controller.
    NativeController(std::shared_ptr<vthost::ITransportFactory> transport,
                     vthost::proto::AttachIntent intent,
                     std::string profileName,
                     std::optional<vtbackend::Settings> attachSettings,
                     SessionSettingsSource sessionSettings,
                     core::platform::IClock const& clock,
                     vthost::latency::StartupTimeline& timeline);

    /// Stops the reactor (detaching if still connected) and joins.
    ~NativeController() override;

    NativeController(NativeController const&) = delete;
    NativeController& operator=(NativeController const&) = delete;
    NativeController(NativeController&&) = delete;
    NativeController& operator=(NativeController&&) = delete;

    // connectAndWait() and stop() are inherited from RemoteController; this
    // controller supplies the hooks below.

    /// @return The daemon's release as the ServerHello of the last accepted connection stated it; empty until
    ///         the handshake is through (valid after connectAndWait) and for a daemon that predates the
    ///         field.
    [[nodiscard]] std::string daemonRelease() const;

    /// @return What the handshake of the last accepted connection agreed on, captured as it completed; empty
    ///         until then.
    [[nodiscard]] std::optional<vthost::proto::Negotiated> negotiated() const;

    /// @return Where the connection to the daemon stands: Reconnecting while a lost one is sought again, its
    ///         bindings and ptys kept meanwhile. Safe from any thread.
    [[nodiscard]] vthost::client::ConnectionState connectionState() const;

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

    /// The daemon's row that the row @p row of @p session's mirror holds: what the daemon is asked to act on
    /// when the user acts on a row of the mirror. Call on the reactor, where the mirror is applied.
    /// @see vthost::client::ScreenMirror::hostRowOf
    /// @return The daemon's row, or std::nullopt when the mirror cannot name it.
    [[nodiscard]] std::optional<vthost::proto::RowAddress> hostRowOf(uint64_t session,
                                                                     contour::session::HostedRow row) const;

    /// Asks the daemon to create a new tab (B3-Qt). The daemon honors it and
    /// re-pushes its layout, which the GUI reconciles into a new local tab. A no-op
    /// once detached.
    /// @param actingPty A pty of the WINDOW the tab belongs in — the daemon hosts more than one
    ///        and resolves the window from the session this pty is bound to. Null or unbound
    ///        falls back to the daemon's first window.
    /// @param spawn What the tab's session runs: its profile, by the name the daemon resolves.
    void requestCreateTab(vtpty::Pty const* actingPty, contour::session::RemoteSpawn const& spawn);

    /// Asks the daemon to create a new window (B4). The daemon honors it and pushes
    /// that window's LayoutState; the GUI opens a matching OS window and reconciles
    /// the window's tabs into it. A no-op once detached.
    /// @param spawn What the window's first session runs: its profile, by the name the daemon resolves.
    void requestCreateWindow(contour::session::RemoteSpawn const& spawn);

    /// Asks the daemon to split the pane hosting the remote session bound to local
    /// pty @p actingPty (@p vertical orientation). The daemon honors it and
    /// re-pushes its layout, which reconciles into a local split. A no-op if the pty
    /// is not bound or the connection is gone.
    /// @param spawn What the new pane's session runs: its profile, by the name the daemon resolves.
    void requestSplitPane(vtpty::Pty const* actingPty,
                          bool vertical,
                          contour::session::RemoteSpawn const& spawn);

    /// Asks the daemon to close the pane hosting @p session (destroying that remote
    /// session). Sent when the user closes a mirror pane, so the session is really
    /// removed rather than left running headless. Asked while the connection is down, it is
    /// sent once the same daemon answers again, and dropped if another run of it does.
    void requestClosePane(uint64_t session);

    /// A status-line extension the daemon runs for a pane asks whether it may run its declared commands.
    struct HostedPermissionRequest
    {
        vtpty::Pty const* pty = nullptr;          ///< The pty of the pane to ask in.
        vthost::proto::PermissionRequest request; ///< What it asks, as the daemon sent it.
    };

    /// Takes the questions extensionPermissionRequested() announced, for the panes they are asked in. One
    /// for a session not bound to a pane yet stays, and is announced again once it is. GUI thread.
    /// @return The questions, oldest first.
    [[nodiscard]] std::vector<HostedPermissionRequest> takePermissionRequests();

    /// Answers a question takePermissionRequests() handed out: the daemon records the answer, for every
    /// session. A no-op once detached.
    /// @param hostId The installed extension that asked.
    /// @param digest The manifest digest the answer is bound to.
    /// @param answer The user's answer.
    void answerPermission(std::string hostId, std::string digest, vthost::GrantState answer);

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

    /// SessionFactory: every pane this factory makes mirrors a session the daemon hosts.
    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return contour::session::SessionHosting::Remote;
    }

    /// SessionFactory: a GUI "new tab" is authored on the daemon (requestCreateTab)
    /// rather than created locally; the daemon's layout re-push reconciles it in.
    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                        contour::session::RemoteSpawn const& spawn) override
    {
        requestCreateTab(actingPty, spawn);
        return true;
    }

    /// SessionFactory: a GUI split is authored on the daemon; the layout re-push
    /// reconciles the new pane in. A split issued BY the reconciler itself (while
    /// realizing) is not re-authored — it builds the pane locally.
    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty,
                                          bool vertical,
                                          contour::session::RemoteSpawn const& spawn) override
    {
        if (isRealizingLayout())
            return false;
        requestSplitPane(actingPty, vertical, spawn);
        return true;
    }

    /// SessionFactory: the user moved a divider, so the daemon's model — what a re-attaching client
    /// rebuilds from — is told the new ratio. @see proto::ResizeSplit.
    void reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio) override;

    /// SessionFactory: the user changed how a tab is presented, so the daemon's model -- and every other
    /// client -- is told: one proto::EditTab per field that differs from the last LayoutState this
    /// controller holds for the tab (vthost::client::tabEditsToReport). Dropped while realizing a layout,
    /// whose changes came from the daemon, and for a tab the daemon has not pushed.
    void reportTabPresentation(contour::session::LocalTabPresentation const& presentation) override;

    /// SessionFactory: a GUI "new window" is authored on the daemon
    /// (requestCreateWindow); the daemon's new-window layout push maps it onto a
    /// fresh OS window. A window spawned BY the reconciler itself (while realizing) is
    /// not re-authored — it is the local half of a daemon window that already exists.
    [[nodiscard]] bool requestRemoteWindow(contour::session::RemoteSpawn const& spawn) override
    {
        if (isRealizingLayout())
            return false;
        requestCreateWindow(spawn);
        return true;
    }

    /// SessionFactory: the user ended the pane backed by @p pty, so close it on the daemon too
    /// rather than leave the session running there with nothing showing it. The counterpart of
    /// `unbind()`, which merely forgets a binding and cannot tell why the pty went away.
    void requestRemoteClose(vtpty::Pty const* pty) override;

    /// SessionFactory: every pane's terminal mirrors one the daemon hosts, so an operation on the engine is
    /// sent there (proto::PerformOperation) and the mirror follows through its deltas. A pty that is not
    /// bound to a daemon session runs it here; without a connection every operation is Unreachable.
    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* pty, contour::session::HostedOperationRequest const& request) override;

    /// SessionFactory: the terminal built around one of our ptys arrived, so the session's mirror
    /// can be created and primed with whatever the daemon has already told us about it.
    /// The mirror also reports the command a restart stopped in the daemon's session to @p interruptions,
    /// posted to its context's (the session's, the GUI) thread.
    void bindTerminal(vtpty::Pty const* pty,
                      vtbackend::Terminal& terminal,
                      contour::session::InterruptionSink const& interruptions) override;

  signals:
    /// A remote session appeared that has no local tab yet (fires on the
    /// reactor thread; connect queued).
    void remoteSessionDiscovered();

    /// The connection to the daemon ended for good: the first connect failed, a reconnect was refused, or
    /// stop() ended it. A connection that is lost and sought again does not end it (@see
    /// connectionStateChanged).
    void connectionClosed();

    /// connectionState() changed (fires on the reactor thread; connect queued).
    void connectionStateChanged();

    /// After a reconnect: daemon windows this client showed that the daemon no longer has for it -- it
    /// restarted, or another client closed them -- for the GUI to close (fires on the reactor thread, ahead
    /// of the layoutChanged() of the windows that came instead; connect queued).
    /// @param daemonWindows The lost windows' ids, as the lost connection named them.
    void windowsLost(std::vector<uint64_t> daemonWindows);

    /// The daemon's tab/pane layout changed (fires on the reactor thread; connect
    /// queued). The GUI reconstructs its tab/split tree from layout() (B2).
    void layoutChanged();

    /// A daemon's status-line extension asks this client's user something (fires on the reactor thread;
    /// connect queued). The GUI takes the questions with takePermissionRequests().
    void extensionPermissionRequested();

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
    /// directly. The terminal is therefore absent until `bindTerminal` announces it — a pty exists
    /// before the terminal built around it does — and the mirror until the reactor primes it, since
    /// what it is built with (where the daemon runs) is the connection's, which lives there.
    struct Binding
    {
        /// Which binding this is, never reused: a reconnect to a restarted daemon detaches every binding,
        /// whose pty still lives until its window closes, and a new session may take the old one's id.
        /// What such a pty sends is matched by this, never by the session id alone.
        uint64_t serial = 0;
        vtpty::ChannelPty* pty = nullptr;
        vtbackend::Terminal* terminal = nullptr;
        std::unique_ptr<vthost::client::ScreenMirror> mirror;
        /// Where the session is told what a restart stopped in it; empty until bindTerminal().
        contour::session::InterruptionSink interruptions {};
        /// What @ref interruptions was last told, so only a change is posted. Reactor thread, under _mutex.
        std::string interruptionTold {};
    };

  protected:
    /// The reactor's whole lifetime: connect, serve, notify. Takes the loop
    /// by pointer (coroutine reference parameters can dangle).
    [[nodiscard]] core::async::Task<void> runClient(core::net::EventLoop* loop) override;

    // RemoteController hooks: the attach-specific half of the shared connect lifecycle.
    [[nodiscard]] bool detachOnReactor() override
    {
        if (_supervisor == nullptr)
            return false;
        _supervisor->stop();
        return true;
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
    /// Reactor-side, _mutex held: posts what @p binding's mirror knows of a restart's stopped command to its
    /// session, when that differs from what the session was last told.
    /// @param binding The binding whose mirror was just applied.
    static void tellInterruption(Binding& binding);

    /// Reactor-side: builds the client of one connection.
    /// @param loop The reactor's loop.
    /// @param connection The connection it runs over.
    /// @param intent What it asks to follow.
    [[nodiscard]] std::unique_ptr<vthost::client::NativeClient> makeClient(
        core::net::EventLoop& loop,
        std::unique_ptr<vthost::Connection> connection,
        vthost::proto::AttachIntent intent);

    /// Reactor-side: the supervisor's state changed.
    /// @param state Where the connection stands now.
    /// @param reason Why, for a lost, failed or refused one.
    void onConnectionState(vthost::client::ConnectionState state, std::string const& reason);

    /// Reactor-side, under _mutex: a reconnect was accepted. Judges which windows were lost, and makes the
    /// bindings ready for the new connection's snapshots: restarted mirrors when the same daemon answered,
    /// detached bindings when it restarted. The panes closed while the connection was down are closed on
    /// the daemon now when it is the same one, and forgotten when it restarted.
    void adoptReconnect();

    /// Lets the tests run code on the reactor thread, against the client of the moment, to stage a
    /// connection that is ending before its state change is delivered -- a window no daemon can be made to
    /// open on demand.
    friend struct NativeControllerTestAccess;

    /// Reactor-side: sends again the closes the client wrote nowhere because its connection was ending, for
    /// the panes @p layout still lists -- the daemon never saw them. Each goes out once.
    /// @param layout A window's layout the daemon just pushed.
    void replayUnsentCloses(vthost::proto::LayoutState const& layout);

    /// Reactor-side: closes @p session's pane on the daemon, or remembers to when no connection can take it.
    /// @param session The session whose pane the user closed.
    void closePaneOnReactor(uint64_t session);

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

    /// Reactor-side: keeps a daemon extension's question for the GUI, and tells it.
    void onPermissionRequest(vthost::proto::PermissionRequest const& request);

    /// Reactor-side: builds the mirror of a binding whose terminal arrived, judging once whether the
    /// daemon runs on the terminal's machine, and feeds it its full replay if the session's screen is
    /// already known.
    void primeBinding(uint64_t session);

    /// GUI-side (from a pane's pty resize sink, on whichever thread resized it): records the grid
    /// @p session's pane is now rendered at and schedules ONE coalesced geometry report.
    ///
    /// Coalescing matters: a window resize or a divider drag resizes every pane in the same layout
    /// pass, and reporting per pane would make the daemon re-project (and answer with a forced full
    /// snapshot) once per pane — on a partially-updated tree, since the other panes have not
    /// reported yet.
    /// @param session The remote session whose pane resized.
    /// @param serial The binding of that pane; a detached one's report is dropped.
    /// @param cells The pane's new grid.
    void reportPaneGeometry(uint64_t session, uint64_t serial, vtpty::PageSize cells);

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
    /// pane cannot resurrect it through the next delta. Only the binding @p serial names: a detached one's
    /// pty leaves alone the binding a new session of the same id has.
    ///
    /// It does NOT decide whether the remote session should be closed: a pty is destroyed by a pane
    /// close, a window close, an app quit and a lost connection alike, and only the first of those
    /// means "end this session". That intent is authored upstream by whoever holds it
    /// (@see requestRemoteClose, contour::SessionEnd).
    void unbind(uint64_t session, uint64_t serial);

    /// @return Whether @p session is bound, by the binding @p serial names. Takes _mutex.
    [[nodiscard]] bool isBinding(uint64_t session, uint64_t serial) const;

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllBindings();

    /// Resolves the session settings on the calling (GUI) thread, then posts @p create to the reactor
    /// thread, preceded by restating those settings when they changed, so the session it creates gets the
    /// configuration as it is now.
    /// @param create The creation verb to send.
    void postSessionCreation(std::function<void(vthost::client::NativeClient&)> create);

    // The reactor and the connect state machine (_reactor, _mutex, _connected, _state, _failure,
    // _stopped) live in RemoteController; the registry below is attach-specific.
    std::shared_ptr<vthost::ITransportFactory> _transport;
    vthost::proto::AttachIntent _intent; ///< What the first connection follows.
    std::string _profileName;            ///< Stated in every ClientHello.
    /// This client's profile as stated in the ClientHello; the daemon applies it to the sessions this
    /// connection creates.
    std::optional<vtbackend::Settings> _attachSettings;
    /// This client's profile as it is now, restated before every session this connection creates.
    SessionSettingsSource _sessionSettings;
    vthost::latency::StartupTimeline& _timeline;
    /// Filled, under _mutex, by the first callback after each accepted connection's ServerHello. @see
    /// negotiated()
    std::optional<vthost::proto::Negotiated> _negotiated;
    std::string _daemonRelease; ///< @see daemonRelease(). Guarded by _mutex.
    /// Takes the handshake's outcome from the client once it has one. Call under _mutex.
    void captureHandshake();
    /// Forgets what the daemon said of each session's echo, and tells every bound pane it is Unknown. Call
    /// under _mutex, on the reactor thread, while the connection is down.
    void forgetInputEchoes();
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
    /// What the daemon last said each session's terminal does with typed text (SessionInputEcho), kept so a
    /// pane bound after it arrived starts from it. Guarded by _mutex.
    std::unordered_map<uint64_t, vtpty::InputEcho> _inputEchoes;
    /// The daemon's latest tab/pane tree per window id (B2/B4). Ordered so windowIds()
    /// yields a stable ascending order (the primary window is the lowest id).
    std::map<uint64_t, vthost::proto::LayoutState> _layouts;
    /// What this client last reported for a tab, by the daemon's tab id, until the daemon's next layout
    /// push of that window says how it presents the tab now: a second report before then is compared with
    /// this, not with a layout that predates the first, so what was already sent is not sent again.
    /// Guarded by _mutex.
    std::map<uint64_t, vthost::client::TabPresentation> _reportedPresentations;
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
    /// empty set. A reconnect to a restarted daemon, whose ids count from 1 again, empties it. The daemon's
    /// own removal notice arrives as a re-pushed `LayoutState`, which the reconciler acts on structurally
    /// rather than per id (@see applyRemoteLayout).
    std::unordered_set<uint64_t> _closedSessions;
    /// The sessions whose panes the user closed while no connection was up, in the order closed: their
    /// ClosePane goes out once the same daemon answers again, and is dropped with the tombstones when
    /// another run of it does (@see adoptReconnect). Reactor-owned.
    std::vector<uint64_t> _pendingCloses;
    /// The sessions whose ClosePane was issued while the connection was ending, so nothing was written: sent
    /// again when the daemon of the same run (a non-zero instance on both sides, @see adoptReconnect) pushes
    /// a layout still listing them, and dropped when another run, or a legacy daemon that names none,
    /// answers. Reactor-owned.
    std::vector<uint64_t> _unsentCloses;
    /// The daemon extensions' questions the GUI has not taken yet, oldest first. Guarded by _mutex.
    std::vector<vthost::proto::PermissionRequest> _permissionRequests;
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
    /// The binding each of those panes belongs to (@see Binding::serial), so a detached pane's resize is not
    /// taken for the new session that has its id.
    std::unordered_map<uint64_t, uint64_t> _paneSerials;
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
    bool _geometryFlushScheduled = false; ///< A coalesced flush is already queued.
    /// Runs the connections; set, under _mutex, while runClient() runs. Reactor-owned.
    vthost::client::ClientSupervisor* _supervisor = nullptr;
    /// The client of the connection that is up, and null whenever none is: every verb sent to the daemon
    /// goes through it, so nothing is written to a connection still in its handshake, or to none.
    /// Reactor-owned; set and cleared under _mutex.
    vthost::client::NativeClient* _client = nullptr;
    vthost::client::ConnectionState _connection =
        vthost::client::ConnectionState::Connecting; ///< Under _mutex.
    /// Which run of the daemon the last accepted connection reached; 0 for unknown. Under _mutex.
    uint64_t _daemonInstance = 0;
    /// How many connections were accepted so far; reactor-owned.
    uint64_t _acceptedConnections = 0;
    /// The connection input may be written to: its number among the accepted ones, or 0 while none is up.
    /// Read by every pty's write sink, on whichever thread writes, without a lock -- the sink can be reached
    /// while the reactor holds _mutex -- and checked again on the reactor, so input typed while no
    /// connection was up is dropped, never replayed into whatever shell answers after a reconnect.
    std::atomic<uint64_t> _inputConnection = 0;
    std::atomic<std::size_t> _droppedInputBytes = 0; ///< Since the connection was lost; for the log.
    /// The windows the last reconnect lost, announced with the first layout that came instead. Under _mutex.
    std::vector<uint64_t> _lostWindows;
    uint64_t _nextBindingSerial = 1; ///< @see Binding::serial. Under _mutex.
};

} // namespace contour::remote
