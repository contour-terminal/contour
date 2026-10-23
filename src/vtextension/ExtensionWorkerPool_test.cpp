// SPDX-License-Identifier: Apache-2.0
//
// The production scheduler: lazily started, lanes pinned to one thread each, timers, and a destructor
// that runs what was posted and joins.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <ranges>
#include <set>
#include <thread>
#include <vector>

#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/testing/LaneReport.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>

using namespace vtextension;
using namespace std::chrono_literals;
using vtextension::testing::Eventually;
using vtextension::testing::reportFromLane;

TEST_CASE("ExtensionWorkerPool.workerCount", "[vtextension]")
{
    CHECK(extensionWorkerCount(0) == 1);
    CHECK(extensionWorkerCount(1) == 1);
    CHECK(extensionWorkerCount(3) == 3);
    CHECK(extensionWorkerCount(64) == MaxExtensionWorkers);
}

TEST_CASE("ExtensionWorkerPool.lazy", "[vtextension][threading]")
{
    auto const backend = wasm::makeDefaultBackend();
    auto pool = ExtensionWorkerPool(*backend, 2);
    CHECK(pool.runningWorkers() == 0);
    [[maybe_unused]] auto const lane = pool.assignLane();
    CHECK(pool.runningWorkers() == 2);
}

TEST_CASE("ExtensionWorkerPool.lanesArePinned", "[vtextension][threading]")
{
    // A lane is prepared for the runtime an engine keeps up, as a runner's engine does while its jobs run.
    auto const backend = wasm::makeDefaultBackend();
    auto const engine = Engine::create(*backend, ExtensionLimits {});
    REQUIRE(engine.has_value());
    auto pool = ExtensionWorkerPool(*backend, 2);
    auto const first = pool.assignLane();
    auto const second = pool.assignLane();
    CHECK(first != second);
    CHECK(pool.assignLane() == first); // round-robin

    // Every job of one lane runs on one thread, prepared to call into instances.
    auto mutex = std::mutex {};
    auto threads = std::set<std::thread::id> {};
    auto prepared = std::size_t { 0 };
    auto done = std::promise<void> {};
    constexpr auto Jobs = 20;
    for (auto const index: std::views::iota(0, Jobs))
        pool.post(first, [&, index] {
            auto const lock = std::scoped_lock { mutex };
            threads.insert(std::this_thread::get_id());
            prepared += backend->isCurrentThreadPrepared() ? 1 : 0;
            if (index == Jobs - 1)
                done.set_value();
        });
    REQUIRE(done.get_future().wait_for(Eventually) == std::future_status::ready);
    auto const lock = std::scoped_lock { mutex };
    CHECK(threads.size() == 1);
    CHECK(prepared == Jobs);
    CHECK_FALSE(threads.contains(std::this_thread::get_id()));
}

TEST_CASE("ExtensionWorkerPool.timers", "[vtextension][threading]")
{
    auto const backend = wasm::makeDefaultBackend();
    auto pool = ExtensionWorkerPool(*backend, 1);
    auto const lane = pool.assignLane();
    auto mutex = std::mutex {};
    auto fired = std::vector<int> {};
    auto done = std::promise<void> {};

    auto const cancelled = pool.schedule(lane, 1ms, [&] {
        auto const lock = std::scoped_lock { mutex };
        fired.push_back(0);
    });
    pool.cancel(cancelled);
    [[maybe_unused]] auto const timer = pool.schedule(lane, 1ms, [&] {
        auto const lock = std::scoped_lock { mutex };
        fired.push_back(1);
        done.set_value();
    });
    REQUIRE(done.get_future().wait_for(Eventually) == std::future_status::ready);
    auto const lock = std::scoped_lock { mutex };
    CHECK(fired == std::vector { 1 });
}

TEST_CASE("ExtensionWorkerPool.scheduleWhileWaiting", "[vtextension][threading]")
{
    // The worker sleeps towards its earliest timer while another thread schedules more: scheduling may move
    // the timers in memory, and the worker must not be left holding on to where the earliest one was
    // (ASan checks that it is not).
    auto const backend = wasm::makeDefaultBackend();
    auto pool = ExtensionWorkerPool(*backend, 1);
    auto const lane = pool.assignLane();
    auto timers = std::vector<TimerId> {};
    timers.push_back(pool.schedule(lane, std::chrono::hours { 1 }, [] {}));

    constexpr auto Timers = 256;
    auto scheduler = std::thread([&] {
        for (auto const index: std::views::iota(1, Timers))
            timers.push_back(pool.schedule(lane, std::chrono::hours { 1 + index }, [] {}));
    });
    scheduler.join();
    for (auto const timer: timers)
        pool.cancel(timer);

    // The worker is still alive and well: a job posted now runs.
    auto ran = std::promise<void> {};
    pool.post(lane, [&ran] { ran.set_value(); });
    CHECK(ran.get_future().wait_for(Eventually) == std::future_status::ready);
}

TEST_CASE("ExtensionWorkerPool.destructionRunsPostedJobs", "[vtextension][threading]")
{
    auto ran = std::vector<int> {};
    auto timerRan = false;
    auto blocker = std::promise<void> {};
    auto const backend = wasm::makeDefaultBackend();
    {
        auto pool = ExtensionWorkerPool(*backend, 1);
        auto const lane = pool.assignLane();
        // The worker is held up, so that the jobs below are still queued when the pool is destroyed.
        pool.post(lane, [future = blocker.get_future().share()] { future.wait(); });
        pool.post(lane, [&] { ran.push_back(1); });
        pool.post(lane, [&] { ran.push_back(2); });
        [[maybe_unused]] auto const timer =
            pool.schedule(lane, std::chrono::hours { 1 }, [&] { timerRan = true; });
        blocker.set_value();
    }
    CHECK(ran == std::vector { 1, 2 });
    CHECK_FALSE(timerRan);
}

TEST_CASE("ExtensionWorkerPool.reprepareForNewRuntime", "[vtextension][threading]")
{
    // A lane prepared for one runtime is prepared again once the last engine went and a new one brought
    // the runtime up anew: on Windows the old preparation would have no state under the new runtime.
    auto const backend = wasm::makeDefaultBackend();
    auto pool = ExtensionWorkerPool(*backend, 1);
    auto const lane = pool.assignLane();

    auto first = Engine::create(*backend, ExtensionLimits {});
    REQUIRE(first.has_value());
    auto const firstGeneration = backend->runtimeGeneration();
    auto const before = reportFromLane(pool, lane, *backend);
    CHECK(before.prepared == firstGeneration);
    CHECK(before.ready);

    first->reset(); // The last engine: the runtime goes down.
    auto second = Engine::create(*backend, ExtensionLimits {});
    REQUIRE(second.has_value());
    auto const secondGeneration = backend->runtimeGeneration();
    REQUIRE(secondGeneration != firstGeneration);

    auto const after = reportFromLane(pool, lane, *backend);
    CHECK(after.prepared == secondGeneration);
    CHECK(after.ready);
}
