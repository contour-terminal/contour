// SPDX-License-Identifier: Apache-2.0

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

#include <vthost/FlushPolicy.hpp>

using vthost::ActivityMark;
using vthost::CursorVisibility;
using vthost::decideFlush;
using vthost::flushDeadline;
using vthost::FlushInputs;
using vthost::FlushPolicy;
using vthost::FlushReason;
using vthost::IncrementHistory;
using vthost::MarkPace;
using vthost::OutputReach;

TEST_CASE("decideFlush follows the echo, first-output and coalesce rules", "[vthost][flush]")
{
    using namespace std::chrono_literals;
    auto const t0 = core::platform::SteadyTimePoint {} + 1s;
    struct Row
    {
        std::string_view name;
        FlushInputs in;
        FlushReason expected;
    };
    auto const rows = std::array {
        Row { "no input, increments already sent",
              { t0, std::nullopt, std::nullopt, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Coalesce },
        Row { "first increment after a snapshot",
              { t0, std::nullopt, std::nullopt, IncrementHistory::NoneSinceSnapshot },
              FlushReason::FirstOutput },
        Row { "input 3 ms ago, unanswered",
              { t0, t0 - 3ms, std::nullopt, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Echo },
        Row { "input exactly 50 ms ago",
              { t0, t0 - 50ms, std::nullopt, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Echo },
        Row { "input 51 ms ago",
              { t0, t0 - 51ms, std::nullopt, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Coalesce },
        Row { "input 3 ms ago, already answered",
              { t0, t0 - 3ms, t0 - 3ms, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Coalesce },
        Row { "a newer input than the answered one",
              { t0, t0 - 1ms, t0 - 9ms, IncrementHistory::SomeSinceSnapshot },
              FlushReason::Echo },
        Row { "echo wins over first output",
              { t0, t0 - 1ms, std::nullopt, IncrementHistory::NoneSinceSnapshot },
              FlushReason::Echo },
        Row { "input 1 ms ago, unanswered, cursor hidden: a redraw under way",
              { t0, t0 - 1ms, std::nullopt, IncrementHistory::SomeSinceSnapshot, CursorVisibility::Hidden },
              FlushReason::AwaitRedraw },
        Row { "a redraw under way waits even as the first output",
              { t0, t0 - 1ms, std::nullopt, IncrementHistory::NoneSinceSnapshot, CursorVisibility::Hidden },
              FlushReason::AwaitRedraw },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(decideFlush(FlushPolicy {}, row.in) == row.expected);
    }
}

TEST_CASE("flushDeadline is now for an immediate reason, now + 5 ms for a redraw, now + 20 ms to coalesce",
          "[vthost][flush]")
{
    using namespace std::chrono_literals;
    auto const t0 = core::platform::SteadyTimePoint {} + 1s;
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::Echo, t0) == t0);
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::FirstOutput, t0) == t0);
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::AwaitRedraw, t0) == t0 + 5ms);
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::Coalesce, t0) == t0 + 20ms);
}

TEST_CASE("ActivityMark tells main-display output from a status-line rewrite", "[vthost][flush]")
{
    auto mark = ActivityMark {};
    CHECK(mark.look(0) == OutputReach::StatusLineOnly);
    CHECK(mark.look(3) == OutputReach::MainDisplay);
    CHECK(mark.look(3) == OutputReach::StatusLineOnly);
    CHECK(mark.look(4) == OutputReach::MainDisplay);
}

TEST_CASE("ActivityMark marks at most once per interval", "[vthost][flush]")
{
    using namespace std::chrono_literals;
    auto const t0 = core::platform::SteadyTimePoint {} + 1s;
    auto mark = ActivityMark {};
    CHECK(mark.mark(t0, 1s) == MarkPace::Marked);
    CHECK(mark.mark(t0 + 999ms, 1s) == MarkPace::TooSoon);
    CHECK(mark.markedAt == t0);
    CHECK(mark.mark(t0 + 1s, 1s) == MarkPace::Marked);
    CHECK(mark.markedAt == t0 + 1s);
}

TEST_CASE("ActivityMark owes held output a trailing mark as the interval ends", "[vthost][flush]")
{
    using namespace std::chrono_literals;
    auto const t0 = core::platform::SteadyTimePoint {} + 1s;
    auto mark = ActivityMark {};
    REQUIRE(mark.mark(t0, 1s) == MarkPace::Marked);
    CHECK(mark.trailingDue(1s) == std::nullopt);

    REQUIRE(mark.mark(t0 + 300ms, 1s) == MarkPace::TooSoon);
    CHECK(mark.trailingDue(1s) == std::optional { t0 + 1s });

    // The trailing mark is a mark: it owes nothing more.
    REQUIRE(mark.mark(t0 + 1s, 1s) == MarkPace::Marked);
    CHECK(mark.trailingDue(1s) == std::nullopt);
}
