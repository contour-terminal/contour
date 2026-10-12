// SPDX-License-Identifier: Apache-2.0
#include <contour/display/RedrawCoalescer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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
    if (std::thread::hardware_concurrency() < 2)
        SKIP("needs two cores");
    constexpr auto Threads = 2;
    constexpr auto Rounds = 100'000;
    auto coalescer = RedrawCoalescer {};
    auto round = std::atomic<int> { 0 };
    auto finished = std::atomic<int> { 0 };
    auto posts = std::atomic<int> { 0 };
    auto badRounds = std::atomic<int> { 0 };
    {
        auto threads = std::vector<std::jthread> {};
        for ([[maybe_unused]] auto const thread: std::views::iota(0, Threads))
            threads.emplace_back([&] {
                for (auto const r: std::views::iota(1, Rounds + 1))
                {
                    while (round.load(std::memory_order_acquire) != r)
                    {
                        // the start line
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
                        round.store(r + 1, std::memory_order_release);
                    }
                }
            });
        round.store(1, std::memory_order_release);
    }
    CHECK(badRounds.load() == 0);
}
