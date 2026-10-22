// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionWorkerPool.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <optional>
#include <utility>

#include <vtextension/Engine.hpp>

namespace vtextension
{

ExtensionWorkerPool::ExtensionWorkerPool(std::size_t workerCount)
{
    Require(workerCount > 0);
    _workers.reserve(workerCount);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, workerCount))
        _workers.push_back(std::make_unique<Worker>());
}

ExtensionWorkerPool::~ExtensionWorkerPool()
{
    stop();
}

void ExtensionWorkerPool::stop() noexcept
{
    for (auto const& worker: _workers)
    {
        {
            auto const lock = std::scoped_lock { worker->mutex };
            worker->state = WorkerState::Stopping;
        }
        worker->wakeup.notify_one();
    }
    for (auto const& worker: _workers)
        if (worker->thread.joinable())
            worker->thread.join();
}

void ExtensionWorkerPool::start()
{
    std::call_once(_started, [this] {
        for (auto const& worker: _workers)
        {
            worker->thread = std::thread([this, &worker = *worker] { work(worker); });
            _running.fetch_add(1, std::memory_order_release);
        }
    });
}

LaneId ExtensionWorkerPool::assignLane()
{
    start();
    auto const next = _nextLane.fetch_add(1, std::memory_order_relaxed);
    return LaneId { static_cast<uint32_t>(next % _workers.size()) };
}

ExtensionWorkerPool::Worker& ExtensionWorkerPool::workerOf(LaneId lane) const
{
    return *_workers.at(std::to_underlying(lane));
}

void ExtensionWorkerPool::post(LaneId lane, std::function<void()> job)
{
    auto& worker = workerOf(lane);
    {
        auto const lock = std::scoped_lock { worker.mutex };
        worker.jobs.push_back(std::move(job));
    }
    worker.wakeup.notify_one();
}

TimerId ExtensionWorkerPool::schedule(LaneId lane, std::chrono::milliseconds after, std::function<void()> job)
{
    // The lane is folded into the id, so that cancel() finds the worker without a lookup.
    auto const sequence = _nextTimer.fetch_add(1, std::memory_order_relaxed);
    auto const id = TimerId { (sequence * _workers.size()) + std::to_underlying(lane) };
    auto& worker = workerOf(lane);
    {
        auto const lock = std::scoped_lock { worker.mutex };
        worker.timers.push_back(Timer { .due = _clock.now() + after, .id = id, .job = std::move(job) });
        std::ranges::push_heap(worker.timers, dueLater);
    }
    worker.wakeup.notify_one();
    return id;
}

void ExtensionWorkerPool::cancel(TimerId timer) noexcept
{
    auto& worker = *_workers[std::to_underlying(timer) % _workers.size()];
    auto cancelled = std::function<void()> {};
    {
        auto const lock = std::scoped_lock { worker.mutex };
        auto const found = std::ranges::find(worker.timers, timer, &Timer::id);
        if (found == worker.timers.end())
            return;
        cancelled = std::move(found->job);
        worker.timers.erase(found);
        std::ranges::make_heap(worker.timers, dueLater);
    }
    // The job is destroyed outside the lock: what it captured may take locks of its own.
}

void ExtensionWorkerPool::work(Worker& worker)
{
    // Held for the thread's whole life, and renewed whenever the runtime it was entered for has been
    // replaced by a new one (the last engine went, another came). A worker the runtime cannot prepare
    // still runs its jobs; a runner finds out from Engine::ThreadEnv::isCurrentThreadPrepared() that it
    // cannot instantiate.
    auto threadEnv = std::optional<Engine::ThreadEnv> {};
    auto const prepare = [&] {
        if (threadEnv && threadEnv->generation() == Engine::runtimeGeneration())
            return;
        threadEnv.reset(); // Ended first: the new guard must not nest in the stale one.
        if (auto entered = Engine::ThreadEnv::enter())
            threadEnv.emplace(std::move(*entered));
    };
    prepare();

    auto lock = std::unique_lock { worker.mutex };
    while (true)
    {
        if (!worker.jobs.empty())
        {
            auto job = std::move(worker.jobs.front());
            worker.jobs.pop_front();
            lock.unlock();
            prepare();
            job();
            job = nullptr; // Destroyed outside the lock, on this lane.
            lock.lock();
            continue;
        }
        if (worker.state == WorkerState::Stopping)
            break;
        if (worker.timers.empty())
        {
            worker.wakeup.wait(lock);
            continue;
        }
        if (worker.timers.front().due > _clock.now())
        {
            worker.wakeup.wait_until(lock, worker.timers.front().due);
            continue;
        }
        std::ranges::pop_heap(worker.timers, dueLater);
        worker.jobs.push_back(std::move(worker.timers.back().job));
        worker.timers.pop_back();
    }

    // The timers that never fired are dropped here, on the lane their jobs belong to.
    auto discarded = std::exchange(worker.timers, {});
    lock.unlock();
    discarded.clear();
}

} // namespace vtextension
