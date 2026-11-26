// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IExtensionScheduler that runs nothing until the test pumps it, on the test's own
// thread, by a manual clock.
#pragma once

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::testing
{

/// Runs posted jobs when the test says so, and fires timers as the test advances the clock. Every lane
/// is the test's thread, so this is deterministic; as a worker pool's lane would be, the thread is prepared
/// to call into instances while a job runs.
class ManualScheduler final: public IExtensionScheduler
{
  public:
    /// @param backend What the jobs' instances run on: the thread is prepared for it while each job runs,
    ///        when it has a runtime up. Must outlive the scheduler.
    explicit ManualScheduler(wasm::IWasmBackend& backend): _backend { backend } {}

    [[nodiscard]] LaneId assignLane() override
    {
        ++lanesAssigned;
        return LaneId { 0 };
    }

    void post(LaneId /*lane*/, std::function<void()> job) override { _jobs.push_back(std::move(job)); }

    [[nodiscard]] TimerId schedule(LaneId /*lane*/,
                                   std::chrono::milliseconds after,
                                   std::function<void()> job) override
    {
        auto const id = TimerId { _nextTimer++ };
        _timers.push_back(Timer { .due = _clock.now() + after, .id = id, .job = std::move(job) });
        return id;
    }

    void cancel(TimerId timer) noexcept override
    {
        std::erase_if(_timers, [&](auto const& t) { return t.id == timer; });
    }

    [[nodiscard]] core::platform::IClock& clock() noexcept override { return _clock; }

    /// Runs posted jobs, and the jobs they post, until none is left. Timers do not fire.
    void runPending()
    {
        auto ran = std::size_t { 0 };
        while (!_jobs.empty())
        {
            REQUIRE(ran++ < MaxJobs);
            auto job = std::move(_jobs.front());
            _jobs.pop_front();
            run(job);
        }
    }

    /// Moves the clock forward by @p delta. Every timer that falls due on the way fires at its due time,
    /// earliest first, followed by the jobs it posted.
    void advance(core::platform::SteadyDuration delta)
    {
        runPending();
        auto const end = _clock.now() + delta;
        auto fired = std::size_t { 0 };
        while (true)
        {
            // Timers that keep re-arming at the same instant would never let time pass.
            REQUIRE(fired++ < MaxJobs);
            auto const next = std::ranges::min_element(_timers, {}, [](auto const& t) { return t.due; });
            if (next == _timers.end() || next->due > end)
                break;
            _clock.setNow(std::max(_clock.now(), next->due));
            auto job = std::move(next->job);
            _timers.erase(next);
            run(job);
            runPending();
        }
        _clock.setNow(end);
    }

    /// @return How many timers are waiting.
    [[nodiscard]] std::size_t pendingTimers() const noexcept { return _timers.size(); }

    std::size_t lanesAssigned = 0; ///< How many runners asked for a lane.

  private:
    /// How many jobs, or timers, one pump may run: a runaway that keeps posting would hang the test,
    /// and this bound is far above any real pump.
    static constexpr auto MaxJobs = std::size_t { 100'000 };

    struct Timer
    {
        core::platform::SteadyTimePoint due;
        TimerId id {};
        std::function<void()> job;
    };

    /// Runs @p job with the thread prepared, if the backend can prepare it: a thread it cannot prepare still
    /// runs the job, and a runner finds out that it cannot instantiate, as on a pool's worker.
    void run(std::function<void()> const& job)
    {
        auto const guard = _backend.enterThread();
        job();
    }

    wasm::IWasmBackend& _backend;
    core::platform::ManualClock _clock;
    std::deque<std::function<void()>> _jobs;
    std::vector<Timer> _timers;
    uint64_t _nextTimer = 1;
};

} // namespace vtextension::testing
