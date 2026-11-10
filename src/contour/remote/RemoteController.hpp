// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Shared scaffolding for the GUI's mux controllers — the native-attach engine
/// (`NativeController`) and the tmux -CC mirror (`TmuxController`). Both run a
/// Qt-free client on a `ReactorThread` reactor and expose the same connection
/// handshake to the GUI thread. This header single-sources the parts that must
/// stay identical between them: the connection phase, the blocking
/// connect-and-wait, and the fallback pty handed out when a session must be born
/// with nothing pending.

#include <contour/remote/ReactorThread.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/Pty.hpp>

#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <vthost/AttachBudget.hpp>

namespace core::net
{
class EventLoop;
}

namespace contour::remote
{

/// A ChannelPty that runs an on-destroy callback when the owning terminal
/// destroys it — so each controller drops the pty's binding without its own
/// self-unbinding subclass. The callback (typically a controller's unbind,
/// keyed by the pane/session id) fires exactly once, from the destructor,
/// serialized against the controller by that unbind's own lock.
class SelfUnbindingChannelPty final: public vtpty::ChannelPty
{
  public:
    /// @param size The initial page size.
    /// @param writeSink Receives every chunk the terminal writes (input), or
    ///        empty to buffer writes.
    /// @param resizeSink Receives resize requests, or empty.
    /// @param onDestroy Invoked once from the destructor (e.g. controller.unbind(id)).
    SelfUnbindingChannelPty(vtpty::PageSize size,
                            vtpty::ChannelPty::WriteSink writeSink,
                            vtpty::ChannelPty::ResizeSink resizeSink,
                            std::function<void()> onDestroy):
        vtpty::ChannelPty(size), _onDestroy(std::move(onDestroy))
    {
        setWriteSink(std::move(writeSink));
        setResizeSink(std::move(resizeSink));
    }

    ~SelfUnbindingChannelPty() override
    {
        if (_onDestroy)
            _onDestroy();
    }

    SelfUnbindingChannelPty(SelfUnbindingChannelPty const&) = delete;
    SelfUnbindingChannelPty& operator=(SelfUnbindingChannelPty const&) = delete;
    SelfUnbindingChannelPty(SelfUnbindingChannelPty&&) = delete;
    SelfUnbindingChannelPty& operator=(SelfUnbindingChannelPty&&) = delete;

  private:
    std::function<void()> _onDestroy;
};

/// A mux controller's connection lifecycle. `awaitConnected` blocks until the
/// phase leaves Connecting; the reactor's serve-loop epilogue settles it to
/// Failed or Closed.
enum class MuxConnectPhase : uint8_t
{
    Connecting,   ///< The reactor started; no session/pane discovered yet.
    Ready,        ///< The first unit of remote structure arrived (handshake done).
    Failed,       ///< The connection could not be established.
    Closed,       ///< The connection ended after having been Ready.
    Reconnecting, ///< It had been Ready, was lost, and is being sought again; Ready once it is back.
};

/// The result of `awaitMuxConnect`: exactly one of ready/timedOut holds, or
/// neither (the phase reached Failed/Closed without a timeout).
struct MuxConnectOutcome
{
    bool ready = false;    ///< The phase reached Ready (and may have lost it again since: Reconnecting).
    bool timedOut = false; ///< The wait elapsed while still Connecting.
    std::string failure;   ///< The recorded failure reason (empty if none).
};

/// Blocks the calling (GUI) thread until @p phase leaves Connecting or @p timeout
/// elapses — the condition-variable handshake both controllers share. The caller
/// owns @p mutex / @p cv / @p phase / @p failure (its own members) and maps the
/// outcome to its own messages: on `timedOut` it must run its `stop()`.
/// @param mutex Guards @p phase and @p failure (locked internally).
/// @param cv Notified by the reactor on every phase transition.
/// @param phase The controller's connection phase.
/// @param failure The controller's failure reason (copied out when not Ready).
/// @param timeout How long to wait before giving up.
/// @return The outcome; see @c MuxConnectOutcome.
[[nodiscard]] MuxConnectOutcome awaitMuxConnect(std::mutex& mutex,
                                                std::condition_variable& cv,
                                                MuxConnectPhase const& phase,
                                                std::string const& failure,
                                                std::chrono::milliseconds timeout);

/// The dead-end pty a controller hands out when a session must be created but
/// none is pending (the creation guards should have prevented this). Sized to
/// @p pageSize or a sane default. Shared so both controllers behave identically.
/// @param pageSize The requested size, if any.
/// @return An unbound `ChannelPty` the session can drive and close cleanly.
[[nodiscard]] std::unique_ptr<vtpty::Pty> makeUnboundFallbackPty(std::optional<vtbackend::PageSize> pageSize);

/// The least awaitConnected() waits, however much of its budget the caller spent before asking: the
/// budget runs from startConnecting(), so a GUI boot stalled by something the daemon does not control
/// (a fontconfig rebuild, a wedged desktop portal) would otherwise report a healthy daemon as timed out.
/// A budget shorter than this is waited in full and no longer.
inline constexpr auto MinimumConnectAwait = std::chrono::milliseconds { 2000 };

/// Where a controller's run is in being stopped.
enum class StopPhase : uint8_t
{
    Running = 0, ///< Not stopped: connecting, up, or lost and being sought again.
    WindingDown, ///< Stopped, by a stop in the background whose thread is still waiting for the reactor.
    Stopped,     ///< Stopped, and the reactor joined.
};

/// How long stopMuxReactor lets a detached client end its connection before it cancels the loop:
/// the graceful close's own deadline plus room for the turns around it.
inline constexpr auto DefaultDetachBound = std::chrono::milliseconds { 3000 };

/// The reactor teardown both controllers share: moves @p phase from Running to Stopped exactly once
/// (guarded by @p mutex), posts @p detach onto the reactor so a live client
/// detaches cleanly, and waits up to @p detachBound for the root task to end by
/// itself -- so the connection closes in order, with close_notify over TLS, rather
/// than being cancelled mid-close. Only then does it request stop, and join. A task
/// still parked in connect has no client to detach, so @p detach itself should
/// request the stop there (RemoteController::stop does); the wait then ends as soon
/// as the task has unwound rather than at the bound.
/// @param mutex Guards @p phase (locked internally).
/// @param phase The controller's stop phase.
/// @param reactor The controller's reactor thread.
/// @param detach Posted onto the reactor to detach the live client/gateway.
/// @param detachBound How long the detach may take before the loop is cancelled.
/// @return True when this call performed the teardown; false when it was already
///         stopped, or being stopped — so the caller closes its own bindings only on the first stop.
[[nodiscard]] bool stopMuxReactor(std::mutex& mutex,
                                  StopPhase& phase,
                                  ReactorThread& reactor,
                                  std::function<void()> detach,
                                  std::chrono::milliseconds detachBound = DefaultDetachBound);

/// The second half of stopMuxReactor: waits up to @p detachBound for the root task a detach was posted to
/// to end by itself, cancels the loop if it did not, and joins the reactor thread.
/// @param reactor The controller's reactor thread.
/// @param detachBound How long the detach may take before the loop is cancelled.
void joinDetachedReactor(ReactorThread& reactor, std::chrono::milliseconds detachBound);

/// The connect state machine both GUI mux controllers share: the reactor thread plus the
/// connection-phase handshake, with `startConnecting()` / `awaitConnected()` / `stop()` implemented ONCE on
/// top of the free functions above. A concrete controller (native attach, tmux mirror) supplies its own
/// client lifetime, detach, binding teardown, and connect messages through the protected hooks; the
/// per-remote PENDING/BINDING registry stays in the derived controller, since the remote unit — a
/// native session vs a tmux pane — differs.
///
/// Threading & lifetime: `_reactor` and the sync state live here as protected members with the
/// SAME names the controllers already use. A derived controller MUST call `stop()` from its own
/// destructor (both do) so the reactor is joined while the derived vtable and members are still
/// alive — the hooks run on the reactor thread up to that join.
class RemoteController
{
  public:
    RemoteController(RemoteController const&) = delete;
    RemoteController& operator=(RemoteController const&) = delete;
    RemoteController(RemoteController&&) = delete;
    RemoteController& operator=(RemoteController&&) = delete;

    /// Starts the reactor on `runClient()` and returns at once, the connect running on the reactor
    /// while the caller goes on with its own work (the GUI loads QML meanwhile). Stamps the instant
    /// awaitConnected() counts its budget from. Idempotent: a second call does nothing.
    void startConnecting();

    /// Blocks the calling (GUI) thread until the handshake completes, fails, or the budget runs out.
    /// The budget counts from startConnecting(), not from this call: it waits for what remains of it,
    /// so the time the caller spent between the two is already spent -- but never less than
    /// `MinimumConnectAwait` (or the whole budget, if that is shorter). On expiry it stops and returns
    /// `connectTimeoutMessage()`; a non-ready close returns the recorded failure or
    /// `connectClosedMessage()`. Starts the connect first if startConnecting() has not.
    ///
    /// With an AttachBudget that has no connect bound, the connect phase is not timed here at all -- the
    /// transport keeps its own clock, which a prompt may pause -- and the handshake bound runs from the
    /// moment the transport handed out its connection (noteTransportReturned()).
    /// @param budget How long the attach may take (@see vthost::AttachBudget); a plain duration bounds the
    ///        whole of it, from startConnecting().
    /// @return Nothing on success; a human-readable reason on failure.
    [[nodiscard]] std::expected<void, std::string> awaitConnected(vthost::AttachBudget budget);

    /// The non-blocking form of awaitConnected(): looks at where the attach stands and returns at once, so
    /// the caller's event loop goes on serving whatever the connect needs it for (ssh's prompts are answered
    /// in a window on that thread). Starts the connect if startConnecting() has not. Counts the budget as
    /// awaitConnected() does, the first call being the earliest the caller could have started waiting.
    /// On expiry it stops, as awaitConnected() does.
    /// @param budget How long the attach may take (@see vthost::AttachBudget).
    /// @return Nothing while the attach is still going; else what awaitConnected() would return.
    [[nodiscard]] std::optional<std::expected<void, std::string>> pollConnected(vthost::AttachBudget budget);

    /// @param budget As for pollConnected().
    /// @return How long from now until pollConnected() would give the attach up, or none while that is not
    ///         known yet (no connect bound, and the transport has not returned its connection). The caller
    ///         arms a timer for it and looks again on the controller's own progress signals meanwhile.
    [[nodiscard]] std::optional<std::chrono::milliseconds> untilAttachDeadline(vthost::AttachBudget budget);

  private:
    /// @return When pollConnected() gives the attach up; none while not known yet.
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> attachDeadline(vthost::AttachBudget budget);

  public:
    /// startConnecting(), then awaitConnected(): for a caller with nothing to do meanwhile.
    /// @param timeout How long to wait before giving up.
    /// @return Nothing on success; a human-readable reason on failure.
    [[nodiscard]] std::expected<void, std::string> connectAndWait(vthost::AttachBudget timeout);

    /// Starts a new connect, on a fresh reactor, after the last one ended -- failed, was refused, or was
    /// stopped. The phase goes back to Connecting and the budget of awaitConnected() counts from now.
    /// A run that is still going must not be restarted: the caller asks first (@see isConnecting).
    void restartConnecting();

    /// Detaches the live client (`detachOnReactor`, posted onto the reactor) and joins the reactor
    /// thread. Idempotent; `closeReactorBindings()` runs once, on the first stop. Waits for a stop that
    /// stopInBackground() began, so it is what the destructor of a derived controller calls either way.
    void stop();

    /// Told once a stop that stopInBackground() began has ended: the reactor joined, the bindings closed.
    using Stopped = std::function<void()>;

    /// stop() without waiting for it: the detach is posted at once, and the wait for the daemon to see it
    /// -- up to the detach bound for one that stopped reading -- the join and `closeReactorBindings()` run
    /// on a thread of their own, for a caller (the GUI thread) that a stop must not hold up. The controller
    /// must live until then: stop(), the destructor's included, and restartConnecting() wait for it.
    /// @param stopped Told when the stop has ended, on the stopping thread; on this one, once that thread is
    ///        joined, when a stop had ended the run already; never for a second call while a background stop
    ///        is under way.
    void stopInBackground(Stopped stopped);

    /// @return Whether a stop that stopInBackground() began is still under way. Safe from any thread.
    [[nodiscard]] bool isWindingDown() const;

  protected:
    /// @param clock What awaitConnected() measures the budget spent since startConnecting() on.
    ///        Borrowed: it must outlive the controller.
    /// @param detachBound How long a stop lets a detached client end its connection in order before it
    ///        cancels the loop.
    explicit RemoteController(core::platform::IClock const& clock,
                              std::chrono::milliseconds detachBound = DefaultDetachBound):
        _clock(clock), _detachBound(detachBound)
    {
    }
    ~RemoteController() = default;

    /// The reactor's whole lifetime (connect, serve, notify); runs on the reactor thread.
    [[nodiscard]] virtual core::async::Task<void> runClient(core::net::EventLoop* loop) = 0;
    /// Posted onto the reactor by `stop()` to detach the live client/gateway.
    /// @return Whether there was one to detach. Without one -- a stop during connect -- `stop()`
    ///         cancels the loop at once instead of waiting out the detach bound for a root task
    ///         that nothing is about to end.
    [[nodiscard]] virtual bool detachOnReactor() = 0;
    /// Closes every bound pty on the FIRST stop (EOF to each backing session).
    virtual void closeReactorBindings() = 0;
    /// The failure message when the wait times out before the handshake completes.
    [[nodiscard]] virtual std::string connectTimeoutMessage() const = 0;
    /// The failure message when the connection closes before reaching Ready.
    [[nodiscard]] virtual std::string connectClosedMessage() const = 0;

    /// Settles the connection phase to Failed with @p reason and wakes whoever waits on it.
    ///
    /// The notify happens OUTSIDE the lock, and a phase that already reached Failed/Closed is left
    /// alone — a discipline each controller was re-deriving at its own transitions. Safe from any
    /// thread.
    /// @param reason What went wrong, verbatim into `_failure`.
    void failConnect(std::string reason);

    /// The reactor's run ended with an exception, on the reactor thread. The default settles the connection
    /// phase to Failed (@see failConnect); a controller that tells its owner when a connection ends
    /// overrides it to say so too, since nothing else will.
    /// @param reason The exception's message.
    virtual void reactorDied(std::string const& reason) { failConnect(reason); }

    /// @return Whether a run of the connection is in progress: connecting, up, or lost and being sought
    /// again,
    ///         and not stopped. Safe from any thread.
    [[nodiscard]] bool isConnecting() const;

    /// @return Whether stop() has ended the run and no restartConnecting() has begun another. Safe from any
    ///         thread.
    [[nodiscard]] bool isStopped() const;

    /// @return Whether the connection is up: the handshake completed, and neither the remote end nor a
    ///         stop() has ended it since. Safe from any thread; a request sent on a true answer can still
    ///         meet a connection that ends before it is written.
    [[nodiscard]] bool isConnected() const;

    using State = MuxConnectPhase;
    /// Told on the reactor thread, after the connection phase settled or the transport returned its
    /// connection: whoever polls the attach (pollConnected()) has something new to look at. Does nothing by
    /// default.
    virtual void notePhaseChange() {}

    /// Records that the transport has handed out its connection, so that the handshake bound of an
    /// AttachBudget runs from now. Called on the reactor thread, each time a connection is made.
    void noteTransportReturned();

    core::platform::IClock const& _clock;
    /// When pollConnected() was first called; empty until it was. GUI thread only.
    std::optional<core::platform::SteadyTimePoint> _pollStartedAt;
    /// When startConnecting() started the connect; empty until it has. GUI thread only.
    std::optional<core::platform::SteadyTimePoint> _connectStartedAt;
    ReactorThread _reactor;
    mutable std::mutex _mutex;          ///< Guards the phase/failure AND each controller's registry.
    std::condition_variable _connected; ///< Notified by the reactor on every phase transition.
    State _state = State::Connecting;
    std::string _failure;
    /// When the transport last handed out its connection; empty before. Guarded by _mutex.
    std::optional<core::platform::SteadyTimePoint> _transportReturnedAt;
    StopPhase _stopPhase = StopPhase::Running; ///< Guarded by _mutex.

  private:
    /// @return What a stop posts onto the reactor: the client's detach, or a cancel when there is none.
    [[nodiscard]] std::function<void()> detachOrCancel();

    /// Waits for a background stop's thread, if one was started. GUI thread.
    void awaitBackgroundStop();

    std::chrono::milliseconds _detachBound; ///< @see RemoteController()
    std::jthread _stopper; ///< The thread a background stop runs on. Declared last: joined first.
};

} // namespace contour::remote
