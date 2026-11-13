// SPDX-License-Identifier: Apache-2.0
//
// OffThreadJobs: a job's result comes back to the thread that ran it, and the workers that ended are joined
// as the next job starts rather than kept for the owner's lifetime.

#include <contour/platform/OffThreadJobs.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>

namespace
{
/// Runs the GUI thread's events until @p predicate holds, for up to two seconds.
[[nodiscard]] bool deliveredWhen(std::function<bool()> const& predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST_CASE("OffThreadJobs runs a job off the thread and hands its result back to it",
          "[contour][qt][threading]")
{
    auto jobs = contour::platform::OffThreadJobs {};
    auto const gui = std::this_thread::get_id();
    auto ranOn = std::thread::id {};
    auto deliveredOn = std::thread::id {};
    auto delivered = std::string {};

    jobs.run(
        [&ranOn] {
            ranOn = std::this_thread::get_id();
            return std::string { "answer" };
        },
        [&](std::string result) {
            deliveredOn = std::this_thread::get_id();
            delivered = std::move(result);
        });

    REQUIRE(deliveredWhen([&] { return !delivered.empty(); }));
    CHECK(delivered == "answer");
    CHECK(deliveredOn == gui);
    CHECK(ranOn != gui);
}

// Review (PR 3, item 6): the switcher popup kept every discovery and daemon start it ever ran.
TEST_CASE("OffThreadJobs joins the workers that ended as the next job starts", "[contour][qt][threading]")
{
    auto jobs = contour::platform::OffThreadJobs {};
    auto delivered = 0;
    for ([[maybe_unused]] auto const each: std::views::iota(0, 5))
    {
        jobs.run([] { return 1; }, [&delivered](int one) { delivered += one; });
        auto const expected = delivered + 1;
        REQUIRE(deliveredWhen([&] { return delivered == expected; }));
    }
    // Each delivered, so each ended before the next started: only the last is kept.
    CHECK(jobs.workerCount() <= 2);
    jobs.run([] { return 1; }, [&delivered](int one) { delivered += one; });
    REQUIRE(deliveredWhen([&] { return delivered == 6; }));
    CHECK(jobs.workerCount() <= 2);
}

TEST_CASE("OffThreadJobs stops a job that takes a stop token as it is destroyed", "[contour][qt][threading]")
{
    auto stopped = std::atomic<bool> { false };
    auto delivered = false;
    auto const started = std::chrono::steady_clock::now();
    {
        auto jobs = contour::platform::OffThreadJobs {};
        jobs.run(
            [&stopped](std::stop_token const& stop) {
                // Waits for its owner to go, as a listing waits on the network or on the user.
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
                stopped = true;
                return 0;
            },
            [&delivered](int) { delivered = true; });
    }
    // The destruction asked it to stop and waited for it: it ended, promptly, before the destruction did.
    CHECK(stopped);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds { 5 });
    QCoreApplication::processEvents();
    CHECK_FALSE(delivered); // nobody is left to deliver to
}
