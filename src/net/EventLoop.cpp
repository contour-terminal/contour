// SPDX-License-Identifier: Apache-2.0
#include <net/EventLoop.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace net
{

EventLoop::EventLoop(EventSource& source, IClock& clock): _source(source), _clock(clock)
{
    // The post() self-pipe: its read end is an ordinary Read registration, so a
    // cross-thread post breaks an in-flight _source.wait() like any fd readiness.
    // The daemon depends on this for shutdown — a null pipe silently losing
    // cross-thread wakeups would deadlock. Fail fast instead.
    auto pipe = createSystemPipe();
    if (!pipe)
        throw std::runtime_error("EventLoop: cannot create post self-pipe (fd exhaustion?)");
    _postPipe = std::move(*pipe);
    _postToken = _source.attach(_postPipe->waitHandle(), FdInterest::Read);
}

EventLoop::~EventLoop()
{
    // Cancel every spawned flow and let parked awaiters unwind via RAII before
    // their frames are destroyed. Order matters: request_stop() first so that when
    // wakeAllWaiters() requeues parked handles, drainReadyQueue() resumes them into
    // await_resume(), which sees stop_requested() and throws OperationCancelled —
    // unwinding the frame's locals. The frames then complete (done() == true), so
    // the spawned-flow Tasks destroy already-finished frames. Cancelled re-awaits
    // resume synchronously (await_suspend returns false when stop is requested), so
    // a single drain converges. No-op and effectively free when nothing is parked.
    //
    // Note what is deliberately NOT done here: _closedTokens is left unconsumed.
    // Those wakes resume a flow on its NORMAL path, which is right while the loop
    // runs (the owner just called close) and wrong now — an owner declared after the
    // loop is already destroyed by the time this runs, so the flow must unwind via
    // the cancellation below instead. wakeAllWaiters still finds every such waiter,
    // because notifyHandleClosing leaves the park in place.
    _rootStop.request_stop();
    wakeAllWaiters();
    drainReadyQueue();

    // The source outlives the loop; drop the self-pipe's registration so the
    // source never waits on a handle the pipe's destruction is about to close.
    if (_postToken)
        _source.detach(_postToken);
}

void EventLoop::spawn(coro::Task<void> task)
{
    task.handle().promise().setStopToken(_rootStop.get_token());
    _ready.push_back(task.handle());
    _roots.push_back(std::move(task));
}

void EventLoop::post(std::function<void()> callback)
{
    {
        auto const lock = std::scoped_lock { _postMutex };
        _posted.push_back(std::move(callback));
    }
    // Wake a possibly-blocked wait. One byte per post is fine: the drain reads
    // them in bulk and the callback queue is swapped wholesale.
    if (_postPipe)
    {
        auto const one = char { 1 };
        std::ignore = _postPipe->write(&one, 1);
    }
}

void EventLoop::requestStop()
{
    _rootStop.request_stop();
    wakeAllWaiters();
}

void EventLoop::reapFinishedSpawns()
{
    std::erase_if(_roots, [](coro::Task<void> const& task) { return task.done(); });
}

void EventLoop::runPostedCallbacks()
{
    // Swap under the lock, run outside it: a callback may itself post() (or spawn,
    // or resume coroutines that do), and must not deadlock or invalidate the
    // container mid-iteration. Work posted DURING the run lands in the fresh
    // vector and is picked up on the next pump.
    auto pending = std::vector<std::function<void()>> {};
    {
        auto const lock = std::scoped_lock { _postMutex };
        pending.swap(_posted);
    }
    for (auto const& callback: pending)
        callback();
}

void EventLoop::drainPostPipe()
{
    if (!_postPipe)
        return;
    // A single bounded read: at least one byte is available (the source reported
    // readiness), so this cannot block. If more wakeup bytes remain, the next
    // wait reports the pipe readable again (level-triggered) — nothing is lost.
    auto buffer = std::array<char, 256> {};
    std::ignore = _postPipe->read(buffer.data(), buffer.size());
}

void EventLoop::scheduleTimer(SteadyTimePoint deadline, std::coroutine_handle<> waiter)
{
    _timers.push_back(TimerEntry { .deadline = deadline, .handle = waiter });
    std::ranges::push_heap(_timers, soonestFirst);
}

FdToken EventLoop::registerFdWaiter(NativeHandle fd, FdInterest interest, std::coroutine_handle<> waiter)
{
    auto const token = _source.attach(fd, interest);
    if (!token)
        return FdToken::invalid();
    _fdWaiters.emplace(token, ParkedWaiter { .handle = waiter, .fd = fd });
    _waiterToToken.emplace(waiter, token);
    _fdToTokens.emplace(fd, token);
    return token;
}

std::coroutine_handle<> EventLoop::dropParkedWaiter(FdToken token) noexcept
{
    auto const it = _fdWaiters.find(token);
    if (it == _fdWaiters.end())
        return {};

    auto const parked = it->second;
    _waiterToToken.erase(parked.handle);
    // Erase this token alone: a descriptor may carry a second park (a reader beside
    // a writer), and erasing by key would silently drop that one too.
    auto const [first, last] = _fdToTokens.equal_range(parked.fd);
    for (auto entry = first; entry != last; ++entry)
        if (entry->second == token)
        {
            _fdToTokens.erase(entry);
            break;
        }
    _fdWaiters.erase(it);
    return parked.handle;
}

FdWakeReason EventLoop::unregisterFdWaiter(FdToken token) noexcept
{
    if (!token)
        return FdWakeReason::Ready;
    _source.detach(token);
    static_cast<void>(dropParkedWaiter(token));
    // Consume the mark rather than merely reading it: the awaiter asks exactly once,
    // and a token left behind here would outlive its registration and could collide
    // with nothing — but would grow without bound on a long-lived loop.
    return _abandoned.erase(token) != 0 ? FdWakeReason::Abandoned : FdWakeReason::Ready;
}

void EventLoop::notifyHandleClosing(NativeHandle fd, FdWakePolicy policy)
{
    if (fd == InvalidHandle)
        return;

    auto const [first, last] = _fdToTokens.equal_range(fd);
    for (auto entry = first; entry != last; ++entry)
    {
        auto const token = entry->second;
        // Drop the kernel registration NOW, while the descriptor is still open. Left
        // to the awaiter's own detach it would be issued after the close, against a
        // descriptor number the kernel may already have reassigned — unregistering
        // whichever socket now holds it. Detaching here also releases the private
        // dup() a duplicate registration holds, which would otherwise keep the peer's
        // connection open past the close.
        _source.detach(token);
        // The park itself stays in _fdWaiters: requestStop() and ~EventLoop find their
        // waiters there, and must still be able to cancel this one if either runs
        // before the next pump.
        _closedTokens.push_back(token);
        if (policy == FdWakePolicy::Cancel)
            _abandoned.insert(token);
    }
}

void EventLoop::requeueForCancellation(std::coroutine_handle<> waiter)
{
    if (!waiter || waiter.done())
        return;

    // If parked as an fd waiter, drop and detach its registration so the stale
    // entry cannot also fire. Use the reverse map for O(1) lookup.
    auto wasParked = false;
    if (auto const rt = _waiterToToken.find(waiter); rt != _waiterToToken.end())
    {
        _source.detach(rt->second);
        static_cast<void>(dropParkedWaiter(rt->second));
        wasParked = true;
    }

    // If parked on a timer, remove its heap entry too. We re-queue the waiter below
    // so it unwinds via OperationCancelled and its frame is then destroyed; a
    // lingering entry would leave a dangling coroutine_handle that fireExpiredTimers()
    // or wakeAllWaiters() later dereference through .done()/.resume() — a
    // use-after-free. Detaching it here mirrors the fd branch above. (A coroutine is
    // parked on at most one source, so at most one of these two branches matches.)
    if (std::erase_if(_timers, [waiter](TimerEntry const& entry) { return entry.handle == waiter; }) != 0)
    {
        std::ranges::make_heap(_timers, soonestFirst);
        wasParked = true;
    }

    // Re-queue ONLY a waiter this call actually unparked. A waiter that was already
    // woken is sitting in _ready with its frame intact but its cancellation callback
    // still armed — await_resume, which disarms it, has not run yet — so a stop
    // requested in that window lands here and would queue it a SECOND time. The first
    // resume runs the flow to completion and its owner destroys the frame; the second
    // then calls .done() on freed memory. That is the use-after-free that made the
    // first attempt at close-wakeup segfault, and this guard is what removes it.
    if (wasParked)
        _ready.push_back(waiter);
}

void EventLoop::wakeFdWaiters(std::vector<FdToken> const& tokens)
{
    for (auto const token: tokens)
    {
        // Drop the parked slot now; the awaiter detaches the source registration in
        // its await_resume. Erasing first keeps the map consistent if the resumed
        // frame re-enters the loop. A token with no park (e.g. the post self-pipe,
        // or one already woken this pump) yields a null handle and is skipped.
        auto const handle = dropParkedWaiter(token);
        if (handle && !handle.done())
            _ready.push_back(handle);
    }
}

void EventLoop::drainReadyQueue()
{
    while (!_ready.empty())
    {
        auto const handle = _ready.front();
        _ready.pop_front();
        if (handle && !handle.done())
            handle.resume();
    }
}

int EventLoop::computeTimeoutMs() const
{
    if (_timers.empty())
        return -1; // Block indefinitely until a source becomes ready.

    auto const now = _clock.now();
    if (_timers.front().deadline <= now)
        return 0;

    auto const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(_timers.front().deadline - now).count();
    // A positive remainder under 1ms truncates to 0; clamp to 1 so the next wait
    // actually blocks instead of spinning on wait(0) until the deadline crosses now.
    return static_cast<int>(std::clamp<long long>(ms, 1, std::numeric_limits<int>::max()));
}

void EventLoop::fireExpiredTimers()
{
    auto const now = _clock.now();
    while (!_timers.empty() && _timers.front().deadline <= now)
    {
        std::ranges::pop_heap(_timers, soonestFirst);
        auto const entry = _timers.back();
        _timers.pop_back();
        if (entry.handle && !entry.handle.done())
            _ready.push_back(entry.handle);
    }
}

void EventLoop::wakeAllWaiters()
{
    for (auto const& entry: _timers)
        if (entry.handle && !entry.handle.done())
            _ready.push_back(entry.handle);
    _timers.clear();

    // Flush every fd waiter so a cancelled awaitable can unwind. Detach each from
    // the source and re-queue its handle; await_resume then observes the requested
    // stop and throws OperationCancelled. Move the slots out first so a resumed
    // frame re-entering the loop cannot mutate the container mid-iteration.
    auto parked = std::exchange(_fdWaiters, {});
    _waiterToToken.clear();
    _fdToTokens.clear();
    for (auto const& [token, waiter]: parked)
    {
        _source.detach(token);
        if (waiter.handle && !waiter.handle.done())
            _ready.push_back(waiter.handle);
    }
}

void EventLoop::pumpOnce()
{
    reapFinishedSpawns();
    runPostedCallbacks();
    drainReadyQueue();

    // Descriptors closed since the last pump. The source cannot report these — epoll
    // drops a closed descriptor from its set and kqueue drops its filters, both
    // silently — so the loop supplies that readiness itself and MERGES it into this
    // pump's wait outcome below.
    //
    // Merging rather than short-circuiting is load-bearing. Returning early here
    // would skip the wait, and every other descriptor that became ready in the same
    // instant — a peer's EOF, most importantly — would go unreported until some
    // later pump that may never come, because the caller's blockOn exits as soon as
    // its root flow is done. poll(2) never had that problem: it reports POLLNVAL for
    // the closed descriptor alongside every other revent, in one call. This keeps
    // every backend doing the same.
    //
    // Taken BEFORE the wait so a pending close can turn it into a non-blocking poll:
    // blocking would wait for readiness that can no longer arrive.
    auto const closed = std::exchange(_closedTokens, {});

    // Nothing is parked on a source: a well-formed root flow either completed
    // (the caller's loop will observe `done()`) or is awaiting a child task that
    // will itself park. A cross-thread post() may have enqueued callbacks between
    // the top-of-pump drain and this check — drain them once more before
    // returning so the blockOn loop, which exits when done(), does not strand
    // them.
    auto const hasParked = !_timers.empty() || !_fdWaiters.empty();
    if (!hasParked)
    {
        runPostedCallbacks();
        drainReadyQueue();
        return;
    }

    // A pending close polls instead of blocking: the closed descriptor can no longer
    // produce readiness, so an indefinite wait would never return on its account.
    auto const outcome = _source.wait(closed.empty() ? computeTimeoutMs() : 0);

    // A cross-thread post may have both queued work and signalled the self-pipe.
    if (_postToken && std::ranges::find(outcome.readyRead, _postToken) != outcome.readyRead.end())
        drainPostPipe();
    runPostedCallbacks();

    // Resume coroutines parked on any fd that became ready this wait. The post
    // self-pipe's token has no parked waiter and is skipped naturally.
    wakeFdWaiters(outcome.readyRead);
    wakeFdWaiters(outcome.readyWrite);
    // Then the closed descriptors, as one more source of readiness. Last, so a token
    // the source also reported is woken exactly once — the first wake drops its park,
    // and a token with no park is skipped.
    wakeFdWaiters(closed);

    fireExpiredTimers();

    // Resume coroutines woken during this iteration so readiness is delivered in
    // the same pump it arrived, rather than on the next one.
    drainReadyQueue();
}

DelayAwaiter EventLoop::delay(std::chrono::milliseconds duration) noexcept
{
    return DelayAwaiter { *this, _clock.now() + duration };
}

DelayAwaiter EventLoop::sleepUntil(SteadyTimePoint deadline) noexcept
{
    return DelayAwaiter { *this, deadline };
}

WaitFdAwaiter EventLoop::waitReadable(NativeHandle fd) noexcept
{
    return WaitFdAwaiter { *this, fd, FdInterest::Read };
}

WaitFdAwaiter EventLoop::waitWritable(NativeHandle fd) noexcept
{
    return WaitFdAwaiter { *this, fd, FdInterest::Write };
}

coro::Task<void> pollUntil(EventLoop* loop,
                           std::function<bool()> predicate,
                           std::chrono::milliseconds interval)
{
    while (!predicate())
        co_await loop->delay(interval);
}

} // namespace net
