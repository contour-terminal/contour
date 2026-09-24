// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>

#include <core/async/ExecutorContext.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/ScopeGuard.hpp>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <tuple>
#include <utility>

namespace core::net
{

namespace
{
    /// How many drain passes @c ~EventLoop runs before it stops resuming and starts freeing.
    ///
    /// **Bounded, and that is the whole point.** A cancelled awaitable commonly re-parks — a
    /// bounded wait arms the next step, a teardown drain awaits one more flush — so an unbounded
    /// "drain until empty" spins forever on exactly the shutdown it exists to make clean. What
    /// the passes buy is the ORDINARY case: a flow cancelled once, resumed once, unwound, with
    /// its RAII cleanup run. What survives the bound is handled by step 4, which frees rather
    /// than resumes. Sixteen because a chain of eight `co_await`s unwinding one frame per pass is
    /// already deeper than anything in this tree, and the cost of a pass that finds nothing is a
    /// container check.
    constexpr std::size_t TeardownDrainPasses = 16;

    /// The batch bound a drain uses when it must take everything: teardown, where leaving work
    /// queued would leave it for step 4 to free rather than for the flow to unwind through.
    constexpr std::size_t UnboundedDrain = std::numeric_limits<std::size_t>::max();
} // namespace

EventLoop::EventLoop(IoBackend& backend, platform::IClock& clock, EventLoopOptions options):
    _backend(backend), _clock(clock), _options(options), _hostDriven(backend.isHostDriven())
{
    // A zero batch drains nothing, so queued work would stay queued for ever -- and since an idle
    // turn is one that leaves the ready queue empty, `runUntilIdle` would never return. Refused
    // here, where the configuration is fixed, rather than discovered as a hang.
    assert(options.dispatchBatch > 0
           && "EventLoopOptions::dispatchBatch must be at least 1: a turn that may resume nothing "
              "never empties the ready queue, and runUntilIdle never returns");
    // A host-driven backend has no wait of its own: the HOST is what waits, and what it calls
    // when that wait ends is one turn of this loop. Registered once, here, because the backend
    // needs a pointer to a loop that does not exist until this constructor runs — and cleared in
    // the last step of ~EventLoop, or the host's next pump would call into freed storage.
    // Every other backend ignores it.
    _backend.setPump(&EventLoop::onHostPump, this);
}

EventLoop::~EventLoop()
{
    // ---- 1. Teardown is serialised with dispatch. -----------------------------------------
    // Clearing a pending awaitable races the readiness dispatch, so a loop is destroyed on its
    // own worker thread or with nothing driving it. Any other thread, while a turn has not
    // returned, is the violation (guarantee G5).
    assert(teardownIsSerialisedWithDispatch()
           && "an EventLoop must be destroyed on its own worker thread, or with nothing driving "
              "it -- otherwise clearing a pending awaitable races the readiness dispatch");

    // ---- 2. Request stop, then move every park to the ready queue. ------------------------
    // **What the loop OWNS is taken aside to be freed; what it borrows stays to be resumed.**
    // The same rule the park table follows, for the same reason: a chain the loop owns is a
    // `DetachedTask`, which carries no stop token -- a detached flow has no awaiting coroutine to
    // inherit one from -- so resuming it would not cancel it, it would run the rest of its body on
    // a loop that is being destroyed. A chain the loop BORROWS belongs to a `Task` somebody holds;
    // that owner set a stop token on it, and resuming it is what makes the frame unwind and run
    // its cleanup.
    //
    // Order matters both ways round. Stop FIRST, so that when the parked handles are queued, the
    // drain below resumes them into await_resume, which sees stop_requested() and throws
    // OperationCancelled -- unwinding the frame's locals. A waiter queued before the stop was
    // requested would resume on its NORMAL path instead, into an owner that is already destroyed.
    //
    // Note what is deliberately NOT done here: _closedParks is left unconsumed. Those wakes
    // resume a flow on its normal path, which is right while the loop runs (the owner just called
    // close) and wrong now. unparkEverything still finds every such waiter, because
    // notifyHandleClosing leaves the park in place.
    //
    // A due TIMER CALLBACK is in neither queue: it is dropped. It is not work to unwind and not a
    // frame to free -- it is a call into a `DeadlineTimer`'s owner, and that owner is being
    // destroyed with this loop or is already gone. Running it here would be the one path on which
    // a callback reaches an object whose loop has stopped existing.
    auto ownedQueue = setAsideOwnedReady();

    // What the drains below resume runs with this loop current, as in a turn: a flow that unwinds
    // through an awaitable which reads the context sees the loop it was on. One scope, like the
    // turn's, and for the same reason: the pointer stores are the whole cost.
    auto const current = async::ExecutorScope { *this };

    // **The inbound queue is NOT swept, and that is a decision rather than an omission.** Work
    // handed over and not yet accepted by a turn is DROPPED: never resumed, never unwound.
    //
    // **The discriminator is the CONTAINER, and it is worth saying plainly rather than dressing
    // up.** The ready queue holds work this loop put there -- unparked waiters, spawned roots,
    // continuations a turn queued -- and not resuming it strands flows the loop is itself
    // responsible for. The inbound queue holds what another thread handed over and no turn has
    // taken up. Both are drawn at the container, not at any property of the work.
    //
    // The hazard is real in BOTH and is therefore not the reason for the split: `submit(
    // std::coroutine_handle<>)` takes a bare handle, so a suspended flow that would unwind and a
    // never-started lazy `Task` that would RUN are the same type, and `ResumeOn::await_resume()`
    // is noexcept, so even a genuine continuation runs its body rather than unwinding. Resuming
    // the ready queue can start a lazy task too. What the hazard argues is that the boundary must
    // be a FIXED one rather than a judgement per item -- the loop cannot inspect a handle to
    // decide, so it decides by where the handle is.
    //
    // Resuming the inbound queue as well was tried and reverted: `LoopTeardown_test`'s "work
    // somebody else owns is left alone" and `TestLoop_test`'s "stop short-circuits run()" both
    // failed, the second with a SIGSEGV.
    //
    // **The consequence is worse than a stranded flow and belongs in the open.** Stranding is the
    // benign ordering -- the hand-off reached the queue before the destructor. The other ordering
    // is the likelier one: a thread that has not submitted YET calls `ResumeOn::await_suspend`
    // afterwards, which reads `_worker`, locks `_inboundMutex` and calls `_backend.wake()` on
    // destroyed storage. **G5 does not cover it**, because a thread holding a handle it intends
    // to submit is not driving anything, so `teardownIsSerialisedWithDispatch()` answers true.
    // A loop other threads have been handing work to is quiesced before it is destroyed -- their
    // hand-offs joined, or one more turn run -- and neither this rule nor that assertion is what
    // makes that safe.
    _rootStop.request_stop();
    unparkEverything();

    // ---- 3. Bounded drain passes. ---------------------------------------------------------
    drainForTeardown();

    // ---- 4. Abandon to a fixpoint. --------------------------------------------------------
    // Step 2's owned queue entries go first: freeing a chain can park again, and the fixpoint
    // below is what collects whatever that produces.
    ownedQueue.clear();
    abandonParkedWork();

    // ---- 5. Destroy the spawned roots. ----------------------------------------------------
    // After the abandonment, not before: a spawned flow's frame is owned HERE, so destroying it
    // first would pull the ground out from under anything still parked on it.
    // A root that finished off the loop's thread and was handed over is freed with the rest; the
    // hand-off is forgotten, not reaped through later.
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inbound.finishedRoots.clear();
    }
    _finishedRoots.clear();
    _roots.clear();

    // ---- 6. Drain what destroying them queued, to a fixpoint. -----------------------------
    // A root that owned a socket or a listener closed it on the way out, and a BORROWED flow parked
    // there -- a `Task` somebody else holds -- had its operation abandoned or closed: settled and
    // QUEUED, because a resource never resumes a waiter inline (G2). Nothing after this point
    // drains, so without this the flow stayed suspended with its operation still naming this loop,
    // and its owner, destroying it afterwards, took it out of a ready queue that no longer existed.
    // Resumed here it unwinds -- an abandoned operation throws whatever the token says -- or
    // answers the Cancelled value a close promises, while the loop still exists. Whatever that
    // parks or queues again is freed or dropped as in step 4, until a pass finds nothing.
    for ([[maybe_unused]] auto const pass: std::views::iota(std::size_t { 0 }, TeardownDrainPasses))
    {
        if (_ready.empty())
            break;
        auto owned = setAsideOwnedReady();
        drainForTeardown();
        owned.clear();
        abandonParkedWork();
    }

    // And the registrations kept for a handle's life. After the roots, because a socket destroyed
    // with its flow announces its own close and takes its watch with it, while the handle is still
    // open; what is left here is a handle whose owner never announced one, and the backend holds
    // its handler by address, so it is detached before the map frees it.
    for (auto const& [handle, watch]: _watches)
        _backend.detach(watch->handler);
    _watches.clear();

    // ---- 7. Unregister the wake. ----------------------------------------------------------
    // A host-driven backend holds a pointer to this loop, and a pump it still has out with the
    // host would call through it into freed storage on the host's next turn. That pump cannot be
    // retracted, so it still arrives, and finds no loop to run; once the backend is gone too, it
    // finds no backend either and runs nothing (its ticket expired with it). Every other backend
    // ignores both calls.
    _backend.setPump(nullptr, nullptr);
    _backend.armWakeAt(std::nullopt);
}

detail::RingQueue<EventLoop::ReadyEntry> EventLoop::setAsideOwnedReady()
{
    auto owned = detail::RingQueue<ReadyEntry> {};
    auto borrowed = detail::RingQueue<ReadyEntry> {};
    for (auto& entry: _ready)
    {
        if (entry.callbackPark)
            continue;
        (entry.ownedByLoop ? owned : borrowed).pushBack(std::move(entry));
    }
    _ready = std::move(borrowed);
    return owned;
}

void EventLoop::drainForTeardown()
{
    // Cancelled re-awaits resume synchronously (await_suspend returns false when stop is
    // requested), so one pass usually converges; the bound is what stops a flow that re-parks
    // from spinning here forever. See TeardownDrainPasses.
    for ([[maybe_unused]] auto const pass: std::views::iota(std::size_t { 0 }, TeardownDrainPasses))
    {
        if (_ready.empty())
            break;
        std::ignore = drainReadyQueue(UnboundedDrain);
    }
}

void EventLoop::abandonParkedWork() noexcept
{
    // **A fixpoint, and both containers taken before either is freed.** Freeing a chain RE-ENTERS
    // the loop: a frame holding a deadline runs its disarm into cancelPending, which reads the
    // park table, and a frame holding a bounded wait can reach schedule, which writes to it. So a
    // single fixed-order pass would leave whatever that re-entry produced for MEMBER destruction
    // -- and members die in reverse declaration order, so a chain freed from the later one then
    // searches a container whose destructor has already run.
    //
    // Origin: [fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025),
    // [fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054).
    while (true)
    {
        auto ready = std::exchange(_ready, {});
        auto parks = _parks.takeAll();
        // Every park this pass takes is gone, so every mark naming one is too.
        _abandoned.clear();
        // The inbound queue too, and it is not an afterthought: a coroutine that parked with
        // `ResumeOn` from any thread but the loop's is sitting HERE and nowhere else, so a
        // teardown that swept only the loop's own containers would leak exactly the shape #1025
        // was reported on. Its posts are dropped rather than run -- a callback handed to a loop
        // that is being destroyed has nothing left to run against.
        auto inbound = Inbound {};
        {
            auto const lock = std::scoped_lock { _inboundMutex };
            std::swap(inbound, _inbound);
            _inboundPending.store(false, std::memory_order_relaxed);
        }
        if (ready.empty() && parks.empty() && inbound.empty())
            return;

        // Detached BEFORE anything is freed: the backend holds each handler's address, and a
        // handler freed while it is still registered leaves the backend walking dead storage.
        for (auto const& park: parks)
        {
            if (park->attached)
                _backend.detach(park->handler);
            // And out of any watch slot, which names the park by address as well as by id.
            std::ignore = releaseWatchSlot(*park);
        }

        // And now the frees. Each `Parked` releases its claim on the chain it was holding, and
        // the LAST claim on a chain destroys it -- so what goes is exactly the chains nothing else
        // owns, and borrowed work is left alone.
        ready.clear();
        parks.clear();
        inbound = Inbound {};
    }
}

void EventLoop::run()
{
    assert(!_backend.isHostDriven()
           && "EventLoop::run on a host-driven loop: such a loop does not own its thread and "
              "advances only through host pumps, so run() would spin without ever yielding one");

    // Claimed HERE rather than by each turn, and that is the obligation half of the teardown rule:
    // a loop cannot be inside run() without answering running(), so no loop can forget to claim
    // and leave teardownIsSerialisedWithDispatch() answering `true` unconditionally.
    auto const onWorker = detail::WorkerIdentity::Scope { _worker };
    // What tells step 4 that an idle turn should BLOCK rather than return: see the wait there.
    // A plain member with an RAII reset rather than a parameter, because it is a property of the
    // drive and `runOnce` is reached from three of them.
    _inRun = true;
    auto const leaveRun = detail::ScopeGuard { [this] noexcept { _inRun = false; } };
    while (!stopRequested())
        std::ignore = runOnce();
}

std::size_t EventLoop::runUntilIdle()
{
    auto total = std::size_t { 0 };
    while (true)
    {
        // Zero, so this never blocks whatever the idle policy is: a caller asking the loop to run
        // out its queued work is not asking it to wait for more.
        auto const turn = runOnce(platform::SteadyDuration::zero());
        total += turn.drained;
        if (turn.idle)
            return total;
    }
}

RunOnceResult EventLoop::runOnce(std::optional<platform::SteadyDuration> maxWait)
{
    return turn(maxWait, {});
}

RunOnceResult EventLoop::turn(std::optional<platform::SteadyDuration> maxWait, std::coroutine_handle<> until)
{
    // G1: exactly one thread dequeues a loop. Asserted here rather than only in run(), because
    // this is the entry point a host pump, a test driver and blockOn all reach.
    assert(teardownIsSerialisedWithDispatch()
           && "an EventLoop turn was entered from a second thread while another is driving this "
              "loop -- run(), runOnce() and blockOn() all reach here (G1: exactly one thread "
              "dequeues a loop)");
    auto const onWorker = detail::WorkerIdentity::Scope { _worker };
    // This loop is the current executor for the whole turn (core::async::ExecutorScope), so an
    // awaitable that another thread completes -- AsyncQueue::pop -- sends a flow this turn resumed
    // back here. Once per turn rather than once per resumption: G2 puts every resumption inside
    // step 2 of a turn, so the answer cannot change between them, and a scope costs two
    // thread-local stores and no allocation however many flows the drain resumes.
    auto const current = async::ExecutorScope { *this };

    auto result = RunOnceResult {};

    // ---- 1. Swap the inbound queue: run the posts, then resolve the cancels. --------------
    auto const hadInbound = runInbound();

    // ---- 2. Drain the ready queue. --------------------------------------------------------
    // THE one place a coroutine is resumed (guarantee G2). Readiness dispatched in step 4 and
    // deadlines fired in step 5 are resumed by the NEXT turn's step 2, which is what lets the
    // loop state G2 rather than trust each backend with it.
    result.drained = drainReadyQueue(_options.dispatchBatch);

    // Handles closed since the last turn. The backend cannot report these -- epoll drops a closed
    // descriptor from its set and kqueue drops its filters, both silently -- so the loop supplies
    // that readiness itself and MERGES it into this turn below.
    //
    // Merging rather than short-circuiting is load-bearing. Returning early here would skip the
    // wait, and every other descriptor that became ready in the same instant -- a peer's EOF,
    // most importantly -- would go unreported until some later turn that may never come. poll(2)
    // never had that problem: it reports POLLNVAL for the closed descriptor alongside every other
    // revent, in one call. This keeps every backend doing the same.
    //
    // Taken BEFORE the wait so a pending close can turn it into a non-blocking poll: blocking
    // would wait for readiness that can no longer arrive.
    auto const closed = std::exchange(_closedParks, {});

    // ---- 3. Refresh the clock, then compute the timeout. ----------------------------------
    // The clock is re-sampled around the one blocking call, as IClock::refresh() asks of whoever
    // owns the loop: BEFORE the timeout is computed, so the time this turn has already spent is
    // not waited for again. A clock that reads the OS on every now() ignores it; a CachedClock
    // would otherwise never move.
    _clock.refresh();
    auto timeout = computeTimeout(maxWait);
    if (!closed.empty())
        timeout = platform::SteadyDuration::zero();

    // ---- 4. Wait. -------------------------------------------------------------------------
    // The wait DISPATCHES: every ready park's callback has already queued its coroutine by the
    // time this returns, and nothing has been resumed.
    //
    // It is skipped when nothing could possibly come back from it. A loop with no park and no
    // closed handle has nothing the backend can report, so a wait there is a syscall that can
    // only time out -- and for `blockOn`, whose root flow has just finished, one that would never
    // return at all. Work already queued still waits, with a timeout of zero: readiness that
    // arrived in the same instant is collected rather than deferred a turn.
    //
    // `run()` is the one exception, and it is the whole of what `IdlePolicy::Block` means: a loop
    // that owns its thread and has nothing to do BLOCKS, because another thread may still `post`
    // and the backend's wake channel is what ends that wait. A loop somebody else drives a turn at
    // a time must not, because the caller is what waits.
    //
    // `blockOn` waits for the same reason and it is not a special case: it OWNS the calling
    // thread for the duration, so an idle turn there has a caller who asked to block. The flow it
    // is driving may be running on another executor right now, in which case nothing is queued,
    // nothing is parked, and the hand-off back is what wakes this wait.
    auto const idleWait = (_inRun || static_cast<bool>(until)) && _options.idle == IdlePolicy::Block;
    // And skipped for `blockOn` the moment the flow it exists to finish HAS finished. That drive
    // is bounded by one flow rather than by a stop, so a wait entered after step 2 completed it
    // could only end on a deadline or a wake belonging to work nobody is waiting for -- and on a
    // loop with a spawned flow parked on a socket, on nothing at all.
    auto const driven = !until || !until.done();
    if (driven && (_parks.size() != 0 || !closed.empty() || !_ready.empty() || idleWait))
        result.dispatched = _backend.wait(timeout).dispatched;

    // A registration kept for a handle's life may be armed for more than anybody parked on it
    // wants -- readability is kept on purpose, see `detail::HandleWatch` -- and a report nobody took
    // is the signal to narrow it. HERE, after the wait, because the callback that saw the report
    // was inside the backend's walk, where enqueueing is all Rule 1 allows.
    if (!_watchesToNarrow.empty())
        narrowReportedWatches();

    // ---- 5. Refresh the clock, then fire expired deadlines, FIFO by sequence. --------------
    // And after the wait too, so the deadlines fired here see the instant the wait ENDED at
    // rather than the one it started from.
    _clock.refresh();

    // The closed handles first, as one more source of readiness. After the wait, so a park the
    // backend also reported is queued exactly once -- the first queueing takes its waiter, and a
    // park with no waiter left is skipped.
    //
    // **That idempotence covers a park with a WAITER, and no longer covers every park.** Task B6's
    // frameless readiness park has no waiter to take, so `queueParkedWaiter` pushes a `ReadyEntry`
    // for it unconditionally and a park reported by both the wait and `_closedParks` is dispatched
    // twice. It is harmless as built -- the owner's callback re-checks its own slot, and a stale
    // park id resolves to nothing -- but it is NOT the invariant the paragraph above states, and
    // the next thing written against that sentence would be written against a false premise.
    for (auto const park: closed)
        queueParkedWaiter(park);

    auto const fired = fireExpiredTimers();

    // Nothing posted, nothing resumed, nothing dispatched, nothing due, and NOTHING LEFT QUEUED:
    // this turn did nothing and the next one has nothing to do, which is what runUntilIdle and
    // TestLoop::drain stop on. It deliberately says nothing about whether work is still PARKED --
    // a flow waiting on a socket that never becomes readable leaves a loop idle turn after turn,
    // and a drain that waited for the park to go would never return.
    //
    // **The ready queue is part of the answer, and the counters alone were not.** Each counter
    // names one way work reaches `_ready` during a turn -- step 1's submissions and resolved
    // cancels, whatever step 2's resumed code queues, step 4's dispatch, step 5's due deadlines --
    // and the closed handles above were a fifth that fed none of them: a turn that queued the
    // waiters of a closed listener reported itself idle, `runUntilIdle` returned with them still
    // queued, and `~EventLoop` later resumed or freed their frames after their owners were gone
    // (fastcached's server teardown, a heap-use-after-free under ASan and TSan). Asking the queue
    // itself closes that hole for every path at once, including the next one somebody adds: a
    // turn that leaves work for the next turn is not idle, whichever step put it there.
    result.idle =
        !hadInbound && result.drained == 0 && result.dispatched == 0 && fired == 0 && _ready.empty();

    // A host-driven backend has no wait of its own, so the loop's next deadline reaches the host
    // instead. After every turn, because the turn is what changed the answer.
    armHostWake();
    return result;
}

bool EventLoop::runInbound()
{
    // Nothing handed over, which is every turn of a loop no other thread talks to: answered without
    // the lock. A hand-off that lands after this load is no worse off than one that lands after the
    // swap below: every hand-off wakes the backend once it is queued, so the wait this turn enters
    // returns at once and the next turn takes it.
    if (!_inboundPending.load(std::memory_order_acquire))
        return false;

    // Swap under the lock, run outside it: a callback may itself post (or spawn, or resume
    // coroutines that do), and must not deadlock or invalidate the container mid-iteration. Work
    // handed over DURING the run lands in the fresh queue and is picked up on the next turn.
    auto pending = Inbound {};
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inboundPending.store(false, std::memory_order_relaxed);
        pending.posts.swap(_inbound.posts);
        pending.submissions.swap(_inbound.submissions);
        pending.scheduled.swap(_inbound.scheduled);
        pending.cancels.swap(_inbound.cancels);
        pending.finishedRoots.swap(_inbound.finishedRoots);
    }
    if (pending.empty())
        return false;

    // Spawned flows that ended on another thread, released here on the loop's own.
    _finishedRoots.insert(_finishedRoots.end(), pending.finishedRoots.begin(), pending.finishedRoots.end());
    reapFinishedRoots();

    for (auto const& callback: pending.posts)
        callback();
    for (auto& work: pending.submissions)
        queueReady(std::move(work));
    for (auto& timed: pending.scheduled)
        std::ignore = registerPark(ParkEntry::onDeadline(std::move(timed.work), timed.deadline));

    // **Cancels LAST, and before the drain.** Last within the step, because a post may itself
    // park the very flow a cancel names, and a cancel resolved before that park existed would
    // resolve to nothing and leave the flow parked forever. Before the drain, because resolving a
    // cancel is what puts the cancelled flow's handle into the ready queue -- resolved after the
    // drain it would sit there while steps 3 and 4 computed a timeout and BLOCKED, and a cancel
    // from another thread would then take effect only when something unrelated woke the loop.
    for (auto const park: pending.cancels)
        resolveCancel(park);
    return true;
}

std::size_t EventLoop::drainReadyQueue(std::size_t bound)
{
    // Rule 1, asserted where it would be broken: a backend dispatches, and the loop resumes. A
    // resume from inside a backend's walk over its own ready list lets the resumed frame free the
    // object whose entry the walk has not reached yet. Origin:
    // [fastcached#475](https://github.com/LASTRADA-Software/fastcached/issues/475).
    assert(!detail::readinessDispatchInFlight()
           && "EventLoop::drainReadyQueue reached from inside a backend dispatch: "
              "backend callbacks may only enqueue");

    reapFinishedRoots();

    // Whatever ends the drain -- its bound, an empty queue, or a callback or a flow that THROWS --
    // a callback's waiters still in its position move to the front of the ready queue, ahead of
    // everything else, for the next drain. A guard, because the throw path is the one that used to
    // miss it: the waiters stayed in `_resumeFirst`, which no teardown step reads, and an owned
    // chain there was freed with the loop's members, after the park table it touches. Taken-back
    // entries are left out. The one path here that can allocate, and only where a drain ends in the
    // middle of a callback's work.
    auto const spill = detail::ScopeGuard { [this]() noexcept {
        auto const head = static_cast<std::ptrdiff_t>(_resumeFirstHead);
        for (auto& entry:
             std::ranges::subrange(_resumeFirst.begin() + head, _resumeFirst.end()) | std::views::reverse)
            if (!isTakenBack(entry))
                _ready.pushFront(std::move(entry));
        _resumeFirst.clear();
        _resumeFirstHead = 0;
    } };

    auto resumed = std::size_t { 0 };
    while (resumed < bound)
    {
        // A callback at the front of the ready queue, with the callback position empty, is read
        // where it is and dropped: it holds nothing to move out, so no entry is moved for it.
        if (_resumeFirstHead == _resumeFirst.size() && !_ready.empty() && _ready.front().callbackPark)
        {
            auto const callback = _ready.front().callbackPark;
            auto const wake = _ready.front().wake;
            _ready.dropFront();
            resumed += runInPosition(callback, wake, bound - resumed);
            continue;
        }

        auto next = takeNextReady();
        if (!next)
            break;
        // Taken back by `cancelPending` while it waited in a callback's position: nothing to run.
        if (isTakenBack(*next))
            continue;

        // A due timer callback runs HERE, where a coroutine resumption runs, and nowhere else.
        // Step 5 could have called it the moment it found the deadline due -- and then user code
        // would run at a second point in the turn, outside the bound, outside the one assertion
        // that says no backend dispatch is in flight, and after the drain rather than in it. One
        // place that hands control outside the loop is worth the extra queue hop. Read in place and
        // popped before the call: the entry holds no coroutine to move out, and the callback may
        // queue more.
        if (auto const callback = next->callbackPark)
        {
            resumed += runInPosition(callback, next->wake, bound - resumed);
            continue;
        }

        // `resume()` disowns and resumes in one expression, so work that runs normally is never
        // also freed by the entry going out of scope here -- and a handle it DECLINES to resume
        // has its chain freed rather than dropped. Resumed where `takeNextReady` put it: the entry
        // is already out of both queues.
        next->resume();
        ++resumed;

        // O(1) self-unlink: the turn that runs a spawned flow to its end releases its frame there
        // and then, whichever frame this entry named -- the root's own, or a sub-task's whose
        // completion transferred to the root inside this resume. A sweep over every spawned flow
        // at the top of each turn is O(n) per turn, which a server spawning one flow per
        // connection pays forever.
        reapFinishedRoots();
    }
    return resumed;
}

std::size_t EventLoop::runInPosition(ParkId callback, ParkWake wake, std::size_t budget)
{
    // What the callback queues -- the waiter its completion hands to the loop -- runs in the
    // callback's position: taken next once the callback returns, in the order queued, so it
    // resumes before anything that was queued after the callback. Not inside the callback, which
    // is G2, and not at the back, which let a flow queued ahead of the callback yield past the
    // waiter and read state the waiter had not updated yet. A waiter that re-parks is completed by
    // a later callback, in that one's position, so nothing here recurses.
    //
    // Neither path allocates once warm. The commonest case -- a readiness callback completing ONE
    // socket operation and queueing nothing else -- leaves its waiter in `slot` and it is resumed
    // straight from there below: no queue entry at all. Anything else goes into a member vector
    // whose capacity survives, and its range moves to the front of `_resumeFirst` -- by a swap of
    // the two vectors where the callback position was empty -- rather than into the ready queue,
    // which would shift everything behind it. Each callback owns the range from `mark` and the
    // previous targets are restored, so a callback that drives a nested drain leaves the outer
    // one's entries where they are; an outer slot's waiter is moved into its range first, so only
    // the innermost slot is ever filled. `cancelPending` marks an entry taken rather than erasing
    // it, so no range moves under a mark, and empties the slot where it finds its waiter there.
    flushCompletionSlot();
    auto const mark = _callbackScratch.size();
    auto slot = CompletionSlot { .waiter = {}, .claim = {}, .mark = mark };
    auto waiter = std::coroutine_handle<> {};
    auto claim = async::detail::CountedClaim {};
    {
        auto* const previous = std::exchange(_queuedByCallback, &_callbackScratch);
        auto* const previousSlot = std::exchange(_completionSlot, &slot);
        auto const inPosition = detail::ScopeGuard { [this, mark, previous, previousSlot]() noexcept {
            // A waiter still in the slot -- the budget is spent, or the callback threw -- takes
            // the head of the callback's range like any other.
            flushCompletionSlot();
            _completionSlot = previousSlot;
            _queuedByCallback = previous;
            if (_callbackScratch.size() == mark)
                return;
            if (mark == 0 && _resumeFirstHead == _resumeFirst.size())
            {
                _resumeFirst.clear();
                _resumeFirstHead = 0;
                _resumeFirst.swap(_callbackScratch);
                return;
            }
            auto const first = _callbackScratch.begin() + static_cast<std::ptrdiff_t>(mark);
            _resumeFirst.insert(_resumeFirst.begin() + static_cast<std::ptrdiff_t>(_resumeFirstHead),
                                std::make_move_iterator(first),
                                std::make_move_iterator(_callbackScratch.end()));
            _callbackScratch.erase(first, _callbackScratch.end());
        } };
        runDueCallback(callback, wake);
        if (budget > 1 && slot.waiter)
        {
            waiter = std::exchange(slot.waiter, {});
            claim = std::move(slot.claim);
        }
    }
    if (!waiter)
        return 1;

    // As `ReadyEntry::resume`: the chain is given back before the resume, which may end it, and a
    // handle that cannot be resumed has its claim released, freeing the chain if it was the last.
    if (waiter.done())
        claim.reset();
    else
    {
        claim.giveBack();
        waiter.resume();
    }
    reapFinishedRoots();
    return 2;
}

void EventLoop::flushCompletionSlot()
{
    auto* const slot = _completionSlot;
    if (slot == nullptr || !slot->waiter)
        return;
    // Room first, then the waiter: a failed allocation leaves it in its slot, where the callback's
    // return still finds it, rather than in a queue entry that never got queued. (Reached from a
    // completion or from the callback's unwinding, both noexcept, such a failure ends the process
    // instead -- out of memory only.)
    if (_callbackScratch.size() == _callbackScratch.capacity())
        _callbackScratch.reserve(std::max(std::size_t { 8 }, 2 * _callbackScratch.capacity()));
    auto const owned = static_cast<bool>(slot->claim);
    _callbackScratch.push_back(ReadyEntry {
        .parked = async::detail::Parked { async::ParkedWork { .resume = std::exchange(slot->waiter, {}) } },
        .claim = std::move(slot->claim),
        .ownedByLoop = owned });
}

std::size_t EventLoop::readyCount() const noexcept
{
    auto const head = static_cast<std::ptrdiff_t>(_resumeFirstHead);
    return _ready.size()
           + static_cast<std::size_t>(std::ranges::count_if(
               _resumeFirst.begin() + head, _resumeFirst.end(), [](ReadyEntry const& entry) {
                   return !isTakenBack(entry);
               }));
}

std::optional<EventLoop::ReadyEntry> EventLoop::takeNextReady()
{
    if (_resumeFirstHead < _resumeFirst.size())
    {
        auto entry = std::optional<ReadyEntry> { std::move(_resumeFirst[_resumeFirstHead]) };
        ++_resumeFirstHead;
        if (_resumeFirstHead == _resumeFirst.size())
        {
            _resumeFirst.clear(); // keeps the capacity
            _resumeFirstHead = 0;
        }
        return entry;
    }
    if (_ready.empty())
        return std::nullopt;
    return std::optional<ReadyEntry> { _ready.takeFront() };
}

void EventLoop::reapFinishedRoots() noexcept
{
    // One at a time from the back, keeping the capacity: a loop that spawns a flow per connection
    // reaps on every completion. Popped before the erase, because destroying a root destroys its
    // flow's frames, whose destructors may spawn -- and a spawn ending at once files here.
    while (!_finishedRoots.empty())
    {
        auto const slot = _finishedRoots.back();
        _finishedRoots.pop_back();
        _roots.erase(slot);
    }
}

void EventLoop::handOverFinishedRoot(std::list<SpawnedRoot>::iterator slot) noexcept
{
    // The wake is made UNDER the lock, unlike `post`'s. Turn step 1 takes this lock before it
    // reaps the root, and ~EventLoop takes it (step 5) before the roots and the backend go, so once
    // an owner can see the root released -- spawnedCount() at zero, runUntilIdle returned -- this
    // thread is out of the loop and its backend. Woken after the unlock, it was still inside
    // `_backend.wake()` while an owner that saw zero destroyed both.
    auto const lock = std::scoped_lock { _inboundMutex };
    _inbound.finishedRoots.push_back(slot);
    _inboundPending.store(true, std::memory_order_release);
    _backend.wake();
}

namespace
{
    /// How a spawned root awaits its flow: as `Task`'s own awaiter does -- the flow's continuation
    /// is the root, and it inherits the root's stop token -- except that an exception the flow ended
    /// in is TAKEN rather than rethrown. `co_await task` rethrew it into the root only for the
    /// root's promise to catch it, which is a second throw of every failing flow.
    struct AwaitSpawnedFlow
    {
        async::Task<void>& flow; ///< The flow; owned by the root's frame.

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> root) const noexcept
        {
            auto& promise = flow.handle().promise();
            promise.continuation = root;
            promise.setStopToken(root.promise().stopToken());
            return flow.handle();
        }

        /// @return The exception the flow ended in, or none; the root drops it.
        [[nodiscard]] std::exception_ptr await_resume() const noexcept
        {
            return flow.handle().promise().exception;
        }
    };
} // namespace

EventLoop::SpawnedRoot EventLoop::runSpawned(async::Task<void> task)
{
    std::ignore = co_await AwaitSpawnedFlow { task };
}

std::optional<platform::SteadyDuration> EventLoop::computeTimeout(
    std::optional<platform::SteadyDuration> maxWait)
{
    auto timeout = std::optional<platform::SteadyDuration> {};

    // Work is already queued, so the wait is a POLL: readiness that arrived in the same instant is
    // still collected, but nothing is blocked on behind work the loop could be doing. Without
    // this, a turn whose drain hit its batch bound would go on to block indefinitely with a full
    // ready queue, which is a hang rather than a slow loop.
    if (!_ready.empty())
        timeout = platform::SteadyDuration::zero();
    else if (auto const due = _parks.nextDeadline(); due.has_value())
    {
        auto const now = _clock.now();
        timeout = *due <= now ? platform::SteadyDuration::zero() : (*due - now);
    }

    if (maxWait.has_value() && (!timeout.has_value() || *maxWait < *timeout))
        timeout = maxWait;

    // A loop somebody else drives never blocks inside a turn: the caller is what waits, and a
    // turn that slept through a deadline would sleep through the caller's own work too.
    if (_options.idle == IdlePolicy::Return)
        timeout = platform::SteadyDuration::zero();

    // The rounding -- a sub-millisecond remainder must not become a zero-timeout spin -- belongs
    // to the backend's own conversion (detail::toTimeoutMillis), which is where the unit is.
    return timeout;
}

std::size_t EventLoop::fireExpiredTimers()
{
    // No deadline armed at all, which is every turn of a loop serving sockets and nothing else:
    // answered before the heap is consulted.
    if (_parks.timerSlotCount() == 0)
        return 0;
    auto fired = std::size_t { 0 };
    _parks.takeExpired(_clock.now(), _expired);
    for (auto const park: _expired)
    {
        // Two kinds of park come back from one heap, in one order: a coroutine to resume, and a
        // callback to call. Both are QUEUED here and run by the next turn's drain, which is what
        // makes the order between them the heap's -- soonest first, then by arming sequence --
        // rather than an artefact of which mechanism got to fire first.
        auto const* const entry = _parks.find(park);
        if (entry != nullptr && entry->onExpired != nullptr)
            queueEntry(ReadyEntry { .parked = {}, .callbackPark = park, .ownedByLoop = false });
        else
            queueParkedWaiter(park);
        ++fired;
    }
    return fired;
}

void EventLoop::runDueCallback(ParkId park, ParkWake wake)
{
    // A READINESS park is answered first, and the difference from a timer is that it SURVIVES its
    // own dispatch. Its owner runs a retry loop across many wakes -- a `write` of a buffer larger
    // than the send window takes as many writable edges as it takes -- so taking it out here would
    // retire an operation after one partial transfer. Only the owner retires it, through
    // `unregisterPark`. Read before the call rather than after, because the callback is allowed to
    // do exactly that and the table must not be touched afterwards.
    if (auto* const readiness = _parks.find(park); readiness != nullptr && readiness->onReady != nullptr)
    {
        auto* const onReady = readiness->onReady;
        auto* const state = readiness->callbackState;
        // Before the call: a report arriving from here on is one this call has not answered.
        readiness->readinessQueued = false;
        onReady(state, wake);
        return;
    }

    // Taken out of the table BEFORE the call, and that is what makes two things true at once: a
    // `cancelTimer` from inside the callback finds nothing (this timer HAS fired), and the
    // callback may destroy whatever owns it, because nothing here reads the table afterwards.
    //
    // A park that is already gone is a timer cancelled between step 5 queueing it and this drain
    // reaching it -- the window `cancelTimer` documents -- and skipping it is what closes it.
    auto entry = _parks.take(park);
    if (!entry || entry->onExpired == nullptr)
        return;
    entry->onExpired(entry->callbackState);
    // Kept for the next park rather than freed, as `unregisterPark` keeps one: a timer is made and
    // fired once per deadline, and a receive deadline is one per read that waits. The spare list
    // is not the table, so this reads nothing the callback may have changed.
    _parks.recycle(std::move(entry));
}

void EventLoop::armHostWake()
{
    if (!_hostDriven)
        return;

    // **`_closedParks` is not in the test below, and this asserts the reason rather than trusting
    // it.** This function is the ONLY place that decides what counts as work for a host-driven
    // loop, so an omission here has nothing left in the tree to contradict it.
    //
    // It is empty at this point for two independent reasons, which is why the assertion survives
    // either of them changing alone:
    //
    //  1. A closed park can only exist where a park is on a handle, and
    //     `HostDrivenBackend::attach` refuses every handle (`NetErrorCode::Unsupported`, "this
    //     backend has no readiness"), so `registerPark` never files one and
    //     `notifyHandleClosing` finds nothing to record.
    //  2. The turn takes `_closedParks` with `std::exchange` before the wait, and nothing
    //     between there and this call runs code that could refill it. **Including a timer
    //     callback**, which is the path worth checking rather than assuming:
    //     `fireExpiredTimers` only QUEUES a due callback, and `runDueCallback` runs it from
    //     `drainReadyQueue` -- step 2 of the NEXT turn, which is ahead of that turn's own
    //     exchange. A close performed by a timer callback is therefore caught by the turn that
    //     ran it, never stranded behind one.
    //
    // Counting it in the condition instead would be worse than useless: a closed park would then
    // be handled SILENTLY, so the day the premise stops holding is the day nothing says so. An
    // assertion has no behaviour to inherit -- it is the invariant the paragraph above describes,
    // made to fail loudly. **If a host-driven backend ever gains readiness, this fires, and
    // `notifyHandleClosing` needs the arming too, or a flow parked on a descriptor that closes is
    // never resumed** -- the one failure a readiness poller cannot report for itself, which is why
    // `notifyHandleClosing` exists at all.
    //
    // **Its case is a canary mode, `core-cpp.hostdriven-canary.closedPark`**, because observing an
    // assertion from inside a Catch case aborts the binary. It was once declined, priced against a
    // whole `WILL_FAIL` process for an invariant unreachable today; with the marker scheme a mode is
    // a few lines in an existing canary, and "unreachable today" is the argument FOR a canary, not
    // against one -- the assertion exists for the day the premise breaks, so the case builds a
    // host-driven backend that accepts readiness and watches it fire.
    assert(_closedParks.empty()
           && "armHostWake with a closed park pending: a host-driven backend has gained readiness, "
              "so this condition and notifyHandleClosing both need to arm the host");

    // Work already queued means "as soon as you can", which is a deadline of now rather than no
    // deadline at all: a host-driven loop has no other way of getting another turn.
    if (!_ready.empty() || hasInbound())
    {
        _backend.armWakeAt(_clock.now());
        return;
    }
    _backend.armWakeAt(_parks.nextDeadline());
}

void EventLoop::onHostPump(void* state) noexcept
{
    // Zero, because the host is what waits: this turn collects whatever is due and returns, and
    // the turn's own armWakeAt asks for the next one.
    std::ignore = static_cast<EventLoop*>(state)->runOnce(platform::SteadyDuration::zero());
}

void EventLoop::submit(std::coroutine_handle<> handle)
{
    submit(async::ParkedWork { .resume = handle });
}

void EventLoop::submit(async::ParkedWork work)
{
    if (!work.resume)
        return;
    // Inline only from the loop's OWN thread. Anywhere else -- another thread, or this one while
    // no turn is in flight -- it goes through the inbound queue, because "nobody is driving right
    // now" is not the same fact as "nobody else can start", and a queue two threads write to
    // without a lock is a data race whether or not one of them happens to be the owner.
    if (isOnWorkerThread())
    {
        queueReady(std::move(work));
        return;
    }
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inbound.submissions.push_back(std::move(work));
        _inboundPending.store(true, std::memory_order_release);
    }
    _backend.wake();
}

void EventLoop::schedule(platform::SteadyTimePoint deadline, std::coroutine_handle<> handle)
{
    schedule(deadline, async::ParkedWork { .resume = handle });
}

void EventLoop::schedule(platform::SteadyTimePoint deadline, async::ParkedWork work)
{
    if (!work.resume)
        return;
    if (isOnWorkerThread())
    {
        std::ignore = registerPark(ParkEntry::onDeadline(std::move(work), deadline));
        return;
    }
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inbound.scheduled.push_back(TimedWork { .deadline = deadline, .work = std::move(work) });
        _inboundPending.store(true, std::memory_order_release);
    }
    _backend.wake();
}

bool EventLoop::cancelPending(std::coroutine_handle<> handle) noexcept
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::cancelPending from a second thread while another is driving this loop: "
              "post() a call to it instead");
    if (!handle)
        return false;

    // The ready queue first: a handle submitted and not yet resumed -- and what a running
    // drain-step callback has queued, which joins the ready queue only when the callback returns.
    // An entry in a callback's range, or in the callback position, is marked taken rather than
    // erased: marks into those ranges stay valid, and the drain skips it.
    auto const takeFrom = [this, handle](auto& queue, auto const& first, bool erase) {
        auto const found = std::find_if(first, queue.end(), [handle](ReadyEntry const& entry) {
            return entry.parked.handle() == handle;
        });
        if (found == queue.end())
            return false;
        // Taken rather than only erased: the caller becomes the only one who may resume or destroy
        // it, so this entry must do neither on its way out. And the chain is DISARMED rather than
        // released -- releasing the last claim would free the very frame the caller has just been
        // handed, which is the opposite of an ownership transfer.
        auto const source = found->sourcePark;
        found->takeBack();
        if (erase)
            queue.erase(found);
        // **The park it was queued from comes down with it** -- the two structures answer "is it
        // queued" and "is it parked", and a waiter dispatched by readiness is BOTH until its
        // `await_resume` runs. This frame will never run it, so what `await_resume` would have
        // done happens here: the park leaves the table and its handler leaves the backend.
        // Returning without it handed the caller a frame the backend could still dispatch into
        // (core-cpp#41). A stale or invalid id is a no-op, which is the generation check.
        unregisterPark(source);
        return true;
    };
    // The running callback's slot: a completed waiter whose frame is destroyed before the callback
    // returns. The claim is given back, as for any entry taken back.
    if (auto* const slot = _completionSlot; slot != nullptr && slot->waiter == handle)
    {
        slot->waiter = {};
        slot->claim.giveBack();
        return true;
    }
    if (takeFrom(_ready, _ready.begin(), true)
        || takeFrom(_resumeFirst, _resumeFirst.begin() + static_cast<std::ptrdiff_t>(_resumeFirstHead), false)
        || takeFrom(_callbackScratch, _callbackScratch.begin(), false))
        return true;

    if (auto const id = _parks.byWaiter(handle))
    {
        auto park = _parks.take(id);
        // Same reason as in `unregisterPark`: the mark is per park, so it goes when the park does.
        _abandoned.erase(id);
        if (park)
        {
            if (park->attached)
                _backend.detach(park->handler);
            std::ignore = releaseWatchSlot(*park);
            park->parked.take().abandon.disarm();
            return true;
        }
    }

    // And the inbound queue, which is where work handed over from another thread -- or from this
    // one between turns -- waits for step 1. Leaving it out would make the answer depend on which
    // thread submitted, which is exactly the kind of "true here, false there" an ownership
    // transfer cannot afford.
    auto const lock = std::scoped_lock { _inboundMutex };
    auto const queued = std::ranges::find_if(
        _inbound.submissions, [handle](async::ParkedWork const& work) { return work.resume == handle; });
    if (queued != _inbound.submissions.end())
    {
        queued->abandon.disarm();
        _inbound.submissions.erase(queued);
        return true;
    }
    auto const timed = std::ranges::find_if(
        _inbound.scheduled, [handle](TimedWork const& entry) { return entry.work.resume == handle; });
    if (timed != _inbound.scheduled.end())
    {
        timed->work.abandon.disarm();
        _inbound.scheduled.erase(timed);
        return true;
    }
    return false;
}

void EventLoop::post(std::function<void()> callback)
{
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inbound.posts.push_back(std::move(callback));
        _inboundPending.store(true, std::memory_order_release);
    }
    // Break a possibly-blocked wait. The wakeup channel belongs to the backend -- `wake()` is the
    // one member of IoBackend another thread may call -- so the loop holds no descriptor of its
    // own for this, and a backend with no wait to break (HostDriven) still gets told there is
    // work.
    _backend.wake();
}

void EventLoop::stop() noexcept
{
    _stopRequested.store(true, std::memory_order_release);
    _backend.wake();
}

bool EventLoop::stopRequested() const noexcept
{
    return _stopRequested.load(std::memory_order_acquire);
}

void EventLoop::requestStop()
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::requestStop from a second thread while another is driving this loop: "
              "post() a call to it instead");
    _rootStop.request_stop();
    unparkEverything();

    // **Not redundant with the stop above, and the narrow case is why.** A flow that registered a
    // cancellation on this token -- every `spawn`ed one parked through `DelayAwaiter` -- has already
    // reached `requestCancel` by now, and that wakes. What has not is a park filed through the
    // public `registerPark` with no stop registration behind it: `unparkEverything` queues its
    // waiter and nothing tells the host, so the unwind this call exists to perform waits for a
    // turn that never comes.
    //
    // `wake()` rather than `armHostWake()`: the unparked waiters are ready NOW and carry no
    // deadline of their own, which is exactly what `wake()` says.
    if (!isOnWorkerThread())
        _backend.wake();
}

void EventLoop::spawn(async::Task<void> task)
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::spawn from a second thread while another is driving this loop: "
              "post() a call to it instead");
    if (!task.handle())
        return;
    auto const slot = _roots.insert(_roots.end(), runSpawned(std::move(task)));
    auto const handle = slot->handle();
    handle.promise().loop = this;
    handle.promise().slot = slot;
    handle.promise().token = _rootStop.get_token();
    // Borrowed, not owned: the frame belongs to the root in _roots above, so the ready entry must
    // not carry a claim on it. Its completion, or step 5 of the teardown, is what frees it.
    queueReady(async::ParkedWork { .resume = handle });

    // A spawn from outside a turn has to WAKE the loop, or a flow queued before anything drives it
    // is one nothing will ever start: a blocked wait does not know the queue changed, and a
    // host-driven backend has no wait at all to notice -- there, `wake()` IS how the host is asked
    // for the turn that runs this flow. Inside a turn it is redundant, because the turn drains
    // what it queued and arms the host itself, so it is asked only where it is needed.
    if (!isOnWorkerThread())
        _backend.wake();
}

TimerId EventLoop::addTimer(platform::SteadyTimePoint deadline, TimerCallback onExpired, void* state)
{
    // The same predicate the turn and the destructor use: the loop's own thread, or nobody
    // driving. Anywhere else this would write the park table beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::addTimer from a second thread while another is driving this loop: "
              "post() a call to it instead");
    assert(onExpired != nullptr
           && "EventLoop::addTimer with no callback: a timer with nothing to "
              "run would be filed and fired into nothing");
    if (onExpired == nullptr)
        return TimerId::invalid();
    // **A timer armed outside a turn has to ask for one, and that arming lives in `registerPark`**
    // -- the call below -- rather than here, where it first landed. `registerPark` is the
    // primitive every park goes through, so an arming placed one level above it covered this
    // caller and left the other five without it. Two members computing the same answer from the
    // same deadline heap is what this must not become.
    return TimerId { registerPark(ParkEntry::onCallback(onExpired, state, deadline)) };
}

bool EventLoop::cancelTimer(TimerId timer) noexcept
{
    // The same predicate `addTimer` asserts, and this is the half that needs it more: `addTimer`
    // is called where the author of the timer chose, while this is reached from `~DeadlineTimer`
    // -- wherever the object owning the timer happens to be destroyed. Unsynchronised it mutates
    // the park map, the handle multimap and the deadline heap while a turn is reading them.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::cancelTimer from a second thread while another is driving this loop: "
              "post() a call to it instead");
    if (!timer)
        return false;

    // Looked up before it is taken, and the kind is checked: a `ParkId` that named a COROUTINE
    // park would otherwise be unparked here -- silently freeing a flow's park and leaving it
    // waiting forever -- by a caller who only had the wrong strong type to begin with.
    auto const* const entry = _parks.find(timer.park);
    if (entry == nullptr || entry->onExpired == nullptr)
        return false;

    // The park goes; any ReadyEntry naming it resolves to nothing when the drain reaches it. That
    // is the generation check doing the work, and it is why cancelling a timer that step 5 has
    // already queued costs no scan of the ready queue.
    _parks.recycle(_parks.take(timer.park));
    return true;
}

void EventLoop::resumeSoon(async::ParkedWork work)
{
    queueSoon(entryFor(std::move(work)));
}

void EventLoop::resumeCompleted(std::coroutine_handle<> waiter, std::coroutine_handle<> unownedRoot)
{
    // The hot path: the running drain-step callback's first completion, with nothing queued by it
    // before -- held in its slot, and resumed from there once it returns (`runInPosition`).
    if (auto* const slot = _completionSlot;
        slot != nullptr && waiter && !slot->waiter && _callbackScratch.size() == slot->mark)
    {
        slot->claim = async::detail::CountedClaim::on(unownedRoot);
        slot->waiter = waiter;
        return;
    }
    // No claim for an empty handle, which `queueSoon` drops: it would arm the chain with nothing
    // left to give it back.
    auto claim = waiter ? async::detail::CountedClaim::on(unownedRoot) : async::detail::CountedClaim {};
    auto const owned = static_cast<bool>(claim);
    queueSoon(ReadyEntry { .parked = async::detail::Parked { async::ParkedWork { .resume = waiter } },
                           .claim = std::move(claim),
                           .ownedByLoop = owned });
}

void EventLoop::queueSoon(ReadyEntry&& entry)
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    // Named after `resumeSoon`, the public member, whose affinity canary reads this text.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::resumeSoon from a second thread while another is driving this loop: "
              "post() a call to it instead");
    if (!entry.parked.handle())
        return;
    queueEntry(std::move(entry));

    // Ready work filed outside a turn asks for one, for `registerPark`'s reason. `wake()` and not
    // `armHostWake()` because this work has no deadline: "as soon as you can" is precisely what
    // `wake()` means, and it is what `post`, `submit`, `spawn` and `stop` already use for the same
    // kind of work. `wake()` is also the one member the backend contract declares thread-safe.
    if (!isOnWorkerThread())
        _backend.wake();
}

void EventLoop::queueReady(async::ParkedWork work, ParkId sourcePark)
{
    queueEntry(entryFor(std::move(work), sourcePark));
}

EventLoop::ReadyEntry EventLoop::entryFor(async::ParkedWork work, ParkId sourcePark)
{
    auto const owned = static_cast<bool>(work.abandon);
    return ReadyEntry { .parked = async::detail::Parked { std::move(work) },
                        .sourcePark = sourcePark,
                        .ownedByLoop = owned };
}

void EventLoop::queueEntry(ReadyEntry&& entry)
{
    // Every entry of every queue a completion passes through has this size, so it is held: the
    // claim a completion carries once took it from 56 bytes to 64 for want of member order.
    static_assert(sizeof(void*) != 8 || sizeof(ReadyEntry) <= 56, "EventLoop::ReadyEntry grew past 56 bytes");
    if (_queuedByCallback != nullptr)
    {
        flushCompletionSlot();
        _queuedByCallback->push_back(std::move(entry));
    }
    else
        _ready.pushBack(std::move(entry));
}

ParkId EventLoop::registerPark(ParkEntry entry, NetError* refusal)
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::registerPark from a second thread while another is driving this loop: "
              "post() a call to it instead");
    // A park is a coroutine to resume OR a callback to call; one without either would be filed,
    // indexed and fired into nothing.
    if (!entry.work.resume && entry.onExpired == nullptr && entry.onReady == nullptr)
        return ParkId::invalid();

    // A socket operation that has to wait, the once-per-request case, is filed in the storage its
    // handle keeps for that direction. Anything else, or that storage still held, comes below; the
    // test before the call is what every other kind of park pays for it.
    if (entry.onReady != nullptr && entry.lifetime == RegistrationLifetime::UntilClosed)
        if (auto const resident = registerResident(entry, refusal))
            return *resident;

    auto park = _parks.acquire();
    park->loop = this;
    park->handle = entry.handle;
    park->deadline = entry.deadline;
    park->onExpired = entry.onExpired;
    park->onReady = entry.onReady;
    park->callbackState = entry.callbackState;
    park->ownedByLoop = static_cast<bool>(entry.work.abandon);
    // A frameless park -- a socket operation, a timer callback -- has no coroutine to hold, and a
    // park recycled by `unregisterPark` holds none already: nothing to move in.
    if (entry.work.resume)
        park->parked = async::detail::Parked { std::move(entry.work) };

    detail::HandleWatch* watch = nullptr;
    if (entry.handle != platform::InvalidHandle && entry.lifetime == RegistrationLifetime::UntilClosed)
    {
        // One registration for the handle's life, shared by its reader and its writer: this park
        // takes a slot on it, and the backend is asked for nothing unless the registration is not
        // yet armed for what this park watches. See `detail::HandleWatch`.
        auto watched = watchHandle(entry.handle, entry.kind, entry.interest);
        if (!watched)
        {
            // Refused as the per-park path below refuses, and for its reasons.
            if (refusal != nullptr)
                *refusal = std::move(watched.error());
            park->parked.take().abandon.disarm();
            return ParkId::invalid();
        }
        watch = *watched;
        // A park takes a slot only for a direction it asks for, and `watch` is set only on a park a
        // slot will name: one asking for neither is filed by handle like a park with a registration
        // of its own, so a close still finds it, and the watch never has a park it cannot clear.
        if (hasInterest(entry.interest, Interest::Read) || hasInterest(entry.interest, Interest::Write))
            park->watch = watch;
        else
            watch = nullptr;
    }
    else if (entry.handle != platform::InvalidHandle)
    {
        // Both directions point at the same callback, and there is deliberately no onError: a
        // failure then reaches whichever direction this park watches, which is what a parked read
        // and a parked accept both want -- they resume, look, and report what they find. A
        // dedicated onError would have to decide that for them.
        park->handler = ReadinessHandler { .handle = entry.handle,
                                           .kind = entry.kind,
                                           .owner = park.get(),
                                           .onReadable = &EventLoop::onParkReady,
                                           .onWritable = &EventLoop::onParkReady,
                                           .onError = nullptr };

        auto attached = _backend.attach(park->handler);
        // A refused interest must not leave a park behind claiming the handle is watched: the
        // awaiting flow has to fail rather than park on an interest the kernel never accepted,
        // which nothing could ever resume.
        auto armed = attached ? _backend.setInterest(park->handler, entry.interest) : std::move(attached);
        if (!armed)
        {
            // The kernel's reason travels out with the refusal rather than being flattened to a
            // bool here. Both calls return it for exactly that purpose, and `BadHandle` versus a
            // filter the kernel would not arm is the difference a consumer debugging descriptor
            // exhaustion has to be able to see.
            if (refusal != nullptr)
                *refusal = std::move(armed.error());
            _backend.detach(park->handler);
            // The flow is about to resume and report the refusal, so the chain is ITS again.
            // Letting this park's claim go out of scope instead would free -- if it held the last
            // one -- the very frame that is about to run await_resume.
            park->parked.take().abandon.disarm();
            return ParkId::invalid();
        }
        park->attached = true;
    }

    auto* const filed = park.get();
    auto const id = _parks.add(std::move(park));

    // The slot is taken once the park has its id. One read operation and one write operation per
    // socket: a slot that still names a live park is a second operation armed over the first, which
    // the socket contract forbids and `contract::claimReadSlot` catches one level up. A park filed
    // through this function directly reaches here without that guard, and the park it would
    // displace is never woken by readiness again -- a hang with no message -- so this ends the
    // process in every build, naming the handle and the direction, as the socket's guard does
    // (core/net/SocketContract.hpp says why neither refusing nor resolving is the answer).
    // `ParkTable::fileByHandle` is then a no-op, kept so a slot is always released the same way.
    if (watch != nullptr && hasInterest(entry.interest, Interest::Read))
    {
        auto* displaced = _parks.find(watch->reader);
        contract::claimReadSlot(displaced, entry.handle);
        _parks.fileByHandle(watch->reader);
        watch->reader = id;
        watch->readerPark = filed;
    }
    if (watch != nullptr && hasInterest(entry.interest, Interest::Write))
    {
        auto* displaced = _parks.find(watch->writer);
        contract::claimWriteSlot(displaced, entry.handle);
        _parks.fileByHandle(watch->writer);
        watch->writer = id;
        watch->writerPark = filed;
    }

    // **A park filed outside a turn has to ask for the turn that will reach it**, and on a
    // host-driven backend nothing else ever will: `armHostWake` runs at the END of a turn, and a
    // quiescent host-driven loop has nothing scheduled that would start one. The park would be
    // filed, correct, and silently never fired or resumed -- which is precisely how a browser event
    // handler, a frame callback or a TUI input path files one. It needs no test double to reach:
    // `core::async::DetachedTask` is eagerly started, so `co_await loop->delay()` inside such a
    // handler arrives here off-turn.
    //
    // **It belongs here rather than in `addTimer`, where it first landed.** Six call sites in this
    // module reach this function -- `runInbound`, `schedule`, `addTimer`, `DelayAwaiter`,
    // `WaitHandleAwaiter` and `TokenDelayAwaiter` -- plus whatever a consumer files through the
    // public overload, and an arming placed in `addTimer` covered exactly one of them.
    //
    // **`armHostWake()` and not `wake()`, which is the opposite choice from `resumeSoon` and
    // `requestStop`, and the difference is the deadline rather than a preference.** Those two file
    // work that is ready NOW, which is what `wake()` means. A park has a time attached, and
    // `HostDrivenBackend::wake()` is `scheduleAt(now)` -- so asking for a wake here would tell the
    // host to pump immediately for a deadline fifty milliseconds out, spend a turn finding nothing
    // due, and re-arm from the same heap. `HostDrivenLoop_test.cpp` measures exactly that: the
    // deadline cases assert `soonestDelayMs(host) == 50`, and both expand to 0 against a `wake()`.
    // So the rule is one rule, not two idioms: **ready work wakes, a park arms.**
    //
    // The thread-safety question this raises is real and does not bite here. `wake()` is the only
    // member of the backend contract declared thread-safe, and `armHostWake` reads `_ready`,
    // `hasInbound()` and `_parks.nextDeadline()` on the way to `armWakeAt`. But this function
    // already mutates `_parks` with no synchronisation of its own, so two threads calling it
    // concurrently is a data race with or without the arming -- which is what the assertion above
    // forbids. The arming cannot make a single-threaded-by-contract function less safe.
    //
    // On a backend that is not host-driven `armHostWake` returns immediately, and correctly so:
    // `!isOnWorkerThread()` together with that assertion means nothing is driving this loop, so
    // there is no blocking wait to break and the next turn computes its own timeout. Inside a turn
    // it is skipped, because the turn arms the host itself on its way out, and `scheduleAt`
    // coalesces whatever repetition is left.
    if (!isOnWorkerThread())
        armHostWake();
    return id;
}

std::optional<ParkId> EventLoop::registerResident(ParkEntry const& entry, NetError* refusal)
{
    auto const oneDirection = entry.interest == Interest::Read || entry.interest == Interest::Write;
    if (entry.onReady == nullptr || entry.work.resume || entry.deadline.has_value()
        || entry.handle == platform::InvalidHandle || entry.lifetime != RegistrationLifetime::UntilClosed
        || !oneDirection)
        return std::nullopt;

    auto watched = watchHandle(entry.handle, entry.kind, entry.interest);
    if (!watched)
    {
        // Refused as the ordinary path refuses, and for its reasons. There is no coroutine to hand
        // back: this path only ever files a frameless park.
        if (refusal != nullptr)
            *refusal = std::move(watched.error());
        return ParkId::invalid();
    }
    auto& watch = **watched;
    auto const reads = entry.interest == Interest::Read;
    auto& slot = reads ? watch.readerResident : watch.writerResident;
    if (slot == detail::ParkTable::NoResidentSlot)
        slot = _parks.openResident();
    auto* const park = slot == detail::ParkTable::NoResidentSlot ? nullptr : _parks.idleResident(slot);
    if (park == nullptr)
        return std::nullopt;

    park->loop = this;
    park->handle = entry.handle;
    park->onReady = entry.onReady;
    park->callbackState = entry.callbackState;
    park->watch = &watch;
    auto const id = _parks.addResident(slot);

    // The slot, as `registerPark` takes it and for its reasons: a live park still named there is a
    // second operation armed over the first, which ends the process naming the handle.
    auto& named = reads ? watch.reader : watch.writer;
    if (named)
    {
        auto* displaced = _parks.find(named);
        if (reads)
            contract::claimReadSlot(displaced, entry.handle);
        else
            contract::claimWriteSlot(displaced, entry.handle);
        _parks.fileByHandle(named);
    }
    named = id;
    (reads ? watch.readerPark : watch.writerPark) = park;

    // As `registerPark`: a park filed outside a turn asks for the turn that will reach it.
    if (!isOnWorkerThread())
        armHostWake();
    return id;
}

void EventLoop::unregisterPark(ParkId park) noexcept
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::unregisterPark from a second thread while another is driving this loop: "
              "post() a call to it instead");
    // A resident park -- a socket operation filed in its handle's kept storage -- gives its slot
    // back as below, and is then kept for that direction's next operation rather than taken out of
    // an id map. Nothing else differs: it holds no coroutine and no registration of its own.
    if (detail::ParkTable::isResident(park))
    {
        if (!_abandoned.empty())
            _abandoned.erase(park);
        auto* const resident = _parks.find(park);
        if (resident == nullptr)
            return;
        if (auto* const watch = resident->watch; hasInterest(releaseWatchSlot(*resident), Interest::Write))
            narrowWatch(*watch, Interest::Read);
        _parks.retireResident(*resident);
        return;
    }

    auto entry = _parks.take(park);
    // The abandon mark goes with the park, on every path that takes one. `wakeReasonOf` consumes
    // it one turn later on the ordinary path, but a park closed under `FdWakePolicy::Cancel` and
    // then taken by `cancelPending` -- or freed at teardown -- never reaches that, and the mark
    // would outlive the park it names for the loop's whole life. Asked only when there is a mark
    // at all: this runs once per parked operation, and there almost never is.
    if (!_abandoned.empty())
        _abandoned.erase(park);
    if (!entry)
        return;
    if (entry->attached)
        _backend.detach(entry->handler);
    // A watched park gives its slot back and leaves the registration where it is. Writability is
    // narrowed away NOW rather than when a wait next reports it, because it would be reported on
    // the very next wait: a socket with room in its send buffer is writable on every one of them.
    if (auto* const watch = entry->watch; hasInterest(releaseWatchSlot(*entry), Interest::Write))
        narrowWatch(*watch, Interest::Read);
    // Whatever is still here is a frame that is resuming right now -- await_resume is what calls
    // this -- so the chain belongs to it again rather than to the loop.
    if (entry->parked)
        entry->parked.take().abandon.disarm();
    // Kept for the next park rather than freed: this is the once-per-operation path, and the park
    // is empty now that its chain is handed back.
    _parks.recycle(std::move(entry));
}

FdWakeReason EventLoop::wakeReasonOf(ParkId park) noexcept
{
    // The same predicate the turn and the destructor use: this loop's own thread, or nobody
    // driving it. Anywhere else, this writes scheduler state beside a turn that is reading it.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::wakeReasonOf from a second thread while another is driving this loop: "
              "post() a call to it instead");
    // Consumed rather than merely read: the awaiter asks exactly once, and an id left behind here
    // would outlive its park and grow without bound on a long-lived loop. An empty set is the
    // common case by far -- a handle closed under a parked flow is rare -- and is answered unhashed.
    if (_abandoned.empty())
        return FdWakeReason::Ready;
    return _abandoned.erase(park) != 0 ? FdWakeReason::Abandoned : FdWakeReason::Ready;
}

void EventLoop::requestCancel(ParkId park) noexcept
{
    if (!park)
        return;

    // Resolved HERE when the caller is already the loop's thread, which a stop callback commonly
    // is: `whenAny` stops its losers from inside the drain that ran the winner. A park left live
    // until the next turn is one whose frame its OWNER may destroy in between -- a `whenAny` loser
    // is freed the moment the winner returns -- and the loop would then hold a handle to freed
    // storage.
    if (isOnWorkerThread())
    {
        resolveCancel(park);
        return;
    }

    // From another thread the loop's state is not ours to touch, so it goes through the inbound
    // queue and step 1 resolves it. push_back can throw and this is noexcept: a lost cancel is a
    // flow that hangs, so terminating is the honest answer rather than swallowing it.
    {
        auto const lock = std::scoped_lock { _inboundMutex };
        _inbound.cancels.push_back(park);
        _inboundPending.store(true, std::memory_order_release);
    }
    _backend.wake();
}

void EventLoop::resolveCancel(ParkId park)
{
    auto* const entry = _parks.find(park);
    // No such park: it has already resumed, or it was never here. That is the generation check,
    // and the id itself is what performs it -- ids are never reused, so a stale request can never
    // name a park made since.
    //
    // A FRAMELESS readiness park has no `parked` and is still cancellable: it is how a socket
    // operation is stopped, and its owner is told so through `ParkWake::Cancelled` rather than by
    // a frame observing a stopped token. Its registration comes down here for the same reason a
    // coroutine park's does.
    if (entry == nullptr || (!entry->parked && entry->onReady == nullptr))
        return;

    // Drop the kernel registration NOW, while the park is still alive: a stale registration could
    // fire again and queue a frame that has since unwound.
    if (entry->attached)
    {
        _backend.detach(entry->handler);
        entry->attached = false;
    }
    // A watched park's registration outlives it by design, so what stops it firing again is the
    // slot: a report that finds the slot empty queues nothing, and narrows instead.
    std::ignore = releaseWatchSlot(*entry);
    queueParkedWaiter(park, entry->onReady != nullptr ? ParkWake::Cancelled : ParkWake::Ready);
}

void EventLoop::onParkReady(ReadinessHandler& handler) noexcept
{
    auto* const park = static_cast<detail::Park*>(handler.owner);
    park->loop->queueParkedWaiter(*park, ParkWake::Ready);
}

void EventLoop::onWatchReadable(ReadinessHandler& handler) noexcept
{
    auto* const watch = static_cast<detail::HandleWatch*>(handler.owner);
    auto* const loop = watch->loop;
    if (watch->reader)
        loop->queueParkedWaiter(*watch->readerPark, ParkWake::Ready);
    // The writer too, for the starvation reason the declaration gives.
    if (watch->writer)
        loop->queueParkedWaiter(*watch->writerPark, ParkWake::Ready);
    if (!watch->reader && !watch->narrowQueued)
    {
        watch->narrowQueued = true;
        loop->_watchesToNarrow.push_back(handler.handle);
    }
}

void EventLoop::onWatchWritable(ReadinessHandler& handler) noexcept
{
    auto* const watch = static_cast<detail::HandleWatch*>(handler.owner);
    auto* const loop = watch->loop;
    if (watch->writer)
        loop->queueParkedWaiter(*watch->writerPark, ParkWake::Ready);
    else if (!watch->narrowQueued)
    {
        watch->narrowQueued = true;
        loop->_watchesToNarrow.push_back(handler.handle);
    }
}

std::expected<detail::HandleWatch*, NetError> EventLoop::watchHandle(platform::NativeHandle handle,
                                                                     HandleKind kind,
                                                                     Interest interest)
{
    auto const [found, created] = _watches.try_emplace(handle);
    if (created)
    {
        found->second = std::make_unique<detail::HandleWatch>();
        auto& fresh = *found->second;
        fresh.loop = this;
        // No onError, for the reason a park has none: a failure reaches the readable callback, and
        // that one wakes both directions, which is exactly who has a syscall to learn it through.
        fresh.handler = ReadinessHandler { .handle = handle,
                                           .kind = kind,
                                           .owner = &fresh,
                                           .onReadable = &EventLoop::onWatchReadable,
                                           .onWritable = &EventLoop::onWatchWritable,
                                           .onError = nullptr };
        if (auto attached = _backend.attach(fresh.handler); !attached)
        {
            _watches.erase(found);
            return std::unexpected { std::move(attached.error()) };
        }
    }

    auto& watch = *found->second;
    auto const wanted = watch.armed | interest;
    if (wanted == watch.armed)
        return &watch;
    if (auto armed = _backend.setInterest(watch.handler, wanted); !armed)
    {
        // A watch this call made and could not arm is taken down again, so a refusal leaves
        // nothing behind. One that already existed keeps its arming: it is still right for the
        // parks it has.
        if (created)
        {
            _backend.detach(watch.handler);
            _watches.erase(found);
        }
        return std::unexpected { std::move(armed.error()) };
    }
    watch.armed = wanted;
    return &watch;
}

Interest EventLoop::releaseWatchSlot(detail::Park& park) noexcept
{
    if (park.watch == nullptr)
        return Interest::None; // no watch, or it went with the handle's close announcement
    auto& watch = *std::exchange(park.watch, nullptr);
    auto released = Interest::None;
    if (watch.reader == park.id)
    {
        watch.reader = ParkId::invalid();
        watch.readerPark = nullptr;
        released = released | Interest::Read;
    }
    if (watch.writer == park.id)
    {
        watch.writer = ParkId::invalid();
        watch.writerPark = nullptr;
        released = released | Interest::Write;
    }
    return released;
}

void EventLoop::narrowWatch(detail::HandleWatch& watch, Interest keep) noexcept
{
    auto wanted = Interest::None;
    if (watch.reader || (hasInterest(keep, Interest::Read) && hasInterest(watch.armed, Interest::Read)))
        wanted = wanted | Interest::Read;
    if (watch.writer || (hasInterest(keep, Interest::Write) && hasInterest(watch.armed, Interest::Write)))
        wanted = wanted | Interest::Write;
    if (wanted == watch.armed)
        return;
    // Muting through `Interest::None` keeps the registration attached, and is silent on every
    // backend: a watch with nothing parked on it hears nothing until a park re-arms it.
    if (_backend.setInterest(watch.handler, wanted))
        watch.armed = wanted;
}

void EventLoop::narrowReportedWatches() noexcept
{
    for (auto const handle: _watchesToNarrow)
    {
        auto const found = _watches.find(handle);
        if (found == _watches.end())
            continue; // closed between the report and here
        found->second->narrowQueued = false;
        narrowWatch(*found->second, Interest::None);
    }
    _watchesToNarrow.clear();
}

void EventLoop::dropWatch(platform::NativeHandle handle) noexcept
{
    auto const found = _watches.find(handle);
    if (found == _watches.end())
        return;
    // The parks holding its slots let go of it first: they may outlive it by a turn, and a pointer
    // left behind would name freed storage when they are taken.
    for (auto const slot: { found->second->reader, found->second->writer })
        if (auto* const park = _parks.find(slot); park != nullptr && park->watch == found->second.get())
            park->watch = nullptr;
    // And the storage its operations were filed in: freed now where idle, or when the operation
    // still parked there is taken.
    _parks.closeResident(found->second->readerResident);
    _parks.closeResident(found->second->writerResident);
    _backend.detach(found->second->handler);
    _watches.erase(found);
}

void EventLoop::queueParkedWaiter(ParkId park)
{
    queueParkedWaiter(park, ParkWake::Ready);
}

void EventLoop::queueParkedWaiter(ParkId park, ParkWake wake)
{
    // Not filed any more: a readiness report that raced the park's retirement, or a stale id.
    if (auto* const filed = _parks.find(park))
        queueParkedWaiter(*filed, wake);
}

void EventLoop::queueParkedWaiter(detail::Park& filed, ParkWake wake)
{
    assert(_parks.find(filed.id) == &filed
           && "EventLoop::queueParkedWaiter handed a park the table does not hold: a watch slot "
              "outlived the park it names");
    auto const park = filed.id;
    // A FRAMELESS readiness park is queued as a callback rather than as a resumption, and -- unlike
    // a timer -- it is NOT taken out of the table: its owner runs a retry loop across many wakes
    // and only the owner retires it. Queued rather than called here for the reason a due timer is:
    // this is reached from inside a backend dispatch, where Rule 1 permits enqueueing and nothing
    // else.
    if (filed.onReady != nullptr)
    {
        // A frameless park has no waiter for `notifyHandleClosing`'s mark to reach, so it is
        // consulted HERE instead of in an `await_resume`. Consuming it is safe for the same reason
        // it is safe there -- exactly one thing asks per announcement -- and it cannot take the
        // mark a COROUTINE park's awaiter is owed, because this branch is only reached for a park
        // that has no coroutine at all.
        auto const closing = wakeReasonOf(park) == FdWakeReason::Abandoned ? ParkWake::Abandoned : wake;

        // **Once per park, not once per report.** A coroutine park is queued once by construction
        // -- queueing takes its waiter -- but a frameless park stays filed, and a level-triggered
        // backend reports its handle on every wait until somebody reads it, which nobody does while
        // the callback is still waiting in the queue: behind the drain bound, for one. Each report
        // queued the callback again, each copy spent a slot of the bound doing nothing, and the
        // copies crowded out the very work that would consume the readiness: 64 busy connections
        // on one loop, at the default bound of 64, cost some 1,460 dispatches per round trip. A copy
        // for the SAME reason answers nothing the queued entry will not; a different reason -- a
        // cancel or an abandonment behind a readiness -- is still queued, as it always was.
        if (filed.readinessQueued && filed.queuedWake == closing)
            return;
        filed.readinessQueued = true;
        filed.queuedWake = closing;
        queueEntry(ReadyEntry { .parked = {}, .callbackPark = park, .ownedByLoop = false, .wake = closing });
        return;
    }

    auto work = _parks.takeWaiter(park);
    if (!work.resume)
        return; // already taken this turn, or no such park
    if (work.resume.done())
        // Nothing will run await_resume for a finished frame, so nothing would ever unregister
        // this park; drop it here or the loop waits forever on a registration whose owner is gone.
        unregisterPark(park);
    queueReady(std::move(work), park);
}

void EventLoop::notifyHandleClosing(platform::NativeHandle handle, FdWakePolicy policy)
{
    // Reached from every socket and listener `close()` and destructor in the tree, which is
    // why it is worth asserting rather than trusting: a socket is destroyed in more places
    // than a timer is, and a destructor runs wherever its owner happens to die. Until this
    // landed it was the one public member mutating loop-owned state -- `_closedParks`,
    // `_abandoned`, and `_parks` through `parksOn` -- with neither this assertion nor the
    // `_inbound`-under-`_inboundMutex` route that `post`, `submit`, `schedule`, `stop` and
    // `requestCancel` take, so its requirement lived only in prose, under a rulebook section
    // headed "Thread affinity, asserted rather than documented".
    //
    // **The advice differs from its siblings on purpose, and that is not an inconsistency.**
    // `post()`-ing this call is NOT the fix, because it cannot be separated from the `close()`
    // that must follow it: the handle has to still be open when the backend drops its
    // registration, or the removal lands on a descriptor number the kernel may already have
    // reassigned. The whole close moves to the loop thread instead. A diagnostic copied from a
    // sibling that tells its reader to do something impossible here is worse than none.
    assert(teardownIsSerialisedWithDispatch()
           && "EventLoop::notifyHandleClosing from a second thread while another is driving this "
              "loop: move the close itself to the loop thread -- this call and the close() that "
              "follows it cannot be separated, so post()ing this one alone does not help");
    if (handle == platform::InvalidHandle)
        return;

    // The parks on the handle: those with a registration of their own, from the handle index, and
    // those holding a slot on the handle's watch, which the index does not carry -- the watch names
    // them, and filing them twice would cost the index an insert and an erase per operation.
    auto parks = _parks.parksOn(handle);
    if (auto const watch = _watches.find(handle); watch != _watches.end())
        for (auto const slot: { watch->second->reader, watch->second->writer })
            if (slot && _parks.find(slot) != nullptr)
                parks.push_back(slot);

    // The registration kept for the handle's life ends HERE, which is the promise
    // `RegistrationLifetime::UntilClosed` asked of the caller: while the descriptor is still open,
    // for the reason the loop below gives, and out of any batch a wait in flight is walking, which
    // `detach` does and which is what keeps a closed descriptor from ever being dispatched.
    dropWatch(handle);

    for (auto const park: parks)
    {
        auto* const entry = _parks.find(park);
        if (entry == nullptr)
            continue;
        // Drop the kernel registration NOW, while the descriptor is still open. Left to the
        // awaiter's own unregister it would be issued after the close, against a descriptor number
        // the kernel may already have reassigned -- unregistering whichever socket now holds it.
        // Detaching here also releases the private dup() a duplicate registration holds, which
        // would otherwise keep the peer's connection open past the close.
        if (entry->attached)
        {
            _backend.detach(entry->handler);
            entry->attached = false;
        }
        // The park itself stays: requestStop() and ~EventLoop find their waiters there, and must
        // still be able to cancel this one if either runs before the next turn.
        _closedParks.push_back(park);
        if (policy == FdWakePolicy::Cancel)
            _abandoned.insert(park);
    }
}

void EventLoop::unparkEverything()
{
    // **Only what the loop BORROWS is queued; what the loop owns stays parked and is freed.**
    //
    // The two are different questions with different answers, and `ParkedWork` is what tells them
    // apart. A chain the loop merely borrows is owned by a `Task` somebody holds -- a spawned
    // flow, a `blockOn` root -- and that owner set a stop token on it, so resuming it makes
    // `await_resume` observe the stop and throw `OperationCancelled`: the frame unwinds and its
    // RAII cleanup runs, which is the whole reason a teardown drains at all.
    //
    // A chain the loop OWNS is a `DetachedTask`, and a detached flow carries no stop token by
    // construction -- there is no awaiting coroutine to inherit one from. Resuming it would not
    // cancel it; it would run the rest of its body, on a loop that is being destroyed, with
    // nothing left for it to park on and its owner already gone. So it is left where it is and
    // freed by step 4, which is what
    // [fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025) concluded.
    // A CALLBACK park is neither, and it falls out of the `!entry->parked` test below: there is no
    // frame to unwind and nothing to free, and calling it would reach an owner that is being
    // destroyed. It is left for the table to drop, which teardown step 4 does.
    for (auto const park: _parks.ids())
    {
        auto* const entry = _parks.find(park);
        if (entry == nullptr || !entry->parked || entry->ownedByLoop)
            continue;
        if (entry->attached)
        {
            _backend.detach(entry->handler);
            entry->attached = false;
        }
        std::ignore = releaseWatchSlot(*entry);
        queueParkedWaiter(park);
    }
}

bool EventLoop::hasInbound() const
{
    auto const lock = std::scoped_lock { _inboundMutex };
    return !_inbound.empty();
}

DelayAwaiter EventLoop::delay(platform::SteadyDuration duration) noexcept
{
    return DelayAwaiter { *this, _clock.now() + duration };
}

DelayAwaiter EventLoop::sleepUntil(platform::SteadyTimePoint deadline) noexcept
{
    return DelayAwaiter { *this, deadline };
}

WaitHandleAwaiter EventLoop::waitReadable(platform::NativeHandle handle, HandleKind kind) noexcept
{
    return WaitHandleAwaiter { *this, handle, kind, Interest::Read };
}

WaitHandleAwaiter EventLoop::waitWritable(platform::NativeHandle handle, HandleKind kind) noexcept
{
    return WaitHandleAwaiter { *this, handle, kind, Interest::Write };
}

async::Task<void> pollUntil(EventLoop* loop,
                            std::function<bool()> predicate,
                            std::chrono::milliseconds interval)
{
    while (!predicate())
        co_await loop->delay(interval);
}

} // namespace core::net
