// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `EventLoop` — the single-threaded coroutine driver for the async socket layer.
///
/// The loop owns the one blocking primitive (an injected @c IoBackend) and multiplexes handle
/// readiness and deadlines over it. Flows (`async::Task`s) suspend on the awaitables the loop
/// hands out — `waitReadable()`, `waitWritable()`, `delay()` — and the turn resumes them when
/// what they wait on is ready. It implements @c core::async::IExecutor, so anything that can be
/// handed an executor — `ResumeOn`, `AsyncQueue`, a `Task` chain — can be handed a loop.
///
/// **The turn is a contract, and its order is load-bearing.** `runOnce` does exactly five things,
/// in this order, and the reason for each is written beside it in `EventLoop.cpp`:
///
///   1. swap the inbound queue: run posts, then resolve cancel requests by live @c ParkId;
///   2. drain the ready queue — **the one place a coroutine is resumed, and the one place a timer
///      callback is called**;
///   3. `clock.refresh()`, then compute the timeout;
///   4. `backend.wait(timeout)`;
///   5. `clock.refresh()`, then fire expired deadlines, FIFO by sequence.
///
/// **There is one deadline mechanism, not two.** `delay()` parks a coroutine and `addTimer()`
/// parks a callback, in the same table, on the same heap, with the same never-reused ids; step 5
/// fires both and step 2 runs both. Nothing polls, because the loop already knows its next
/// deadline and that is what bounds the wait in step 3.
///
/// Readiness dispatched in step 4 and deadlines fired in step 5 are RESUMED in the next turn's
/// step 2, which is what makes guarantee G2 — every resumption happens in turn step 2 — a thing
/// the loop can state rather than a thing each backend must be trusted with.
///
/// **Backends dispatch, the loop resumes.** A backend's wait invokes the callbacks on the
/// @c ReadinessHandler each park registers, and those callbacks only ENQUEUE. @c drainReadyQueue
/// asserts it, so a backend that ever resumed from inside its own ready-list walk fails with a
/// stack rather than corrupting the walk. Origin:
/// [fastcached#475](https://github.com/LASTRADA-Software/fastcached/issues/475).
///
/// Ported from contour's `net/EventLoop` (itself Endo's TuiRuntime) and merged with fastcached's
/// `Async/IReactor.hpp` at `0708dd54`: the turn structure, the park table, generation-checked
/// cancellation, the ordered teardown and the thread-affinity guarantees are fastcached's.
///
/// Threading: all scheduler state is touched only on the loop thread. The cross-thread surface is
/// @c post(), @c submit(), @c schedule(), @c requestCancel() and @c stop().

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/detail/CompletionHook.hpp>
#include <core/net/detail/ParkTable.hpp>
#include <core/net/detail/RingQueue.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/WorkerIdentity.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core::net
{

/// Thrown by @c WaitHandleAwaiter::await_resume when the handle could not be registered with the
/// backend (descriptor table exhausted, or a kernel that refused the interest). Distinct from
/// @c async::OperationCancelled so the caller can tell a plumbing failure from a deliberate
/// cancellation.
struct FdRegistrationFailed
{
    /// What the backend refused with. @c IoBackend::attach and @c IoBackend::setInterest both
    /// return the kernel's reason precisely so it is never swallowed, and a consumer debugging
    /// descriptor exhaustion needs to tell that apart from a filter the kernel would not arm.
    /// Default-constructed where the refusal carried none.
    NetError reason {};
};

/// How a flow parked on a handle is resumed when that handle closes.
///
/// A readiness poller cannot report a CLOSED descriptor: epoll drops it from the set and kqueue
/// drops its filters, both silently, so a parked flow would never be resumed at all (poll(2)
/// reports POLLNVAL and Windows reports the handle as failed, which is why those two backends
/// never had the bug). @c EventLoop::notifyHandleClosing is how a closing descriptor supplies
/// that missing readiness — and this says what the flow should observe once it wakes.
enum class FdWakePolicy : std::uint8_t
{
    /// Resume on the flow's normal path. For an explicit `close()`, where the owner is alive — it
    /// is the one that called close — so the flow can safely re-read the owner's closed flag and
    /// report the close as an error.
    Resume = 0,

    /// Resume by throwing @c async::OperationCancelled. For a destructor, where the owner is
    /// already gone: unwinding through @c await_resume never re-enters the flow body, so nothing
    /// dereferences the dead owner. This is the same reason ~EventLoop requests stop BEFORE
    /// moving its parked waiters to the ready queue.
    Cancel,
};

/// Why a parked handle waiter was resumed, reported back to the awaiter by
/// @c EventLoop::wakeReasonOf.
enum class FdWakeReason : std::uint8_t
{
    Ready = 0, ///< Ordinary readiness (or an explicit close under FdWakePolicy::Resume).
    Abandoned, ///< The handle closed under FdWakePolicy::Cancel; unwind instead of resuming.
};

/// What a turn does when it finds nothing to wait for.
///
/// A loop that owns its thread should BLOCK when it is idle — the backend's wake channel is what
/// ends that wait, and a loop that polled instead would spend a core doing nothing. A loop driven
/// a turn at a time by somebody else must not: the caller is what waits, and blocking inside a
/// turn would block them. Fixed at construction, because it is a property of who drives the loop
/// and that does not change halfway through.
enum class IdlePolicy : std::uint8_t
{
    Block = 0, ///< An idle turn waits on the backend until a wake or a deadline arrives.
    Return,    ///< An idle turn returns at once, having waited on nothing.
};

/// The loop's configuration, fixed at construction.
struct EventLoopOptions
{
    IdlePolicy idle = IdlePolicy::Block; ///< What an idle turn does.

    /// How many ready coroutines one turn resumes before it goes back round.
    ///
    /// A turn is BOUNDED so that work which re-queues itself — a flow yielding in a loop, a
    /// queue whose consumer immediately waits again — cannot starve the readiness and deadline
    /// steps. The remainder stays queued and the next turn takes it; nothing is dropped. At least
    /// 1: the constructor asserts it, because a turn that may resume nothing never empties the
    /// ready queue and so is never idle.
    std::size_t dispatchBatch = 64;

    /// A label for diagnostics and for a profiler's thread name. Borrowed: it must outlive the
    /// loop, which a string literal and a long-lived configuration string both do.
    std::string_view name = {};
};

/// What one turn of @c EventLoop::runOnce did.
struct RunOnceResult
{
    /// What step 2 took off the ready queue: coroutines resumed, plus timer callbacks run.
    ///
    /// **Named for the queue rather than for the verb**, because the verb is only true of half of
    /// it — a timer callback is called, not resumed — and one number has to cover both: a turn
    /// that ran a callback and resumed nothing is not an idle turn, and @c idle is derived from
    /// this, so a drain that stopped on "no coroutine resumed" would stop having just handed
    /// control to a callback that may have queued more work. It sits beside @c dispatched, which
    /// counts step 4 the same way.
    std::size_t drained = 0;

    std::size_t dispatched = 0; ///< Readiness reports the backend delivered in step 4.

    /// Whether this turn found nothing at all to do and left nothing for the next one: nothing
    /// posted, nothing drained, nothing dispatched, nothing due, and the ready queue empty when it
    /// returned. What @c runUntilIdle and @c testing::TestLoop::drain stop on. Work still PARKED
    /// does not count: a flow waiting on a handle that never becomes ready would otherwise keep a
    /// drain from ever returning.
    bool idle = false;
};

class DelayAwaiter;
class WaitHandleAwaiter;

/// Single-threaded cooperative scheduler driving coroutine flows over handle readiness and
/// deadlines, and an @c async::IExecutor.
///
/// Construct with an @c IoBackend, `spawn` background flows and/or `blockOn` a root flow; the
/// turn runs on the calling thread.
class EventLoop: public async::IExecutor
{
  public:
    /// @param backend The multiplexed wait the turn drives (not owned; outlives the loop, because
    ///        every registration this loop made is detached in ~EventLoop and not a moment later).
    /// @param clock The monotonic time source for deadlines (not owned; outlives the loop).
    ///        Defaults to the process steady clock; tests inject a @c platform::ManualClock for
    ///        deterministic timing. The loop calls its @c refresh() before it computes a wait's
    ///        timeout and after the wait returns, so a @c platform::CachedClock serves the turn.
    /// @param options The loop's configuration; see @c EventLoopOptions.
    explicit EventLoop(IoBackend& backend,
                       platform::IClock& clock = platform::defaultSteadyClock(),
                       EventLoopOptions options = {});

    EventLoop(EventLoop const&) = delete;
    EventLoop& operator=(EventLoop const&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    /// Tears the loop down in the one order that is safe. The steps, and why each is where it is,
    /// are in `EventLoop.cpp`; the short form is: assert teardown is serialised with dispatch,
    /// request stop and move every park to the ready queue, run bounded drain passes, abandon to
    /// a fixpoint, destroy the spawned roots, unregister the wake.
    ///
    /// **Objects registered with a loop are destroyed before it.** A socket, listener or dial
    /// that outlives its loop has a registration nothing will detach.
    ~EventLoop() override;

    /// Runs turns until @c stop() is called.
    ///
    /// **Non-virtual, and that is the obligation half of the teardown rule**
    /// ([fastcached#668](https://github.com/LASTRADA-Software/fastcached/issues/668)): it claims
    /// the worker identity and then turns, so a loop cannot answer @c running() without having
    /// entered here, and cannot enter here without answering it. A virtual `run` would let a
    /// derived loop forget to claim, and @c teardownIsSerialisedWithDispatch would then answer
    /// `true` unconditionally — the FALSE-SAFE direction, where every guard built on it stays
    /// green while checking nothing.
    ///
    /// @pre The backend is not host-driven. A host-driven loop does not own its thread: it
    ///      advances only through host pumps, and a `run()` there would spin forever without ever
    ///      letting the host deliver one. Asserted rather than compiled out, because the browser
    ///      reaches this through a consumer's mistake, not through ours.
    void run();

    /// Runs exactly one turn: the five steps in the order this file's header states.
    /// @param maxWait An upper bound on how long step 4 may wait, or nullopt for the loop's own
    ///        answer. A caller driving the loop beside something else passes zero.
    /// @return What the turn did.
    RunOnceResult runOnce(std::optional<platform::SteadyDuration> maxWait = std::nullopt);

    /// Runs turns until one of them is idle.
    /// @return How much every turn drained in all — coroutines resumed plus timer callbacks
    ///         run; see @c RunOnceResult::drained.
    std::size_t runUntilIdle();

    /// Drives turns until @p task completes, then returns its result.
    ///
    /// **It returns only when @p task is done.** An idle turn waits on the backend rather than
    /// giving up, because a flow that has hopped to another executor leaves this loop with
    /// nothing to see and is not finished — so a flow that `co_await ResumeOn { pool }`s and
    /// comes back completes here, which is the ordinary reason to have both a loop and a pool.
    /// The cost is that a flow nothing will ever advance HANGS, in the backend's wait, at no CPU.
    ///
    /// **On an @c IdlePolicy::Return loop it POLLS instead, and that is the same question
    /// answered the other way — stated here rather than eight lines away, because the two halves
    /// were previously separated and read as unrelated.** Such a loop is one somebody else drives
    /// a turn at a time, so @c computeTimeout forces a zero timeout and the wait above cannot
    /// block. A flow nothing can advance therefore SPINS there at full CPU rather than waiting
    /// at none: [core-cpp#17](https://github.com/contour-terminal/core-cpp/issues/17)'s exact
    /// shape, in the one configuration this design does not cover.
    ///
    /// @c testing::TestLoop forces that policy, so `testLoop.blockOn(flowParkedOnADeadline())`
    /// with a `ManualClock` nobody advances burns a core until ctest's backstop. **Drive such a
    /// loop with @c runOnce or @c runUntilIdle** and let the case decide when time passes. This
    /// is documented rather than asserted because a flow that finishes within a turn never
    /// reaches the idle wait at all, and refusing those would forbid the ordinary use.
    ///
    /// @pre The backend is not host-driven, for @c run()'s reason.
    /// @param task The root flow to run (its frame is kept alive for the call).
    /// @return The value produced by @p task (or void).
    /// @throws Whatever @p task's body threw, rethrown by @c async::Task::result(). An exception
    ///         escaping a TURN — a `post` callback, an allocation — propagates too, and on that
    ///         path @p task's frame is released rather than destroyed: it may be queued on
    ///         another executor, and freeing it would hand that executor dead storage.
    template <typename T>
    T blockOn(async::Task<T> task)
    {
        assert(!_backend.isHostDriven()
               && "EventLoop::blockOn on a host-driven loop: such a loop advances only through "
                  "host pumps, so blocking on it can never complete");
        auto const onWorker = detail::WorkerIdentity::Scope { _worker };
        task.handle().promise().setStopToken(_rootStop.get_token());
        // Through `queueReady` like every other queueing, rather than a `ReadyEntry` built here:
        // that function's claim to be the one place the ownership flag is decided is what the next
        // person will trust, and a second site writing the same value by hand is how the two
        // answers start to differ.
        queueReady(async::ParkedWork { .resume = task.handle() });
        // **The ONLY exit is `task.done()`.** An empty loop -- nothing queued, nothing parked,
        // nothing inbound -- does not mean the flow cannot advance: between `co_await ResumeOn {
        // pool }` and the `ResumeOn { loop }` that brings it back, the flow is running on
        // somebody else's thread and this loop can see none of it. A drive that gave up there
        // would answer about a flow that is running, and then destroy its frame under the thread
        // running it. So an idle turn WAITS, and the backend's wake channel is what ends the
        // wait: the hand-off calls `submit`, which wakes it.
        //
        // A flow nothing will ever advance therefore hangs -- parked in the backend's wait, at no
        // CPU cost, with one stack that says exactly that. That is worse than a diagnostic and
        // better than the alternatives: the loop cannot distinguish it from the case above
        // without knowing what every other thread intends, and both answers it could give
        // instead -- a value the flow never produced, or a throw about a flow that is running --
        // are wrong in the case the other one gets right.
        // **An exception out of a turn is an exit with the task unfinished**, and `turn()` is not
        // `noexcept`: a `TimerCallback` may throw by design, a posted `std::function` is user
        // code, and a resumed bare handle need not be a `Task`'s. On that path the root frame is
        // still NAMED by this loop -- a `Park` in the table, a `_byWaiter` entry keyed on its
        // address, an entry in the ready queue -- so destroying it leaves the next turn calling
        // `done()` on freed storage. Reachable with one `addTimer` whose callback throws under a
        // `blockOn` whose root is parked on a `delay`: single-threaded, no pool.
        //
        // `cancelPending` is the retrieval, and it searches all three containers and DISARMS
        // rather than releases -- so the frame stops being named and stays owned by `task`, which
        // then destroys it normally. `core::async::syncRunWith` leaks instead, for the principle
        // *a leak report names the coroutine that parked, and a use-after-free names nothing*;
        // it leaks because it has nothing that can retrieve the park. **Disarm beats leak where
        // disarming is available**, and here it is.
        //
        // What this cannot retrieve is a frame parked on something that is not this loop -- an
        // `AsyncQueue`, another executor. That is C1's case and it is why the drive waits rather
        // than giving up, above.
        {
            auto const undo = detail::ScopeGuard { [&]() noexcept {
                if (!task.done())
                    std::ignore = cancelPending(task.handle());
            } };
            while (!task.done())
                std::ignore = turn(std::nullopt, task.handle());
        }
        return task.result();
    }

    /// Asks @c run() to return once the current turn ends. Idempotent, and safe from any thread.
    void stop() noexcept;

    /// Requests cancellation of every flow and moves all parked waiters to the ready queue so
    /// they unwind promptly via @c async::OperationCancelled. Must be called on the loop thread —
    /// from a signal handler or another thread, `post()` a call to it.
    ///
    /// Called outside a turn, it asks the backend for one. A flow that registered a cancellation
    /// on this loop's token has already reached @c requestCancel by then, which wakes; a park
    /// filed through @c registerPark with nothing watching the token has not, and on a host-driven
    /// loop its unwind would wait for a turn that never comes.
    void requestStop();

    /// @return The root cancellation source; `request_stop()` cancels every flow (but does not
    ///         unpark its waiters — prefer @c requestStop()).
    [[nodiscard]] async::StopSource& rootStopSource() noexcept { return _rootStop; }

    /// Both overloads, so a call through an `EventLoop&` reaches the owning one. A derived class
    /// that re-declares one overload of a name hides every other overload of it, which is how
    /// seven parking sites upstream silently bound to the borrowing form
    /// ([fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041)).
    using async::IExecutor::submit;

    /// Queues @p handle for resumption on the loop thread. BORROWS: the caller guarantees the
    /// frame outlives the resumption. Safe from any thread.
    /// @param handle The coroutine to resume.
    void submit(std::coroutine_handle<> handle) override;

    /// Queues @p work for resumption on the loop thread, saying what may be freed if it never is.
    /// Safe from any thread.
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    void submit(async::ParkedWork work) override;

    /// Queues @p handle for resumption once the clock reaches @p deadline. BORROWS. Safe from any
    /// thread.
    /// @param deadline When to resume it.
    /// @param handle The coroutine to resume.
    void schedule(platform::SteadyTimePoint deadline, std::coroutine_handle<> handle);

    /// Queues @p work for resumption once the clock reaches @p deadline, saying what may be freed
    /// if the deadline never arrives. Safe from any thread.
    /// @param deadline When to resume it.
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    void schedule(platform::SteadyTimePoint deadline, async::ParkedWork work);

    /// Takes @p handle back off this loop while it is still waiting to be resumed.
    ///
    /// **Its result is an ownership transfer, not a status.** `true` means THIS call removed it,
    /// so the caller is now the only one who may resume or destroy it — and the claim on any
    /// chain the loop was holding is given back (disarmed) rather than released, or taking work
    /// off a loop would free what the caller has just been handed. `false` means the loop no
    /// longer had it — already resumed, or never here — and the caller must not touch it.
    ///
    /// It destroys rather than resumes, because a resume only queues on a loop that is about to
    /// stop. Must be called on the loop thread.
    /// @param handle A handle previously given to @c submit, @c schedule or an awaitable.
    /// @return Whether this call took it back.
    [[nodiscard]] bool cancelPending(std::coroutine_handle<> handle) noexcept;

    /// Enqueues @p callback to run on the loop thread, in step 1 of the next turn, and wakes the
    /// loop if it is blocked inside a wait. Safe from any thread.
    /// @param callback The work to run on the loop thread.
    void post(std::function<void()> callback);

    /// Starts a background flow that runs alongside the root flow. Its frame is kept alive by the
    /// loop and released once the flow completes -- in O(1), rather than by a later sweep over
    /// every spawned flow. The flow runs inside a root coroutine the loop owns (one more frame
    /// allocation per spawn), and the root's completion is what releases it, whichever frame of the
    /// flow was resumed last.
    ///
    /// **A flow may END on any thread** -- `co_await core::async::ResumeOn { pool }` and return,
    /// or throw, there. Ended on the loop's thread inside a turn, it is released in that turn's
    /// drain, right after the resume that ended it; ended anywhere else, it hands itself over
    /// through the inbound queue and is released in step 1 of the next turn, on the loop's thread.
    /// An exception that ends a flow ends it and goes no further, as it always has.
    ///
    /// **Loop thread only**, or before anything drives the loop. Unlike @c submit, this writes the
    /// loop's own containers directly and there is no inbound queue to hand it to: a spawn from an
    /// acceptor thread while a turn is running splices the `std::list` of roots underneath that
    /// turn. `post()` a call to it instead. It is the one
    /// difference from @c submit that migrating a flow to `spawn` — for the frame lifetime, which
    /// is why anyone does — does not otherwise announce.
    /// @param task The flow to run.
    void spawn(async::Task<void> task);

    /// Arms @p onExpired to run on the loop's thread once the clock reaches @p deadline.
    ///
    /// **A timer with no coroutine behind it, and no second scheduler behind that.** It is filed
    /// in the same park table as @c delay() and fired by the same turn step, so the loop has one
    /// answer to "when is the next deadline" and one firing order across both kinds. Nothing
    /// polls: an armed timer is what bounds the next wait, which is why this can replace a
    /// `DeadlineTimer` that woke every 50ms to notice it had been disarmed.
    ///
    /// The callback runs in turn step 2 — the turn AFTER the one whose step 5 found it due, which
    /// is the same one-turn latency a `co_await delay()` has, and for the same reason (guarantee
    /// G2: exactly one place resumes, and exactly one place hands control to code outside the
    /// loop). A deadline already in the past therefore fires on a turn rather than from this call,
    /// so a caller is never re-entered from its own arming.
    ///
    /// **Arming outside a turn asks the backend for one**, inherited from @c registerPark rather
    /// than computed here. On a host-driven loop that is the only thing that will: the host is
    /// armed at the end of a turn, and a quiescent host-driven loop has nothing scheduled that
    /// would start one — so without it the park would be filed and silently never fired. Inside a
    /// turn it is skipped, because the turn arms the host itself.
    ///
    /// Loop thread only, like @c registerPark and @c resumeSoon — and all three ask for that turn.
    /// From another thread, `post()` a call to it.
    /// @param deadline When to run @p onExpired, on this loop's clock.
    /// @param onExpired What to run; must not be null.
    /// @param state An opaque pointer handed to @p onExpired. Borrowed: it must outlive the timer,
    ///        or the timer must be cancelled before it goes.
    /// @return The timer's id, for @c cancelTimer, or @c TimerId::invalid() if @p onExpired was
    ///         null.
    [[nodiscard]] TimerId addTimer(platform::SteadyTimePoint deadline, TimerCallback onExpired, void* state);

    /// Retires @p timer so its callback does not run.
    ///
    /// **`true` means THIS call prevented the callback**, which is the only useful reading: it
    /// also covers the window between step 5 finding a timer due and step 2 running it, where the
    /// timer is queued but has not fired. Without that, an owner destroyed in that window — a
    /// @c DeadlineTimer is typically a member of the thing its callback touches — would have its
    /// callback run against storage that is gone, and "already fired" would mean "no longer
    /// cancellable and not yet harmless".
    ///
    /// `false` means there was nothing to prevent: the callback has already run, the timer was
    /// cancelled before, or @p timer never named a timer of this loop. Generation-checked, so a
    /// stale id can never retire a timer armed since — ids are never reused.
    ///
    /// Idempotent, and loop thread only.
    /// @param timer The timer to retire.
    /// @return Whether this call is what stopped the callback from running.
    [[nodiscard]] bool cancelTimer(TimerId timer) noexcept;

    /// @return The number of spawned background flows whose frames are still held.
    [[nodiscard]] std::size_t spawnedCount() const noexcept { return _roots.size(); }

    /// @return The number of parks waiting on a deadline. A cancelled park detaches its entry, so
    ///         a leaked entry after a `whenAny`/`withTimeout` loser unwinds is observable as a
    ///         nonzero count — the invariant the cancellation path must preserve.
    [[nodiscard]] std::size_t pendingTimerCount() const noexcept { return _parks.timerCount(); }

    /// @return How many slots the deadline heap holds, live and stale together. A diagnostic: the
    ///         difference from @c pendingTimerCount is exactly what lazy pruning is carrying, and
    ///         `Timers_test.cpp` measures it rather than arguing about it.
    [[nodiscard]] std::size_t pendingTimerSlotCount() const noexcept { return _parks.timerSlotCount(); }

    /// @return The number of readiness parks the loop still holds — one per park with a handle
    ///         key, which is NOT the same as one per backend registration: a park detached at
    ///         close or cancel stays here until it is taken. See @c detail::ParkTable::
    ///         readinessCount. The same invariant as @c pendingTimerCount, for the other kind of
    ///         park.
    [[nodiscard]] std::size_t parkedWaiterCount() const noexcept { return _parks.readinessCount(); }

    /// @return How many coroutines are queued for the next drain. Nonzero after a turn means the
    ///         turn's batch bound was reached. The ready queue only: what another thread -- or this
    ///         one, outside a turn -- submitted waits in the inbound queue until turn step 1, and
    ///         @c inboundSubmissionCount counts that.
    [[nodiscard]] std::size_t readyCount() const noexcept;

    /// @return How many coroutines were submitted from off the loop's worker thread and are waiting
    ///         for turn step 1 to queue them. Read under the inbound lock, so any thread may ask.
    [[nodiscard]] std::size_t inboundSubmissionCount() const
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        return _inbound.submissions.size();
    }

    /// @return How many deadlines were scheduled from off the loop's worker thread and are waiting
    ///         for turn step 1 to arm them; @c pendingTimerCount counts them once armed. Read under
    ///         the inbound lock, so any thread may ask.
    [[nodiscard]] std::size_t inboundScheduledCount() const
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        return _inbound.scheduled.size();
    }

    /// @return How many spawned flows ended off the loop's thread and wait for turn step 1 to
    ///         release them; @c spawnedCount still counts them until then. Read under the inbound
    ///         lock, so any thread may ask.
    [[nodiscard]] std::size_t inboundFinishedRootCount() const
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        return _inbound.finishedRoots.size();
    }

    /// @return The monotonic clock backing every deadline. Awaiters read deadlines through this so
    ///         tests can drive time deterministically via an injected @c platform::ManualClock.
    [[nodiscard]] platform::IClock& clock() const noexcept { return _clock; }

    /// @param duration How long to suspend.
    /// @return An awaitable that resumes after @p duration elapses.
    [[nodiscard]] DelayAwaiter delay(platform::SteadyDuration duration) noexcept;

    /// @param deadline The absolute instant (on this loop's clock) to resume at.
    /// @return An awaitable that resumes once the clock reaches @p deadline.
    [[nodiscard]] DelayAwaiter sleepUntil(platform::SteadyTimePoint deadline) noexcept;

    /// Suspends until @p handle is readable (data, EOF, or HUP/ERR), without consuming any bytes
    /// — the caller then performs a non-blocking read.
    /// @param handle The native handle to wait on (must outlive the await).
    /// @param kind What @p handle is. It matters where a platform has more than one kind of
    ///        waitable object: on Windows a SOCKET and a waitable HANDLE reach different wait
    ///        primitives, and the backend cannot tell them apart from the value alone.
    /// @return An awaitable resolving when @p handle is readable; throws
    ///         @c async::OperationCancelled if the flow is cancelled while parked.
    [[nodiscard]] WaitHandleAwaiter waitReadable(platform::NativeHandle handle,
                                                 HandleKind kind = DefaultHandleKind) noexcept;

    /// Suspends until @p handle is writable (space available in the send buffer).
    /// @param handle The native handle to wait on (must outlive the await).
    /// @param kind What @p handle is; see @c waitReadable.
    /// @return An awaitable resolving when @p handle is writable; throws
    ///         @c async::OperationCancelled if the flow is cancelled while parked.
    [[nodiscard]] WaitHandleAwaiter waitWritable(platform::NativeHandle handle,
                                                 HandleKind kind = DefaultHandleKind) noexcept;

    /// Announces that @p handle is ABOUT TO BE CLOSED, so any flow parked on it is resumed
    /// instead of waiting forever for readiness that can no longer arrive.
    ///
    /// Call this BEFORE the `close()` syscall, on the loop thread: the handle must still be valid
    /// so the backend can drop its kernel registration cleanly. Deferring that to the awaiter's
    /// own detach would issue the removal against a descriptor number the kernel may already have
    /// handed to a new socket, silently unregistering that one instead.
    ///
    /// The wake is only RECORDED here and delivered by the next turn as ordinary readiness. It is
    /// deliberately not queued for resumption from this call: a coroutine queued outside the turn
    /// still has its cancellation callback armed, so a later `requestStop()` would queue it a
    /// second time and the turn would then resume a frame the first resume had already destroyed.
    /// Not @c noexcept, though every caller is: recording a wake appends to a vector, so
    /// allocation failure propagates as termination from a `close()` that cannot report it.
    ///
    /// **It is also what ends a registration kept for the handle's life**
    /// (@c RegistrationLifetime::UntilClosed): the loop detaches it here, while the descriptor
    /// still names this handle, and nothing else ever does.
    /// @param handle The handle about to be closed.
    /// @param policy How a flow parked on @p handle should observe the close.
    void notifyHandleClosing(platform::NativeHandle handle, FdWakePolicy policy);

    /// @return Whether a thread is currently inside a turn of this loop. False before the first
    ///         turn and after the last one returns, which is the honest answer: with nothing
    ///         dequeuing there is no worker thread to be on.
    [[nodiscard]] bool running() const noexcept { return _worker.running(); }

    /// @return Whether the calling thread is the one currently driving this loop.
    [[nodiscard]] bool isOnWorkerThread() const noexcept { return _worker.isOnWorkerThread(); }

    /// Whether an object this loop owns may be destroyed right now, on this thread.
    ///
    /// **This is the rule, and the two queries above exist only to express it** (guarantee G5).
    /// Clearing a pending awaitable races the readiness dispatch, so a socket or listener
    /// belonging to a loop is destroyed either on that loop's worker thread — where no dispatch
    /// can be running concurrently, because dispatching is what that thread is doing — or with
    /// the loop stopped, where there is nothing to race. Any other thread, while a turn has not
    /// returned, is the violation.
    ///
    /// Non-virtual on purpose: one rule derived from two facts, in one place, so a loop can
    /// answer the facts and cannot restate the rule differently. Origin:
    /// [fastcached#668](https://github.com/LASTRADA-Software/fastcached/issues/668).
    /// @return True when destruction here is serialised against readiness dispatch.
    [[nodiscard]] bool teardownIsSerialisedWithDispatch() const noexcept
    {
        return !running() || isOnWorkerThread();
    }

    /// The completion port behind this loop's backend, or nullptr on a readiness backend.
    ///
    /// **What a socket factory asks to decide which socket to build**, and the only thing about
    /// the backend it needs: a port means "issue an overlapped operation and be completed", no
    /// port means "park on readiness and then call the syscall". It lends the PORT and not the
    /// backend, for the reason @c ICompletionPort is narrow: a socket that could reach the
    /// backend could dequeue it, and one thread dequeues a loop (G1).
    /// @return The port, valid for the backend's lifetime, or nullptr.
    [[nodiscard]] ICompletionPort* completionPort() noexcept { return _backend.completionPort(); }

    /// @name Awaiter-facing scheduler primitives
    /// Called by the loop's own awaitables and by the socket layer built on it.
    /// @{

    /// Queues @p work for resumption in the next drain. It ENQUEUES — it never resumes.
    ///
    /// **A readiness completion resumes its waiter before anything queued after the readiness
    /// callback.** Called from a callback the drain step runs -- a readiness park's owner, a timer
    /// -- @p work resumes immediately after that callback returns, in the same drain, ahead of
    /// whatever was queued behind the callback, still on the loop's thread and never inside the
    /// callback. Called from anywhere else it joins the back of the queue.
    ///
    /// **Loop thread only, and the assert enforces it.** An earlier version of this line named
    /// "thread-pool callback" among its callers, which the assert aborts: a pool thread must use
    /// @c submit(async::ParkedWork), which is this operation plus the hand-off through the
    /// inbound queue. The two are the same operation with different thread affinity, so a doc
    /// advertising the laxer one's callers on the stricter one is how the next off-thread caller
    /// gets written. What legitimately reaches this is the loop's own awaiters, a backend's
    /// readiness dispatch and a stop callback resolved on the loop thread.
    ///
    /// Called outside a turn, it asks the backend for one, for @c registerPark's reason: queued
    /// work on a quiescent host-driven loop has nothing coming that would drain it.
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    void resumeSoon(async::ParkedWork work);

    /// A frame-free operation's completion, and nothing else: see @c resumeCompleted.
    friend void detail::resumeSoonOn(EventLoop& loop,
                                     std::coroutine_handle<> waiter,
                                     std::coroutine_handle<> unownedRoot) noexcept;

    /// Parks @p entry: registers its handle with the backend if it names one, arms its deadline if
    /// it has one, and files it so a cancel can find it by id.
    ///
    /// A handle parked on with @c RegistrationLifetime::UntilClosed is registered once, by the
    /// first such park, and every later one only takes a slot on that registration -- arming it
    /// further if it is not yet armed for what the park watches, and otherwise asking the backend
    /// for nothing.
    ///
    /// **Called outside a turn, it also asks the backend for the turn that will reach the park.**
    /// This is where that arming lives for every park — @c addTimer, @c schedule, `delay()` and
    /// `interruptibleSleepUntil()` all inherit it from here rather than each computing it — and on
    /// a host-driven loop it is the only thing that will supply one: the host is armed at the END
    /// of a turn, so a park filed while the loop is quiescent would otherwise be correct, filed
    /// and silently never fired. A backend that is not host-driven is unaffected.
    /// @param entry What to park; see @c ParkEntry.
    /// @param refusal Where the backend's reason is written when the registration is refused, or
    ///        null where the caller has nowhere to report it.
    ///
    ///        **An out-parameter, which this project's own rule says a fallible API should not
    ///        be** — `std::expected` is the form everywhere else. The deviation is deliberate and
    ///        recorded here rather than left to be rediscovered: the return is a @c ParkId and
    ///        the great majority of calls cannot be refused this way at all (a deadline has no
    ///        handle), so `expected<ParkId, NetError>` would put a `.value()` or a monadic chain
    ///        on every one of them to carry a reason only the readiness path can produce. If a
    ///        second fallible reason ever appears here, that trade stops holding and the return
    ///        type should change.
    /// @return The park's id, or @c ParkId::invalid() if the backend refused the registration —
    ///         which it does for an invalid handle, and for a kernel that would not arm the
    ///         interest. A refusal leaves nothing registered.
    [[nodiscard]] ParkId registerPark(ParkEntry entry, NetError* refusal = nullptr);

    /// Detaches @p park from the backend and drops it, if it is still there. Idempotent. Called by
    /// the awaiter on resume, whether ready or cancelled.
    /// @param park The park to remove.
    void unregisterPark(ParkId park) noexcept;

    /// @param park The park to ask about.
    /// @return @c FdWakeReason::Abandoned if the handle was closed under @c FdWakePolicy::Cancel
    ///         while this waiter was parked on it — the awaiter then unwinds instead of resuming
    ///         into an owner that is gone — and @c FdWakeReason::Ready otherwise. Consumes the
    ///         mark: the awaiter asks exactly once, and a mark left behind would outlive its park.
    [[nodiscard]] FdWakeReason wakeReasonOf(ParkId park) noexcept;

    /// Asks that the flow parked at @p park be unparked and resumed so it can observe its
    /// cancellation. **Safe from any thread**, which is what a stop callback needs: a token may be
    /// stopped from a signal handler, a watchdog or a peer's thread.
    ///
    /// Generation-checked: a request naming a park that has already resumed resolves to nothing,
    /// because ids are never reused. From another thread it goes through the inbound queue and is
    /// resolved in step 1 of the next turn; from the loop's own thread it is resolved here,
    /// because the owner of a cancelled flow commonly destroys its frame the moment control
    /// returns to it — a `whenAny` loser is freed as soon as the winner returns — and a park left
    /// live until the next turn would then name freed storage.
    /// @param park The park to cancel.
    void requestCancel(ParkId park) noexcept;

    /// @}

  private:
    class SpawnedRoot;

    /// A deadline handed over from another thread, waiting to be armed on the loop's own.
    struct TimedWork
    {
        platform::SteadyTimePoint deadline {}; ///< When to resume it.
        async::ParkedWork work {};             ///< What to resume, and what to free if it is not.
    };

    /// What another thread has handed the loop, waiting for step 1.
    struct Inbound
    {
        std::vector<std::function<void()>> posts;   ///< Callbacks to run on the loop thread.
        std::vector<async::ParkedWork> submissions; ///< Coroutines to queue for the next drain.
        std::vector<TimedWork> scheduled;           ///< Deadlines to arm.
        std::vector<ParkId> cancels;                ///< Parks to unpark and resume for cancellation.
        /// Spawned flows that ended off the loop's thread, to release on it.
        std::vector<std::list<SpawnedRoot>::iterator> finishedRoots;

        /// @return Whether anything is waiting.
        [[nodiscard]] bool empty() const noexcept
        {
            return posts.empty() && submissions.empty() && scheduled.empty() && cancels.empty()
                   && finishedRoots.empty();
        }
    };

    /// The turn, with the one extra question @c blockOn has to be able to ask.
    /// @param maxWait An upper bound on how long step 4 may wait, or nullopt for the loop's own
    ///        answer.
    /// @param until A flow this drive exists to finish, or an empty handle. Once it is done the
    ///        turn skips step 4, for the reason written there.
    /// @return What the turn did.
    RunOnceResult turn(std::optional<platform::SteadyDuration> maxWait, std::coroutine_handle<> until);

    /// Turn step 1: swap the inbound queue, run the posts and submissions, then resolve the
    /// cancels.
    /// @return Whether anything was found there.
    bool runInbound();

    /// Teardown step 4: frees the `abandon` roots of every piece of parked work nothing else owns,
    /// looping until a pass finds nothing — because freeing a chain can park again.
    void abandonParkedWork() noexcept;

    /// @return Whether @c stop() has been called. An atomic rather than a read under the inbound
    ///         mutex, because it is written from any thread and `run()` asks it every turn.
    [[nodiscard]] bool stopRequested() const noexcept;

    /// Turn step 2: resumes queued coroutines, up to the batch bound.
    ///
    /// The one place a coroutine is resumed, which is what makes Rule 1 assertable: it requires
    /// that no backend dispatch is in flight on this thread, so a backend that resumed from
    /// inside its own ready-list walk is caught here rather than when the walk reads the entry a
    /// resumed frame has freed.
    /// @param bound The most coroutines this drain may resume.
    /// @return How many it resumed.
    std::size_t drainReadyQueue(std::size_t bound);

    /// Turn step 3: how long the next wait may block.
    /// @param maxWait The caller's own bound, or nullopt.
    /// @return The timeout for @c IoBackend::wait, or nullopt for an indefinite wait. The rounding
    ///         — a sub-millisecond remainder must not become a zero-timeout spin — belongs to the
    ///         backend's own conversion (@c detail::toTimeoutMillis), which is where the unit is.
    [[nodiscard]] std::optional<platform::SteadyDuration> computeTimeout(
        std::optional<platform::SteadyDuration> maxWait);

    /// Queues @p work for the next drain, recording whether its chain is the loop's to free.
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    /// @param sourcePark The park @p work was taken from, if any; see @c ReadyEntry::sourcePark.
    void queueReady(async::ParkedWork work, ParkId sourcePark = ParkId::invalid());

    /// @c resumeSoon for a frame-free operation's waiter (@c net::ResultAwaitable::complete, through
    /// @c resumeSoonOn): the same queueing, with the chain claimed by an
    /// @c async::detail::CountedClaim rather than handed over as a refcounted work item.
    ///
    /// **A separate path because it is the hot one.** Every socket operation that parks ends here,
    /// and for a chain nobody owns the work item's claim was five atomic operations per completion
    /// against this one's two. The claim is sound only where the waiter takes its queue entry back
    /// if its frame is destroyed first, which the awaitable does -- the reason this is reachable
    /// from @c resumeSoonOn alone and not a public overload of @c resumeSoon.
    /// @param waiter The suspended coroutine.
    /// @param unownedRoot Its chain's root where nobody owns it, or an empty handle.
    void resumeCompleted(std::coroutine_handle<> waiter, std::coroutine_handle<> unownedRoot);

    /// Turn step 5: queues the waiters of every park whose deadline has been reached.
    /// @return How many were queued.
    std::size_t fireExpiredTimers();

    /// Runs the callback of the timer park @p park, if it is still there.
    ///
    /// Reached from the drain, so a timer callback runs where a coroutine resumption runs and
    /// nowhere else. The park is taken out of the table BEFORE the callback is invoked, for two
    /// reasons at once: a @c cancelTimer from inside the callback must find nothing (the timer has
    /// fired), and the callback is allowed to destroy whatever owns it, so nothing may be read
    /// back out of the table afterwards.
    /// @param park The callback park to fire. A park that is gone — cancelled between step 5 and
    ///        here — is skipped, which is what makes that window cancellable.
    /// @param wake Why it is being run; see @c ParkWake. A timer ignores it.
    void runDueCallback(ParkId park, ParkWake wake);

    /// Queues the coroutine parked at @p park for resumption, and takes it out of the scheduling
    /// indices. What a backend's readiness callback reaches, and it ENQUEUES. Idempotent per park
    /// within one turn: the second call finds the waiter already taken and does nothing.
    /// @param park The park whose waiter to queue.
    void queueParkedWaiter(ParkId park);

    /// Queues @p park's waiter, or — for a frameless readiness park — its callback, with the
    /// reason it is being woken.
    /// @param park The park to queue.
    /// @param wake Why. Overridden by @c ParkWake::Abandoned where the handle was announced
    ///        closing under @c FdWakePolicy::Cancel.
    void queueParkedWaiter(ParkId park, ParkWake wake);

    /// @c queueParkedWaiter for a park the caller already holds -- a readiness report reaches its
    /// park through the handler or the handle's watch -- so the park table is not probed for it.
    /// @param filed The park; must be filed.
    /// @param wake Why it is being queued.
    void queueParkedWaiter(detail::Park& filed, ParkWake wake);

    /// The readiness callback every park registers, for both directions.
    ///
    /// Static and `noexcept`, because that is what a @c ReadinessCallback is. It only enqueues.
    /// @param handler The ready park's handler, whose `owner` is its @c detail::Park.
    static void onParkReady(ReadinessHandler& handler) noexcept;

    /// The readable callback of a @c detail::HandleWatch. It only enqueues: the reader, the writer
    /// beside it, and -- when there is no reader to take the report -- a request to narrow.
    ///
    /// **The writer is queued too, and the reason is the one-callback rule rather than a guess.**
    /// A backend services one callback per registration per wait and prefers readability, so on a
    /// registration a reader and a writer share, a socket that stays readable would never have its
    /// writability reported at all: the writer would starve behind its own socket's reads. Queuing
    /// it costs one `send` that may answer `EAGAIN`, which its owner's retry loop already treats
    /// as "stay parked" -- and only while a writer is parked and readability is armed: beside a
    /// parked reader, or for the one report a readability kept armed after its read draws before
    /// the loop narrows it away.
    /// @param handler The watch's handler, whose `owner` is its @c detail::HandleWatch.
    static void onWatchReadable(ReadinessHandler& handler) noexcept;

    /// The writable callback of a @c detail::HandleWatch. It only enqueues.
    /// @param handler The watch's handler, whose `owner` is its @c detail::HandleWatch.
    static void onWatchWritable(ReadinessHandler& handler) noexcept;

    /// Finds or makes the registration a @c RegistrationLifetime::UntilClosed park on @p handle
    /// shares, and arms it for @p interest on top of whatever it is armed for already.
    /// @param handle The handle to watch.
    /// @param kind What @p handle is.
    /// @param interest What the new park needs the registration armed for.
    /// @return The watch, or why the backend refused it. A refusal leaves no watch behind that this
    ///         call made.
    [[nodiscard]] std::expected<detail::HandleWatch*, NetError> watchHandle(platform::NativeHandle handle,
                                                                            HandleKind kind,
                                                                            Interest interest);

    /// Files @p entry in the resident park its handle's watch keeps for that direction, if it is
    /// the shape one holds: a frameless readiness park asking for exactly one direction on the
    /// registration kept for the handle's life -- a socket operation that has to wait
    /// (core-cpp#52). Everything a park filed the ordinary way answers is the same; what it saves is
    /// the id-map insert and erase and the park made and recycled, per operation.
    /// @param entry What @c registerPark was handed.
    /// @param refusal Where a backend refusal goes, as for @c registerPark.
    /// @return The park's id; @c ParkId::invalid() if the backend refused; nullopt if @p entry is
    ///         not that shape, or its direction's resident park is still held -- then the caller
    ///         files it the ordinary way, and the slot guard sees what it always saw.
    [[nodiscard]] std::optional<ParkId> registerResident(ParkEntry const& entry, NetError* refusal);

    /// Frees the slot @p park holds on its handle's watch, if it holds one. The registration stays.
    /// @param park The park being taken, cancelled or unparked.
    /// @return What the freed slot was for, or @c Interest::None if @p park held none.
    [[nodiscard]] Interest releaseWatchSlot(detail::Park& park) noexcept;

    /// Re-arms @p watch for exactly what its slots ask for, plus whatever of @p keep it is armed for
    /// already. A backend that refuses keeps the old arming, which errs towards a spurious report
    /// rather than a lost one.
    /// @param watch The watch to narrow.
    /// @param keep What may stay armed with no slot asking for it: @c Interest::Read after a write
    ///        is taken, @c Interest::None once a wait has reported something nobody took.
    void narrowWatch(detail::HandleWatch& watch, Interest keep) noexcept;

    /// Narrows every watch a wait reported with nobody parked to take the report. Turn step 4,
    /// after the wait, because a backend callback may only enqueue.
    void narrowReportedWatches() noexcept;

    /// Detaches and forgets the watch on @p handle, if there is one. The handle must still be open.
    /// @param handle The handle about to be closed.
    void dropWatch(platform::NativeHandle handle) noexcept;

    /// What a host's pump calls: one turn, waiting on nothing.
    ///
    /// Static and `noexcept`, because that is what a @c HostCallback is. An exception escaping a
    /// resumed coroutine therefore terminates rather than unwinding into the host's own loop,
    /// which is the same trade @c async::DetachedTask makes and for the same reason: there is no
    /// frame left to unwind into.
    /// @param state The loop, as a `void*`.
    static void onHostPump(void* state) noexcept;

    /// Moves every parked waiter to the ready queue so cancelled awaitables can unwind. Detaches
    /// each park from the backend first, so nothing stays registered past this call.
    void unparkEverything();

    /// Resolves one cancel request: unparks @p park and queues its waiter.
    /// @param park The park to cancel.
    void resolveCancel(ParkId park);

    /// @return Whether anything is waiting in the inbound queue.
    [[nodiscard]] bool hasInbound() const;

    /// Tells a host-driven backend when the next turn is due, after every turn.
    void armHostWake();

    IoBackend& _backend;       ///< The injected multiplexed wait and dispatcher.
    platform::IClock& _clock;  ///< The injected monotonic time source.
    EventLoopOptions _options; ///< Fixed at construction.
    bool _hostDriven;          ///< @c IoBackend::isHostDriven, asked once: a backend does not change kind.
    detail::WorkerIdentity _worker; ///< Which thread is inside a turn, if any.

    /// One queued resumption, and whether the chain behind it is the loop's to free.
    ///
    /// The flag is recorded WHERE THE WORK IS QUEUED rather than read back out of
    /// @c async::detail::Parked, which deliberately offers no accessor for it: the ownership
    /// travels inside that type so no fire site can forget the release, and a second way to ask
    /// the same question is a second way for the two answers to drift apart. What the loop needs
    /// it for is teardown, where a chain it owns is freed and a chain it borrows is resumed — the
    /// same rule the park table follows, and the reason both containers can state it once.
    struct ReadyEntry
    {
        async::detail::Parked parked {}; ///< The coroutine, and what to free if it is not resumed.

        /// The callback timer this entry is due to run, or @c ParkId::invalid() for a coroutine.
        ///
        /// **The id rather than the callback**, so that a `cancelTimer` between step 5 queueing a
        /// due timer and step 2 running it is O(1) and needs no scan of this queue: taking the
        /// park out of the table is what makes this entry resolve to nothing. It is the same
        /// generation check every other cancellation path uses, reused rather than re-invented.
        ParkId callbackPark {};

        /// The readiness or deadline park this coroutine was queued FROM, or @c ParkId::invalid().
        ///
        /// Queuing a parked waiter takes the frame and leaves the park filed -- and, for a handle,
        /// still registered with the backend -- because `await_resume` is what unregisters it. A
        /// frame taken back by @c cancelPending never runs `await_resume`, so the entry has to
        /// name the park for `cancelPending` to take it too. Without it the caller owned a frame the
        /// backend still held a handler for: core-cpp#41.
        ParkId sourcePark {};

        /// The claim a COMPLETION queued its waiter with (@c resumeCompleted), in place of
        /// @c parked's own, which is empty then; empty for every other entry.
        ///
        /// It is given back BEFORE the waiter resumes rather than after, because it holds no
        /// reference to the chain's state and the resume may end the chain -- see
        /// @c async::detail::CountedClaim.
        async::detail::CountedClaim claim {};

        // The byte-wide members in one run, after every eight-byte one: two of them apart cost an
        // eight-byte slot each, which took this entry from 56 bytes to 64 when `claim` joined it.

        bool ownedByLoop = false; ///< Whether @c parked carried a claim when it was queued.

        /// Why @c callbackPark is being run, for a frameless READINESS park. Ignored for a timer,
        /// which has exactly one reason to fire and therefore needs none carried.
        ParkWake wake = ParkWake::Ready;

        /// Resumes the entry's coroutine, giving its chain back first; a handle that cannot be
        /// resumed has its chain freed rather than dropped, as @c async::detail::Parked::resume.
        void resume()
        {
            if (auto const handle = parked.handle(); handle && !handle.done())
                claim.giveBack();
            parked.resume();
            // Empty after a resume. For a handle `parked` declined, the claim goes here, AFTER that
            // look at the handle, and frees the chain if it was the last one on it.
            claim.reset();
        }

        /// Takes the entry's coroutine back for a caller that becomes its only owner: the chain is
        /// given back rather than released, since releasing the last claim would free the very
        /// frame the caller has just been handed.
        void takeBack() noexcept
        {
            parked.take().abandon.disarm();
            claim.giveBack();
        }
    };

    /// Teardown: takes what the loop OWNS out of the ready queue, to be freed rather than resumed,
    /// and drops the due timer callbacks; what it borrows stays queued, to be resumed.
    /// @return The owned entries.
    [[nodiscard]] detail::RingQueue<ReadyEntry> setAsideOwnedReady();

    /// Teardown steps 3 and 6: drains what is queued, a bounded number of passes.
    void drainForTeardown();

    /// Coroutines ready to resume now, each owning whatever chain nothing else can free.
    ///
    /// Declared BEFORE @c _parks so it is destroyed after it: freeing a chain re-enters the loop
    /// — a frame holding a deadline runs its disarm into @c cancelPending, which reads both — and
    /// the destructor body frees everything while both are alive precisely so this ordering is
    /// never relied upon. It is stated here because the day somebody deletes the destructor body,
    /// the order is what decides whether the failure is a crash or silence.
    ///
    /// A ring that keeps its capacity rather than a `std::deque`, which allocates a node and frees
    /// one every few entries of a FIFO that never grows -- the steady state of a server, one
    /// completion queued and resumed per request (see @c detail::RingQueue).
    detail::RingQueue<ReadyEntry> _ready;

    /// Where @c queueReady files work while a drain-step callback runs, or null outside one.
    ///
    /// A waiter a callback completes resumes in the CALLBACK'S position: the drain puts what the
    /// callback queued at the front of @c _ready once it returns, ahead of everything queued after
    /// the callback -- the order 0.2.0 had by resuming inline, kept without resuming inside the
    /// callback (G2). Queued at the back instead, a flow queued ahead of the callback that yields
    /// once to let reported readiness run found the waiter not yet resumed.
    std::vector<ReadyEntry>* _queuedByCallback = nullptr;

    /// The one waiter a drain-step callback completed, held apart from every queue.
    ///
    /// A readiness callback that completes one socket operation, and queues nothing else, is the
    /// hot path of the loop: its waiter resumes right after the callback returns. Held here it is
    /// resumed from here -- no queue entry is made, moved, or popped for it. Anything else the
    /// callback queues, or a second completion, first moves this waiter to the head of the
    /// callback's range (@c flushCompletionSlot), so the order is the callback position's either
    /// way.
    struct CompletionSlot
    {
        std::coroutine_handle<> waiter {}; ///< The completed waiter, or empty.
        async::detail::CountedClaim claim; ///< Its chain's claim, where nobody owns the chain.
        std::size_t mark = 0;              ///< Where the running callback's range begins.
    };

    /// The running drain-step callback's slot, or null outside one. It points into the drain's own
    /// frame, and a nested callback's replaces it for as long as that one runs.
    CompletionSlot* _completionSlot = nullptr;

    /// Where what a drain-step callback queues waits until the callback returns. A member, and
    /// cleared rather than freed, so a readiness completion -- the hot path -- costs no
    /// allocation once it has grown; each callback owns the range from where it began, so a
    /// callback that drives a nested drain does not take the outer one's entries.
    std::vector<ReadyEntry> _callbackScratch;

    /// The callback position itself: what a callback queued, taken by the drain before anything in
    /// @c _ready from @c _resumeFirstHead on, emptied (keeping its capacity) once consumed, and moved
    /// to the front of @c _ready, less what was taken back, however the drain ends -- its bound, or
    /// a throw. Empty outside a drain, which is why no teardown step has to read it.
    std::vector<ReadyEntry> _resumeFirst;
    std::size_t _resumeFirstHead = 0; ///< The next entry of @c _resumeFirst to take.

    detail::ParkTable _parks; ///< Every park, by id, with its reverse indices.

    /// The registrations kept for the life of a handle (@c RegistrationLifetime::UntilClosed), by
    /// handle. Held by `unique_ptr` because the backend holds each watch's handler by address.
    std::unordered_map<platform::NativeHandle, std::unique_ptr<detail::HandleWatch>> _watches;

    /// The handles whose watch a wait reported with no park to take the report, to be narrowed once
    /// the wait returns. Cleared rather than swapped, so its capacity is kept across turns.
    std::vector<platform::NativeHandle> _watchesToNarrow;

    /// The parks turn step 5 found due, kept across turns so a firing allocates nothing.
    std::vector<ParkId> _expired;

    /// Parks whose handle closed since the last turn, merged into the next turn as one more source
    /// of readiness. Consumed ONLY in a turn: ~EventLoop must resume parked flows through its own
    /// request_stop()-first path, not on their normal path, because by then their owners are
    /// already destroyed.
    std::vector<ParkId> _closedParks;

    /// The subset of @c _closedParks whose handle closed under @c FdWakePolicy::Cancel, so
    /// @c wakeReasonOf can tell the awaiter to unwind rather than resume.
    std::unordered_set<ParkId> _abandoned;

    /// The coroutine @c spawn runs a flow inside, so that the flow's COMPLETION is what unlinks it.
    ///
    /// The unlink used to key on the frame a ready entry named, and a flow that parks inside a
    /// sub-task is resumed through the sub-task's frame: it completed inside that resume, by
    /// symmetric transfer, and nothing unlinked it -- one frame held per such flow until
    /// ~EventLoop. This root awaits the flow, so however the flow ends, the root reaches its
    /// final suspension, and that is where it files itself for release. It stays SUSPENDED there:
    /// its frame is destroyed by @c reapFinishedRoots after the `resume()` that finished it has
    /// returned, on the loop's thread, never from inside its own final suspension.
    class SpawnedRoot
    {
      public:
        /// The root's promise: which loop holds it, and where.
        struct promise_type
        {
            EventLoop* loop = nullptr;                ///< The loop that spawned it.
            std::list<SpawnedRoot>::iterator slot {}; ///< Where it sits in @c _roots.
            async::StopToken token;                   ///< The loop's root stop token.

            /// Files the root for release, and stays suspended.
            struct FinalAwaiter
            {
                [[nodiscard]] bool await_ready() const noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> self) const noexcept
                {
                    auto& promise = self.promise();
                    // Off the loop's thread -- a flow that ended on a pool -- the loop's own list
                    // is not this thread's to write: hand the slot over, as a submission is.
                    if (promise.loop->isOnWorkerThread())
                        promise.loop->_finishedRoots.push_back(promise.slot);
                    else
                        promise.loop->handOverFinishedRoot(promise.slot);
                }
                void await_resume() const noexcept {}
            };

            [[nodiscard]] SpawnedRoot get_return_object() noexcept
            {
                return SpawnedRoot { std::coroutine_handle<promise_type>::from_promise(*this) };
            }
            [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
            [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }
            void return_void() const noexcept {}
            /// A spawned flow's exception ends the flow and goes no further, as it always has: the
            /// root is owned by the loop, and there is nobody to rethrow it to.
            void unhandled_exception() const noexcept {}
            /// @return The token the awaited flow inherits: the loop's root stop token.
            [[nodiscard]] async::StopToken const& stopToken() const noexcept { return token; }
        };

        explicit SpawnedRoot(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
        SpawnedRoot(SpawnedRoot&& other) noexcept: _handle(std::exchange(other._handle, {})) {}
        SpawnedRoot(SpawnedRoot const&) = delete;
        SpawnedRoot& operator=(SpawnedRoot const&) = delete;
        SpawnedRoot& operator=(SpawnedRoot&&) = delete;
        ~SpawnedRoot()
        {
            if (_handle)
                _handle.destroy();
        }

        /// @return The root's frame.
        [[nodiscard]] std::coroutine_handle<promise_type> handle() const noexcept { return _handle; }

      private:
        std::coroutine_handle<promise_type> _handle;
    };

    /// The root coroutine @c spawn wraps @p task in.
    /// @param task The flow to await; by value, as every coroutine parameter here is.
    /// @return The suspended root.
    static SpawnedRoot runSpawned(async::Task<void> task);

    /// @return The next entry the drain runs: the callback position first, then the ready queue;
    ///         nothing when both are empty.
    [[nodiscard]] std::optional<ReadyEntry> takeNextReady();

    /// Runs the drain-step callback @p callback in its position: what it queues is taken next, and
    /// the one waiter it completes, where that is all it does, is resumed straight after it.
    /// @param callback The callback's park.
    /// @param wake Why it is being run.
    /// @param budget How many resumptions the drain may still make; at least one.
    /// @return How many it made: the callback, and its waiter if that was resumed here.
    std::size_t runInPosition(ParkId callback, ParkWake wake, std::size_t budget);

    /// Moves the waiter in the running callback's @c CompletionSlot, if any, to the head of that
    /// callback's range, as an ordinary queue entry.
    void flushCompletionSlot();

    /// @param entry An entry of the callback position or of a callback's range.
    /// @return Whether @c cancelPending took it back, leaving it with nothing to run.
    [[nodiscard]] static bool isTakenBack(ReadyEntry const& entry) noexcept
    {
        return !entry.callbackPark && !entry.parked.handle();
    }

    /// Destroys every root that reached its final suspension, in O(1) apiece. Called by the drain
    /// after each resume, which is where a root can finish, and before it, and by turn step 1 for
    /// the roots that finished off the loop's thread.
    void reapFinishedRoots() noexcept;

    /// Hands a root that finished off the loop's thread to turn step 1. Safe from any thread.
    ///
    /// Wakes the backend while it holds the inbound lock: step 1 takes that lock before it reaps,
    /// and ~EventLoop before its roots and backend go, so nobody can observe the root released
    /// while this thread is still inside the loop.
    /// @param slot Where the root sits in @c _roots.
    void handOverFinishedRoot(std::list<SpawnedRoot>::iterator slot) noexcept;

    /// Files @p entry at the back of the ready queue, or -- while a drain-step callback runs -- in
    /// that callback's position (@c _queuedByCallback). Every @c ReadyEntry goes through here.
    /// @param entry What to queue.
    void queueEntry(ReadyEntry&& entry);

    /// What @c resumeSoon and @c resumeCompleted share: the affinity assertion, the queueing, and
    /// the wake outside a turn.
    /// @param entry What to queue.
    void queueSoon(ReadyEntry&& entry);

    /// The one place a @c ReadyEntry is made from a work item, so the ownership flag cannot be got
    /// wrong at one site out of six. A completion's entry, which carries no work item, is made by
    /// @c resumeCompleted.
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    /// @param sourcePark The park @p work was taken from, if any; see @c ReadyEntry::sourcePark.
    /// @return The entry, flagged as the loop's to free exactly where @p work carries a claim.
    [[nodiscard]] static ReadyEntry entryFor(async::ParkedWork work, ParkId sourcePark = ParkId::invalid());

    /// Live spawned background flows. A `std::list` because a completing flow unlinks ITSELF in
    /// O(1) through the iterator its root holds: a `vector` swept with `erase_if` every turn is
    /// O(n) per turn, which a server spawning one flow per connection pays forever.
    std::list<SpawnedRoot> _roots;

    /// Roots that have finished and wait for @c reapFinishedRoots.
    std::vector<std::list<SpawnedRoot>::iterator> _finishedRoots;

    /// Whether a @c run() is what is driving the turn, which is what makes an idle turn block
    /// rather than return; see step 4. Loop thread only.
    bool _inRun = false;

    async::StopSource _rootStop; ///< Root cancellation source.

    mutable std::mutex _inboundMutex; ///< Guards @c _inbound.
    Inbound _inbound;                 ///< What other threads have handed over.

    /// Whether @c _inbound may hold something: set under the lock by every hand-off, cleared under
    /// it by the turn that takes them. Read WITHOUT the lock at the top of every turn, so a loop no
    /// other thread talks to never takes the mutex at all.
    std::atomic<bool> _inboundPending { false };

    std::atomic<bool> _stopRequested { false }; ///< Set by @c stop(), from any thread; read by @c run().
};

/// Awaitable that resumes after a delay (or throws on cancellation).
///
/// While parked it registers a stop-callback so that if its cancellation token is stopped before
/// the deadline (a `whenAny`/`withTimeout` sibling won), the park is cancelled promptly and the
/// coroutine unwinds via @c async::OperationCancelled rather than lingering until the deadline
/// elapses. Its park is removed when it is cancelled, so no handle dangles once the frame unwinds.
///
/// **The loop is nullable**, which is what the free @c sleepUntil(EventLoop*, tp) needs: a caller
/// with no loop behind it — an in-memory transport, a test double with no deadline mechanism —
/// passes null and the awaitable resolves inline without ever parking. It is a pointer rather
/// than two types because the alternative is a second awaitable with the same three members and
/// one fewer reason to exist.
///
/// **Both inline resolutions are decided in @c await_suspend, and @c await_ready is a constant.**
/// MSVC 19.44's ARM64 code generator drops the enclosing `try` of a `co_await` on a temporary
/// awaiter whose `await_ready` makes a call
/// ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)): this one read
/// the clock through the virtual @c IClock::now(), and the @c async::OperationCancelled its
/// @c await_resume threw passed a typed `catch` and `catch (...)` alike. Declining to park from
/// @c await_suspend costs nothing a caller can observe: nothing is filed with the loop, and the
/// flow resumes before `co_await` returns.
class DelayAwaiter
{
  public:
    /// @param loop The loop whose clock and deadline heap this parks on.
    /// @param deadline When to resume.
    DelayAwaiter(EventLoop& loop, platform::SteadyTimePoint deadline) noexcept:
        _loop(&loop), _deadline(deadline)
    {
    }

    /// @param loopOrNull The loop to park on, or null to resolve inline without suspending.
    /// @param deadline When to resume.
    DelayAwaiter(EventLoop* loopOrNull, platform::SteadyTimePoint deadline) noexcept:
        _loop(loopOrNull), _deadline(deadline)
    {
    }

    /// @return False: a null loop and an elapsed deadline are answered by @c await_suspend, for
    ///         the reason the class comment gives.
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Parks the awaiting coroutine on the deadline, unless there is no loop, the deadline has
    /// already passed, or the flow is already cancelled.
    ///
    /// The first two are asked BEFORE the token is read, which is what keeps the answer the one
    /// `await_ready` used to give: an elapsed deadline resolves as elapsed even for a flow that has
    /// been stopped, because @c await_resume finds no token to throw on.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) where there is no loop, the deadline has passed or the flow is
    ///         already cancelled; true to park.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_loop == nullptr || _deadline <= _loop->clock().now())
            return false;
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested())
            return false;
        // The park is filed BEFORE the stop callback exists, and nothing unregisters it if the
        // emplace below throws: `await_suspend` exiting by exception unwinds the awaiting frame
        // through its `co_await` without ever running `await_resume`, which is the only other
        // caller of `unregisterPark`. The loop would then hold a park naming storage that is
        // being destroyed. `ScopeGuard` has no dismiss, so the flag is what makes this fire on
        // the exceptional exit alone; `unregisterPark` on an invalid id is a no-op, which covers
        // a throw from `registerPark` before it files anything.
        //
        // **THREE awaiters have this shape**, and an earlier version of this comment named two --
        // which read as an audit that had been done. They are `DelayAwaiter` (here),
        // `WaitHandleAwaiter` below, and `TokenDelayAwaiter` in `InterruptibleSleep.cpp`. The
        // third is the one that matters: its park holds a live backend registration rather than a
        // heap slot. A new awaiter that parks before arming a stop callback belongs on that list.
        auto registered = false;
        // `noexcept` on the lambda is required, not decorative: `ScopeGuard`'s constraint is
        // `is_nothrow_invocable_v<Callable&>`, and an unmarked lambda fails it -- as a deduction
        // failure with no viable constructor, not as a readable message. `unregisterPark` is
        // itself `noexcept`, so the marking is honest.
        auto const undo = detail::ScopeGuard { [&]() noexcept {
            if (!registered)
                _loop->unregisterPark(_park);
        } };
        _park = _loop->registerPark(ParkEntry::onDeadline(async::detail::parkedWorkFor(awaiting), _deadline));
        _cancelReg.emplace(_token, [loop = _loop, park = _park] { loop->requestCancel(park); });
        registered = true;
        return true;
    }

    /// @throws async::OperationCancelled if the flow was cancelled while parked.
    void await_resume()
    {
        _cancelReg.reset();
        if (_loop != nullptr)
            _loop->unregisterPark(_park);
        if (_token.stop_requested())
            throw async::OperationCancelled {};
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    EventLoop* _loop;
    platform::SteadyTimePoint _deadline;
    ParkId _park {};
    async::StopToken _token;
};

/// Awaitable that resumes when a registered handle reaches a given readiness (Read or Write), or
/// throws @c async::OperationCancelled if the awaiting flow is cancelled while parked. Returned by
/// @c EventLoop::waitReadable / @c waitWritable.
///
/// Readiness is observed via the OS wait, so the awaiter is never ready before it suspends: it
/// always parks (after registering the handle with the backend), and the loop resumes it when the
/// backend dispatches readiness for it. On resume — whether ready or cancelled — it unregisters,
/// so the registration never outlives the await.
class WaitHandleAwaiter
{
  public:
    /// @param loop The loop whose backend the handle is registered with.
    /// @param handle The native handle to wait on.
    /// @param kind What @p handle is.
    /// @param interest The readiness to wait for (Read or Write).
    WaitHandleAwaiter(EventLoop& loop,
                      platform::NativeHandle handle,
                      HandleKind kind,
                      Interest interest) noexcept:
        _loop(loop), _handle(handle), _kind(kind), _interest(interest)
    {
    }

    /// Readiness is only known after the OS wait, so a valid handle never reports ready before
    /// suspending. An invalid handle resolves immediately (await_resume then reports
    /// cancellation), avoiding a pointless park on a handle that can never signal.
    /// @return True only for an invalid handle.
    [[nodiscard]] bool await_ready() const noexcept { return _handle == platform::InvalidHandle; }

    /// Captures the cancellation token, then (unless already cancelled) attaches the handle and
    /// parks. Checks cancellation BEFORE registering so a cancelled flow resumes immediately
    /// without leaving a dangling registration.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False (resume now) if already cancelled or the attach failed; true to park.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if constexpr (async::HasStopToken<Promise>)
            _token = awaiting.promise().stopToken();
        if (_token.stop_requested())
            return false;
        // The park-then-arm window, and THIS is the site where it hurts. `DelayAwaiter` and
        // `TokenDelayAwaiter` leak a heap slot if the emplace below throws; this park holds a
        // live backend REGISTRATION whose `ReadinessHandler` names the awaiting frame -- so a
        // frame unwound out of `await_suspend` leaves the kernel side attached to storage that is
        // being destroyed, and the next readiness on that descriptor queues a dead handle.
        //
        // `noexcept` on the lambda is required rather than decorative: `ScopeGuard`'s constraint
        // is `is_nothrow_invocable_v<Callable&>`, which an unmarked lambda fails as a deduction
        // error that never names the requirement. `unregisterPark` is itself `noexcept`, and it
        // detaches the handler before freeing the park, which is the whole point here.
        auto registered = false;
        auto const undo = detail::ScopeGuard { [&]() noexcept {
            if (!registered)
                _loop.unregisterPark(_park);
        } };
        _park = _loop.registerPark(
            ParkEntry::onReadiness(async::detail::parkedWorkFor(awaiting), _handle, _kind, _interest),
            &_refusal);
        if (!_park)
            return false; // registration failed: resume and surface it in await_resume
        // If the token is stopped while parked (a whenAny/withTimeout sibling won), cancel the
        // park promptly so the flow unwinds instead of waiting for readiness that may never come.
        _cancelReg.emplace(_token, [&loop = _loop, park = _park] { loop.requestCancel(park); });
        registered = true;
        return true;
    }

    /// Unregisters the park and reports a failure, a cancellation or an abandoned handle.
    /// @throws FdRegistrationFailed if the handle could not be registered with the backend
    ///         (resource exhaustion, or a kernel that refused the interest — distinct from
    ///         cancellation).
    /// @throws async::OperationCancelled if cancelled while parked, the handle was invalid, or it
    ///         was closed under @c FdWakePolicy::Cancel while parked. That last case is what keeps
    ///         a destructor from resuming this flow into an owner that no longer exists: throwing
    ///         here unwinds the frame without ever re-entering its body.
    void await_resume()
    {
        _cancelReg.reset();
        auto reason = FdWakeReason::Ready;
        if (_park)
        {
            reason = _loop.wakeReasonOf(_park);
            _loop.unregisterPark(_park);
        }
        else if (_handle != platform::InvalidHandle && !_token.stop_requested())
            throw FdRegistrationFailed { .reason = std::move(_refusal) };
        if (_token.stop_requested() || _handle == platform::InvalidHandle
            || reason == FdWakeReason::Abandoned)
            throw async::OperationCancelled {};
    }

  private:
    std::optional<async::StopCallback<std::function<void()>>> _cancelReg;
    EventLoop& _loop;
    platform::NativeHandle _handle;
    HandleKind _kind;
    Interest _interest;
    ParkId _park {};
    async::StopToken _token;
    NetError _refusal {};
};

/// Suspends until @p predicate returns true, re-checking every @p interval on @p loop's clock.
/// This is the shared teardown-drain idiom — wait for a write queue to flush, a debounce to fire,
/// an output pacer to empty — in ONE place.
///
/// It POLLS rather than parking on a completion signal, which is acceptable on the low-frequency
/// connection-teardown paths that use it (the cost is at most one @p interval of extra latency at
/// close); it is NOT for hot paths.
/// @param loop The loop whose delay drives the poll (and cancels it on shutdown).
/// @param predicate Checked before each wait; the poll returns once it holds.
/// @param interval How long to suspend between checks.
/// @return A task that completes once @p predicate holds.
[[nodiscard]] async::Task<void> pollUntil(EventLoop* loop,
                                          std::function<bool()> predicate,
                                          std::chrono::milliseconds interval = std::chrono::milliseconds {
                                              1 });

} // namespace core::net
