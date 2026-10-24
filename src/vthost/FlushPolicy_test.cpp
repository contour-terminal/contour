// SPDX-License-Identifier: Apache-2.0

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

#include <vthost/FlushPolicy.hpp>

using vthost::decideFlush;
using vthost::flushDeadline;
using vthost::FlushInputs;
using vthost::FlushPolicy;
using vthost::FlushReason;
using vthost::IncrementHistory;

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
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(decideFlush(FlushPolicy {}, row.in) == row.expected);
    }
}

TEST_CASE("flushDeadline is now for an immediate reason and now + 20 ms to coalesce", "[vthost][flush]")
{
    using namespace std::chrono_literals;
    auto const t0 = core::platform::SteadyTimePoint {} + 1s;
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::Echo, t0) == t0);
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::FirstOutput, t0) == t0);
    CHECK(flushDeadline(FlushPolicy {}, FlushReason::Coalesce, t0) == t0 + 20ms);
}
