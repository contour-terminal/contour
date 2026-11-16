// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/TimerUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

using core::ManualClock;
using core::SteadyTimePoint;
using core::tui::remainingMs;

using namespace std::chrono_literals;

TEST_CASE("TimerUtils.remainingMs: an optional timer, measured on the injected clock", "[timer]")
{
    struct Row
    {
        std::string_view name;
        std::optional<std::chrono::milliseconds> startedAgo; ///< nullopt: the timer is not active.
        std::chrono::milliseconds timeout;
        int expected;
    };
    auto const rows = std::array {
        Row { .name = "not active", .startedAgo = std::nullopt, .timeout = 500ms, .expected = -1 },
        Row { .name = "just started", .startedAgo = 0ms, .timeout = 500ms, .expected = 500 },
        Row { .name = "part elapsed", .startedAgo = 120ms, .timeout = 500ms, .expected = 380 },
        Row { .name = "one millisecond left", .startedAgo = 499ms, .timeout = 500ms, .expected = 1 },
        Row { .name = "expired exactly", .startedAgo = 500ms, .timeout = 500ms, .expected = 0 },
        Row { .name = "long expired", .startedAgo = 10'000ms, .timeout = 500ms, .expected = 0 },
        Row { .name = "seconds timeout", .startedAgo = 250ms, .timeout = 1000ms, .expected = 750 },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto clock = ManualClock { SteadyTimePoint { 1h } };
        auto start = std::optional<SteadyTimePoint> {};
        if (row.startedAgo)
        {
            start = clock.now();
            clock.advance(*row.startedAgo);
        }
        CHECK(remainingMs(clock, start, row.timeout) == row.expected);
        if (start)
            CHECK(remainingMs(clock, *start, row.timeout) == row.expected); // The time-point overload agrees.
    }
}

TEST_CASE("TimerUtils.remainingMs: a sub-millisecond remainder truncates, other duration types convert",
          "[timer]")
{
    auto clock = ManualClock {};
    auto const start = std::optional { clock.now() };
    clock.advance(std::chrono::microseconds { 1500 });

    CHECK(remainingMs(clock, start, std::chrono::seconds { 1 }) == 998);
    CHECK(remainingMs(clock, start, std::chrono::microseconds { 2000 }) == 0);
}
