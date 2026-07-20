// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `EventLoop` — the single-threaded coroutine driver for the async socket layer.
///
/// The loop owns the one blocking primitive (an injected @c EventSource) and
/// multiplexes fd readiness and timers over it. Flows (`coro::Task`s) suspend on
/// the awaitables the loop hands out — `waitReadable()`, `waitWritable()`,
/// `delay()` — and the pump resumes them when their source is ready.
///
/// Ported from Endo's TuiRuntime (see src/coro/README.md for provenance) with the
/// terminal-input and agent machinery removed, plus two additions the daemon
/// needs: finished spawned flows are reaped every pump (upstream accumulated
/// them until destruction), and a thread-safe @c post() backed by a @c SystemPipe
/// self-pipe lets other threads marshal work onto the loop thread AND break an
/// in-flight blocking wait — one mechanism for both.
///
/// Threading: all scheduler state is touched only on the loop thread. The sole
/// cross-thread surface is @c post().

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <net/EventSource.h>
#include <net/platform/Clock.h>
#include <net/platform/NativeHandle.h>
#include <net/platform/SystemPipe.h>

namespace net
{

class DelayAwaiter;
class WaitFdAwaiter;

/// Single-threaded cooperative scheduler driving coroutine flows over fd
/// readiness and timers.
///
/// Construct with an @c EventSource, `spawn` background flows and/or `blockOn`
/// a root flow; the pump runs on the calling thread until the root flow
/// completes.
class EventLoop
{
  public:
    /// @param source The multiplexed wait the pump drives (not owned; outlives the loop).
    /// @param clock The monotonic time source for timers and delays (not owned;
    ///        outlives the loop). Defaults to the process steady clock; tests
    ///        inject a @c ManualClock for deterministic timing.
    explicit EventLoop(EventSource& source, IClock& clock = defaultSteadyClock());

    EventLoop(EventLoop const&) = delete;
    EventLoop& operator=(EventLoop const&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    /// Cancels and unwinds any still-parked spawned flows before their frames are
    /// destroyed: requests stop, wakes every waiter, and drains the ready queue so
    /// parked awaiters resume, observe cancellation (OperationCancelled), and run
    /// their RAII cleanup. Members (including the spawned-flow storage) destruct
    /// afterward. A no-op when nothing is parked (the common case).
    ~EventLoop();

    /// Drives the pump until @p task completes, then returns its result.
    /// @param task The root flow to run (its frame is kept alive for the call).
    /// @return The value produced by @p task (or void).
    template <typename T>
    T blockOn(coro::Task<T> task)
    {
        task.handle().promise().setStopToken(_rootStop.get_token());
        _ready.push_back(task.handle());
        while (!task.done())
            pumpOnce();
        return task.result();
    }

    /// Starts a background flow that runs alongside the root flow. Its frame is
    /// kept alive by the loop and reclaimed on the pump after it completes.
    /// @param task The flow to run.
    void spawn(coro::Task<void> task);

    /// @return The number of spawned background flows whose frames are still held
    ///         (completed flows are reaped at the top of every pump).
    [[nodiscard]] std::size_t spawnedCount() const noexcept { return _roots.size(); }

    /// Enqueues @p callback to run on the loop thread and wakes the loop if it is
    /// blocked inside a wait. The ONLY EventLoop entry point that is safe to call
    /// from other threads; everything else must run on the loop thread (use post
    /// to get there).
    /// @param callback The work to run on the loop thread.
    void post(std::function<void()> callback);

    /// Requests cancellation of every flow and wakes all parked waiters so they
    /// unwind promptly via @c OperationCancelled. Must be called on the loop
    /// thread — from a signal handler or another thread, `post()` a call to it.
    void requestStop();

    /// @return The root cancellation source; `request_stop()` cancels every flow
    ///         (but does not wake parked waiters — prefer requestStop()).
    [[nodiscard]] coro::StopSource& rootStopSource() noexcept { return _rootStop; }

    /// @return The monotonic clock backing all timers and delays. Awaiters read
    ///         deadlines through this so tests can drive time deterministically
    ///         via an injected @c ManualClock.
    [[nodiscard]] IClock& clock() const noexcept { return _clock; }

    /// @param duration How long to suspend.
    /// @return An awaitable that resumes after @p duration elapses.
    [[nodiscard]] DelayAwaiter delay(std::chrono::milliseconds duration) noexcept;

    /// @param deadline The absolute instant (on this loop's clock) to resume at.
    /// @return An awaitable that resumes once the clock reaches @p deadline.
    [[nodiscard]] DelayAwaiter sleepUntil(SteadyTimePoint deadline) noexcept;

    /// Suspends until @p fd is readable (data, EOF, or HUP/ERR), without consuming
    /// any bytes — the caller then performs a non-blocking read.
    /// @param fd The native handle to wait on (must outlive the await).
    /// @return An awaitable resolving when @p fd is readable; throws
    ///         @c OperationCancelled if the flow is cancelled while parked.
    [[nodiscard]] WaitFdAwaiter waitReadable(NativeHandle fd) noexcept;

    /// Suspends until @p fd is writable (space available in the send buffer).
    /// @param fd The native handle to wait on (must outlive the await).
    /// @return An awaitable resolving when @p fd is writable; throws
    ///         @c OperationCancelled if the flow is cancelled while parked.
    [[nodiscard]] WaitFdAwaiter waitWritable(NativeHandle fd) noexcept;

    /// @name Awaiter-facing scheduler primitives (internal)
    /// Called by the loop's awaitables; not part of the consumer API.
    /// @{

    void scheduleTimer(SteadyTimePoint deadline, std::coroutine_handle<> waiter);

    /// Attaches @p fd to the event source for @p interest and parks @p waiter until
    /// it becomes ready. Several fd waiters may be parked concurrently (one per
    /// distinct fd), so they live in a map keyed by registration token.
    /// @param fd The native handle to wait on.
    /// @param interest The readiness to wait for (Read or Write).
    /// @param waiter The coroutine to resume on readiness or cancellation.
    /// @return The registration token (to detach on resume/cancel), or
    ///         @c FdToken::invalid() if the attach failed.
    [[nodiscard]] FdToken registerFdWaiter(NativeHandle fd,
                                           FdInterest interest,
                                           std::coroutine_handle<> waiter);

    /// Detaches @p token from the event source and drops its parked waiter, if any.
    /// Idempotent. Called by the awaiter on resume (ready or cancelled).
    /// @param token The registration to remove.
    void unregisterFdWaiter(FdToken token) noexcept;

    /// Re-queues @p waiter for resumption because its cancellation token fired while
    /// it was parked on a timer or fd. Used by the timed/fd awaiters' stop-callbacks
    /// so a `whenAny`/`withTimeout` loser parked on `delay`/`waitReadable` unwinds
    /// promptly instead of only when its deadline/fd eventually fires. The awaiter
    /// then observes stop_requested() in await_resume and throws OperationCancelled;
    /// its stale timer entry / fd registration is skipped (handle already done) or
    /// detached by the awaiter. Safe to call once per parked waiter.
    /// @param waiter The parked coroutine to resume for cancellation.
    void requeueForCancellation(std::coroutine_handle<> waiter);

    /// @}

  private:
    /// One scheduled timer: a deadline and the coroutine to resume at it.
    struct TimerEntry
    {
        SteadyTimePoint deadline;
        std::coroutine_handle<> handle;
    };

    /// Heap comparator placing the soonest deadline at the heap root (a min-heap
    /// over the standard max-heap, by reversing the comparison).
    /// @return True if @p a is later than @p b.
    [[nodiscard]] static bool soonestFirst(TimerEntry const& a, TimerEntry const& b) noexcept
    {
        return a.deadline > b.deadline;
    }

    /// Runs one iteration: reap finished spawns, run posted work, resume ready
    /// coroutines, then wait and route readiness.
    void pumpOnce();

    /// Destroys the frames of spawned flows that have completed. Upstream Endo
    /// only released them in the destructor, which is an unbounded leak for a
    /// long-lived loop spawning per-connection flows.
    void reapFinishedSpawns();

    /// Runs every callback handed to post() since the last drain, outside the lock.
    void runPostedCallbacks();

    /// Drains bytes from the post self-pipe after the source reported it readable.
    /// One bounded read per pump: if more wakeup bytes remain, the next wait
    /// reports the pipe readable again (level-triggered), so nothing is lost.
    void drainPostPipe();

    /// Resumes every coroutine currently in the ready queue.
    void drainReadyQueue();

    /// @return The timeout (ms) for the next wait: the soonest timer, or -1 if none.
    [[nodiscard]] int computeTimeoutMs() const;

    /// Moves expired timers' coroutines into the ready queue.
    void fireExpiredTimers();

    /// Resumes the coroutines parked on the fds reported ready by a wait, detaching
    /// each from the event source. Idempotent per token.
    /// @param tokens The ready fd tokens (readyRead or readyWrite from the outcome).
    void wakeFdWaiters(std::vector<FdToken> const& tokens);

    /// Wakes every parked flow so cancelled awaitables can unwind.
    void wakeAllWaiters();

    EventSource& _source;                       ///< The injected multiplexed wait.
    IClock& _clock;                             ///< The injected monotonic time source.
    std::deque<std::coroutine_handle<>> _ready; ///< Coroutines ready to resume now.
    std::vector<TimerEntry> _timers;            ///< Min-heap by deadline (soonest at front).
    std::unordered_map<FdToken, std::coroutine_handle<>>
        _fdWaiters;                       ///< Flows parked on a generic fd, by token.
    std::vector<coro::Task<void>> _roots; ///< Keeps live spawned background flows alive.
    coro::StopSource _rootStop;           ///< Root cancellation source.

    std::unique_ptr<SystemPipe> _postPipe;      ///< Self-pipe waking the source for post(); may be null.
    FdToken _postToken {};                      ///< The self-pipe's registration with the source.
    std::mutex _postMutex;                      ///< Guards _posted (the only cross-thread state).
    std::vector<std::function<void()>> _posted; ///< Callbacks awaiting the loop thread.
};

/// Awaitable that resumes after a delay (or throws on cancellation).
///
/// While parked it registers a stop-callback so that if its cancellation token is
/// stopped before the deadline (e.g. a `whenAny`/`withTimeout` sibling won), the
/// parked coroutine is re-queued promptly and unwinds via @c OperationCancelled,
/// rather than lingering until the deadline elapses. The stale timer entry is
/// skipped when it later fires (the handle is already done).
class DelayAwaiter
{
  public:
    DelayAwaiter(EventLoop& loop, SteadyTimePoint deadline) noexcept: _loop(loop), _deadline(deadline) {}

    [[nodiscard]] bool await_ready() const noexcept { return _deadline <= _loop.clock().now(); }

    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if constexpr (requires { awaiting.promise().stopToken(); })
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested())
            return false;
        _loop.scheduleTimer(_deadline, awaiting);
        _cancelReg.emplace(_token, [&loop = _loop, awaiting] { loop.requeueForCancellation(awaiting); });
        return true;
    }

    /// @throws OperationCancelled if the flow was cancelled while parked.
    void await_resume()
    {
        _cancelReg.reset();
        if (_token.stop_requested())
            throw coro::OperationCancelled {};
    }

  private:
    std::optional<coro::StopCallback<std::function<void()>>> _cancelReg;
    EventLoop& _loop;
    SteadyTimePoint _deadline;
    coro::StopToken _token;
};

/// Awaitable that resumes when a registered fd reaches a given readiness (Read or
/// Write), or throws @c OperationCancelled if the awaiting flow is cancelled while
/// parked. Returned by @c EventLoop::waitReadable / @c waitWritable.
///
/// Readiness is observed via the OS wait, so the awaiter is never ready before it
/// suspends: it always parks (after attaching the fd to the event source), and the
/// loop resumes it when the wait reports the fd ready. On resume — whether ready
/// or cancelled — it detaches the fd so the registration never outlives the await.
class WaitFdAwaiter
{
  public:
    /// @param loop The loop whose event source the fd is registered with.
    /// @param fd The native handle to wait on.
    /// @param interest The readiness to wait for (Read or Write).
    WaitFdAwaiter(EventLoop& loop, NativeHandle fd, FdInterest interest) noexcept:
        _loop(loop), _fd(fd), _interest(interest)
    {
    }

    /// Readiness is only known after the OS wait, so a valid fd never reports ready
    /// before suspending. An invalid fd resolves immediately (await_resume then
    /// reports cancellation), avoiding a pointless park on a handle that can never
    /// signal.
    [[nodiscard]] bool await_ready() const noexcept { return _fd == InvalidHandle; }

    /// Captures the cancellation token, then (unless already cancelled) attaches the
    /// fd and parks. Checks cancellation BEFORE registering so a cancelled flow
    /// resumes immediately without leaving a dangling registration.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if already cancelled or the attach failed; true to park.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if constexpr (requires { awaiting.promise().stopToken(); })
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested())
            return false;
        _registration = _loop.registerFdWaiter(_fd, _interest, awaiting);
        if (!_registration)
            return false; // attach failed: resume and surface the failure in await_resume
        // If the token is stopped while parked (a whenAny/withTimeout sibling won),
        // re-queue this coroutine promptly so it unwinds instead of waiting for the
        // fd to become ready (which may never happen).
        _cancelReg.emplace(_token, [&loop = _loop, awaiting] { loop.requeueForCancellation(awaiting); });
        return true;
    }

    /// Detaches the fd and, if the flow was cancelled while parked or the attach
    /// failed, reports cancellation.
    /// @throws OperationCancelled if cancelled while parked or the fd could not be attached.
    void await_resume()
    {
        _cancelReg.reset();
        if (_registration)
            _loop.unregisterFdWaiter(_registration);
        if (_token.stop_requested() || !_registration)
            throw coro::OperationCancelled {};
    }

  private:
    std::optional<coro::StopCallback<std::function<void()>>> _cancelReg;
    EventLoop& _loop;
    NativeHandle _fd;
    FdInterest _interest;
    FdToken _registration {};
    coro::StopToken _token;
};

} // namespace net
