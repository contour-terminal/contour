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

#include <muxserver/client/AttachClient.h>
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
    /// @param socketPath The daemon's NATIVE-protocol socket path.
    explicit AttachController(std::filesystem::path socketPath);

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

    /// Creates local tabs (in @p window) for every discovered remote session
    /// that has no local tab yet — called once right after the first window
    /// booted. Leaves one session for the QML-created first tab if none is
    /// bound yet.
    void adoptStartupSessions(TerminalSessionManager& manager, vtmux::WindowId window);

    /// @return How many remote sessions await a local tab.
    [[nodiscard]] std::size_t pendingCount() const;

    // SessionFactory: hands out a ChannelPty bound to the next pending
    // remote session; cwd/command/profile do not apply to remote sessions.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override;

    [[nodiscard]] bool canCreateSession() const noexcept override;

  signals:
    /// A remote session appeared that has no local tab yet (fires on the
    /// reactor thread; connect queued).
    void remoteSessionDiscovered();

    /// The connection to the daemon ended (detach, daemon exit, error).
    void connectionClosed();

  private:
    class BoundChannelPty;

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

    std::filesystem::path _socketPath;
    MuxLoopThread _reactor;

    mutable std::mutex _mutex;
    std::condition_variable _connected;
    using State = MuxConnectPhase;
    State _state = State::Connecting;
    std::string _failure;
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
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
