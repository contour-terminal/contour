// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <ranges>
#include <vector>

#include <vthost/latency/KeystrokeProbe.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

TEST_CASE("the probe measures from key to the first frame drawing its echo", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock {};
    auto probe =
        KeystrokeProbe { clock,
                         ProbePlan { .warmupKeys = 1, .measuredKeys = 2, .minGap = 30ms, .maxGap = 30ms } };
    probe.frameSwapped(0);
    CHECK(probe.phase() == ProbePhase::AwaitingPrompt);
    clock.advance(5ms);
    probe.frameSwapped(PromptColumns);
    CHECK(probe.result().firstPrompt == core::platform::SteadyTimePoint {} + 5ms);
    for (auto const i: std::views::iota(std::size_t { 0 }, std::size_t { 3 }))
    {
        CHECK_FALSE(probe.takeDueKey().has_value()); // gap not elapsed
        clock.advance(30ms);
        CHECK(probe.takeDueKey() == keyFor(i));
        clock.advance(4ms);
        probe.frameSwapped(expectedCursorColumn(i) - 1); // stale frame: ignored
        clock.advance(3ms);
        probe.frameSwapped(expectedCursorColumn(i));
    }
    CHECK(probe.phase() == ProbePhase::Finished);
    CHECK(probe.result().keystrokes == std::vector<core::platform::SteadyDuration> { 7ms, 7ms });
}

TEST_CASE("a frame that draws no cursor is ignored", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock {};
    auto probe = KeystrokeProbe { clock, ProbePlan {} };
    probe.frameSwapped(std::nullopt);
    CHECK(probe.phase() == ProbePhase::AwaitingPrompt);
}

TEST_CASE("the gaps stay in range and repeat for a seed", "[vthost][latency]")
{
    auto const run = [](uint32_t seed) {
        auto clock = core::platform::ManualClock {};
        auto probe =
            KeystrokeProbe { clock, ProbePlan { .warmupKeys = 0, .measuredKeys = 20, .seed = seed } };
        probe.frameSwapped(PromptColumns);
        auto gaps = std::vector<core::platform::SteadyDuration> {};
        for (auto const i: std::views::iota(std::size_t { 0 }, std::size_t { 20 }))
        {
            auto waited = core::platform::SteadyDuration {};
            while (!probe.takeDueKey())
            {
                clock.advance(1ms);
                waited += 1ms;
            }
            gaps.push_back(waited);
            probe.frameSwapped(expectedCursorColumn(i));
        }
        return gaps;
    };
    auto const gaps = run(7);
    CHECK(gaps == run(7));
    for (auto const gap: gaps)
    {
        CHECK(gap >= 30ms);
        CHECK(gap <= 60ms);
    }
}

TEST_CASE("a probe whose echo never shows times out naming the awaited column", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock {};
    auto probe = KeystrokeProbe { clock, ProbePlan { .minGap = 0ms, .maxGap = 0ms } };
    probe.frameSwapped(PromptColumns);
    REQUIRE(probe.takeDueKey() == keyFor(0));
    clock.advance(2001ms);
    CHECK_FALSE(probe.takeDueKey().has_value());
    CHECK(probe.phase() == ProbePhase::EchoTimedOut);
    CHECK(probe.result().awaitedColumn == 3);
}

TEST_CASE("a probe whose prompt never shows times out", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock {};
    auto probe = KeystrokeProbe { clock, ProbePlan {} };
    clock.advance(10001ms);
    CHECK_FALSE(probe.takeDueKey().has_value());
    CHECK(probe.phase() == ProbePhase::PromptTimedOut);
    CHECK(probe.result().awaitedColumn == PromptColumns);
}
