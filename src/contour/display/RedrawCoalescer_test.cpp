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

TEST_CASE("RedrawCoalescer.concurrentRequestsPostExactlyOnce", "[display][redraw][threading]")
{
    // The parser thread and the GUI thread's cursor-blink timer both request redraws: however they
    // interleave, exactly one of them may post while nothing has disarmed.
    auto coalescer = RedrawCoalescer {};
    auto posts = std::atomic<int> { 0 };
    {
        auto threads = std::vector<std::jthread> {};
        for ([[maybe_unused]] auto const thread: std::views::iota(0, 4))
            threads.emplace_back([&coalescer, &posts] {
                for ([[maybe_unused]] auto const request: std::views::iota(0, 10'000))
                    if (coalescer.arm() == RedrawPost::Needed)
                        posts.fetch_add(1, std::memory_order_relaxed);
            });
    }
    CHECK(posts.load() == 1);
}
