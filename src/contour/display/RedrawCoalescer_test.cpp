// SPDX-License-Identifier: Apache-2.0
#include <contour/display/RedrawCoalescer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <ranges>
#include <thread>
#include <vector>

using contour::display::RedrawCoalescer;
using contour::display::RedrawPost;
using contour::display::RedrawState;

TEST_CASE("RedrawCoalescer.onlyTheFirstRequestPosts", "[display][redraw]")
{
    auto coalescer = RedrawCoalescer {};

    CHECK(coalescer.arm() == RedrawPost::Needed);
    CHECK(coalescer.arm() == RedrawPost::NotNeeded);
    CHECK(coalescer.arm() == RedrawPost::NotNeeded);
    CHECK(coalescer.state() == RedrawState::Queued);
}

TEST_CASE("RedrawCoalescer.aRequestAfterTheRedrawStartedPostsAgain", "[display][redraw]")
{
    auto coalescer = RedrawCoalescer {};
    REQUIRE(coalescer.arm() == RedrawPost::Needed);

    coalescer.disarm(); // the posted redraw starts running

    CHECK(coalescer.state() == RedrawState::Idle);
    CHECK(coalescer.arm() == RedrawPost::Needed);
}

TEST_CASE("RedrawCoalescer.disarmingWithNothingQueuedIsHarmless", "[display][redraw]")
{
    // assignSession() disarms whatever the state; with nothing queued that must change nothing.
    auto coalescer = RedrawCoalescer {};
    coalescer.disarm();
    CHECK(coalescer.arm() == RedrawPost::Needed);
}

TEST_CASE("RedrawCoalescer.simultaneousFirstRequestsPostExactlyOnce", "[display][redraw][threading]")
{
    // Each round starts from Idle and releases every thread at once, so a check-then-act arm() would
    // tell two of them to post. The threads spin, hence never more of them than there are cores.
    //
    // hardware_concurrency() reports the host's cores, not what a container's quota or affinity leaves
    // this process, and on one CPU every round waits for the peer to be scheduled. So the clock bounds
    // the run: no round starts after the deadline, and a run that managed too few rounds is skipped.
    if (std::thread::hardware_concurrency() < 2)
        SKIP("needs two cores");
    constexpr auto Threads = 2;
    constexpr auto Rounds = 100'000;
    // A load-then-store arm() goes wrong in about 3 rounds in 100 on two real cores, so 10'000 rounds
    // expose it with certainty, while a one-CPU box still reaches that inside the deadline.
    constexpr auto MinimumMeaningfulRounds = 10'000;
    constexpr auto Deadline = std::chrono::seconds(10);
    auto const deadline = std::chrono::steady_clock::now() + Deadline;
    auto coalescer = RedrawCoalescer {};
    auto round = std::atomic<int> { 0 };
    auto finished = std::atomic<int> { 0 };
    auto posts = std::atomic<int> { 0 };
    auto badRounds = std::atomic<int> { 0 };
    auto completedRounds = std::atomic<int> { 0 };
    auto stopped = std::atomic<bool> { false };
    {
        auto threads = std::vector<std::jthread> {};
        for ([[maybe_unused]] auto const thread: std::views::iota(0, Threads))
            threads.emplace_back([&] {
                for (auto const r: std::views::iota(1, Rounds + 1))
                {
                    while (round.load(std::memory_order_acquire) != r)
                    {
                        // the start line, or the end of the run: both threads are past arm() by then
                        if (stopped.load(std::memory_order_acquire))
                            return;
                    }
                    if (coalescer.arm() == RedrawPost::Needed)
                        posts.fetch_add(1, std::memory_order_relaxed);
                    if (finished.fetch_add(1, std::memory_order_acq_rel) == Threads - 1)
                    {
                        // The last one in judges the round; then the queued redraw "starts".
                        if (posts.exchange(0, std::memory_order_relaxed) != 1)
                            badRounds.fetch_add(1, std::memory_order_relaxed);
                        coalescer.disarm();
                        finished.store(0, std::memory_order_relaxed);
                        completedRounds.store(r, std::memory_order_relaxed);
                        if (std::chrono::steady_clock::now() >= deadline)
                            stopped.store(true, std::memory_order_release);
                        else
                            round.store(r + 1, std::memory_order_release);
                    }
                }
            });
        round.store(1, std::memory_order_release);
    }
    if (badRounds.load() != 0)
        FAIL(badRounds.load() << " of " << completedRounds.load() << " rounds posted other than once");
    if (completedRounds.load() < MinimumMeaningfulRounds)
        SKIP("only " << completedRounds.load() << " of " << Rounds << " rounds ran in " << Deadline.count()
                     << " s; too few to judge");
}
