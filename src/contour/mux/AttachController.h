// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `AttachController` — the GUI's native-protocol attach engine.
///
/// One controller = one connection to a `contour daemon`. It runs the
/// Qt-free `muxserver::client::AttachClient` on its own reactor thread
/// (MuxLoopThread) and doubles as the app's `SessionFactory` while attached:
/// every locally created tab is backed by a `vtpty::ChannelPty` bound to one
/// remote session. Remote deltas re-serialize through a per-session
/// `ScreenMirror` and feed that pty — the session's ordinary parser thread
/// does the rest, so the display stack never learns the session is remote.
/// Input and resize flow back through the pty's sinks onto the reactor.

#include <contour/SessionFactory.h>
#include <contour/mux/MuxController.h>
#include <contour/mux/MuxLoopThread.h>

#include <vtpty/ChannelPty.h>

#include <QtCore/QObject>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <muxserver/Daemon.h>
#include <muxserver/client/AttachClient.h>
#include <muxserver/client/LayoutReconstruction.h>
#include <muxserver/client/ScreenMirror.h>
#include <vtmux/Primitives.h>

namespace contour
{

class TerminalSessionManager;

/// The attach-mode session factory and remote-session registry.
class AttachController final: public QObject, public SessionFactory
{
    Q_OBJECT

  public:
    /// @param endpoint How to reach the daemon: the local unix control socket, or
    ///        a TLS-encrypted, token-authenticated TCP endpoint.
    explicit AttachController(muxserver::AttachEndpoint endpoint);

    /// Stops the reactor (detaching if still connected) and joins.
    ~AttachController() override;

    AttachController(AttachController const&) = delete;
    AttachController& operator=(AttachController const&) = delete;
    AttachController(AttachController&&) = delete;
    AttachController& operator=(AttachController&&) = delete;

    /// Starts the reactor thread, connects, and blocks the calling thread
    /// until the handshake completed and the first session snapshot arrived.
    /// @param timeout How long to wait before giving up.
    /// @return Nothing on success; a human-readable reason on failure.
    [[nodiscard]] std::expected<void, std::string> connectAndWait(std::chrono::milliseconds timeout);

    /// Initiates a detach and joins the reactor thread. Idempotent.
    void stop();

    /// @return How many remote sessions await a local tab.
    [[nodiscard]] std::size_t pendingCount() const;

    /// @return True if @p session already has a local pane bound to it — used by
    ///         the incremental layout reconciler to skip tabs it already realized.
    [[nodiscard]] bool isBound(uint64_t session) const;

    /// @return The remote session bound to local pty @p pty, or nullopt. Lets the
    ///         reconciler map a GUI pane back to its daemon session (to split/close
    ///         the right pane).
    [[nodiscard]] std::optional<uint64_t> sessionForPty(vtpty::Pty const* pty) const;

    /// Asks the daemon to create a new tab (B3-Qt). The daemon honors it and
    /// re-pushes its layout, which the GUI reconciles into a new local tab. A no-op
    /// once detached.
    void requestCreateTab();

    /// Asks the daemon to split the pane hosting the remote session bound to local
    /// pty @p actingPty (@p vertical orientation). The daemon honors it and
    /// re-pushes its layout, which reconciles into a local split. A no-op if the pty
    /// is not bound or the connection is gone.
    void requestSplitPane(vtpty::Pty const* actingPty, bool vertical);

    /// Asks the daemon to close the pane hosting @p session (destroying that remote
    /// session). Sent when the user closes a mirror pane, so the session is really
    /// removed rather than left running headless.
    void requestClosePane(uint64_t session);

    /// @return The daemon's most recent tab/pane layout, or nullopt if none has
    ///         arrived yet. A thread-safe copy — the reactor thread updates it.
    ///         The GUI reconstructs its own tab/split tree from this (B2).
    [[nodiscard]] std::optional<muxserver::proto::LayoutState> layout() const;

    /// @return The captured daemon layout converted for `vtmux::realizeLayoutTab`
    ///         (an empty layout if none has arrived), plus its leaf→remote-session
    ///         map. The layout executor realizes this to reproduce the daemon tree.
    [[nodiscard]] muxserver::client::WireLayout wireLayout() const;

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
        muxserver::client::ScreenMirror mirror;
    };

    /// The reactor's whole lifetime: connect, serve, notify. Takes the loop
    /// by pointer (coroutine reference parameters can dangle).
    [[nodiscard]] coro::Task<void> runClient(net::EventLoop* loop);

    /// Reactor-side: applies @p delta through the session's mirror (if bound).
    void onUpdate(muxserver::client::RemoteScreen const& screen, muxserver::proto::Delta const& delta);

    /// Reactor-side: stores the daemon's latest tab/pane layout and signals the
    /// GUI to reconcile its own tree against it.
    void onLayout(muxserver::proto::LayoutState const& layout);

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

    muxserver::AttachEndpoint _endpoint;
    MuxLoopThread _reactor;

    mutable std::mutex _mutex;
    std::condition_variable _connected;
    using State = MuxConnectPhase;
    State _state = State::Connecting;
    std::string _failure;
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
    std::optional<muxserver::proto::LayoutState> _layout; ///< The daemon's latest tab/pane tree (B2).
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
    muxserver::client::AttachClient* _client = nullptr; ///< Reactor-owned; valid while serving.
    bool _stopped = false;
};

} // namespace contour
