// SPDX-License-Identifier: Apache-2.0
#include <net/EventLoop.h>

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
    // If creation fails (fd exhaustion), post() still queues work — it is then
    // only picked up when the loop next wakes for another reason.
    if (auto pipe = createSystemPipe())
    {
        _postPipe = std::move(*pipe);
        _postToken = _source.attach(_postPipe->waitHandle(), FdInterest::Read);
    }
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
    _fdWaiters.emplace(token, waiter);
    return token;
}

void EventLoop::unregisterFdWaiter(FdToken token) noexcept
{
    if (!token)
        return;
    _source.detach(token);
    _fdWaiters.erase(token);
}

void EventLoop::requeueForCancellation(std::coroutine_handle<> waiter)
{
    if (!waiter || waiter.done())
        return;

    // If parked as an fd waiter, drop and detach its registration so the stale
    // entry cannot also fire.
    for (auto it = _fdWaiters.begin(); it != _fdWaiters.end(); ++it)
    {
        if (it->second == waiter)
        {
            _source.detach(it->first);
            _fdWaiters.erase(it);
            break;
        }
    }

    // If parked on a timer, remove its heap entry too. We re-queue the waiter below
    // so it unwinds via OperationCancelled and its frame is then destroyed; a
    // lingering entry would leave a dangling coroutine_handle that fireExpiredTimers()
    // or wakeAllWaiters() later dereference through .done()/.resume() — a
    // use-after-free. Detaching it here mirrors the fd branch above. (A coroutine is
    // parked on at most one source, so at most one of these two branches matches.)
    if (std::erase_if(_timers, [waiter](TimerEntry const& entry) { return entry.handle == waiter; }) != 0)
        std::ranges::make_heap(_timers, soonestFirst);

    // Re-queue once. drainReadyQueue skips already-done handles, so a single push
    // is safe.
    _ready.push_back(waiter);
}

void EventLoop::wakeFdWaiters(std::vector<FdToken> const& tokens)
{
    for (auto const token: tokens)
    {
        auto const it = _fdWaiters.find(token);
        if (it == _fdWaiters.end())
            continue; // unknown token (e.g. the post self-pipe) — not a parked flow
        auto const handle = it->second;
        // Drop the parked slot now; the awaiter detaches the source registration in
        // its await_resume. Erasing first keeps the map consistent if the resumed
        // frame re-enters the loop.
        _fdWaiters.erase(it);
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
    for (auto const& [token, handle]: parked)
    {
        _source.detach(token);
        if (handle && !handle.done())
            _ready.push_back(handle);
    }
}

void EventLoop::pumpOnce()
{
    reapFinishedSpawns();
    runPostedCallbacks();
    drainReadyQueue();

    // Nothing is parked on a source: a well-formed root flow either completed
    // (the caller's loop will observe `done()`) or is awaiting a child task that
    // will itself park. Returning avoids a wait with no one to wake. (Posted
    // callbacks arriving cross-thread are still picked up: blockOn re-enters the
    // pump, whose top runs them.)
    auto const hasParked = !_timers.empty() || !_fdWaiters.empty();
    if (!hasParked)
        return;

    auto const outcome = _source.wait(computeTimeoutMs());

    // A cross-thread post may have both queued work and signalled the self-pipe.
    if (_postToken && std::ranges::find(outcome.readyRead, _postToken) != outcome.readyRead.end())
        drainPostPipe();
    runPostedCallbacks();

    // Resume coroutines parked on any fd that became ready this wait. The post
    // self-pipe's token has no parked waiter and is skipped naturally.
    wakeFdWaiters(outcome.readyRead);
    wakeFdWaiters(outcome.readyWrite);

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
