// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include <vthost/latency/Gate.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

namespace
{
[[nodiscard]] std::vector<core::platform::SteadyDuration> filled(std::size_t count,
                                                                 core::platform::SteadyDuration value)
{
    return std::vector(count, value);
}
} // namespace

TEST_CASE("judge passes at the budget, fails above it, and refuses thin samples", "[vthost][latency]")
{
    auto const base = ModeSamples { filled(10, 100ms), filled(200, 5ms) };
    auto const atBudget = judge(Budgets {}, base, ModeSamples { filled(10, 110ms), filled(200, 6ms) });
    CHECK(atBudget.startup == Verdict::Pass);
    CHECK(atBudget.keystroke == Verdict::Pass);
    auto const over = judge(Budgets {}, base, ModeSamples { filled(10, 111ms), filled(200, 6ms + 1us) });
    CHECK(over.startup == Verdict::Fail);
    CHECK(over.keystroke == Verdict::Fail);
    CHECK(over.startupDelta == 11ms);
    auto const thin = judge(Budgets {}, base, ModeSamples { filled(9, 100ms), filled(199, 5ms) });
    CHECK(thin.startup == Verdict::Inconclusive);
    CHECK(thin.keystroke == Verdict::Inconclusive);
}

TEST_CASE("a faster daemon path passes with a negative delta", "[vthost][latency]")
{
    auto const base = ModeSamples { filled(10, 100ms), filled(200, 5ms) };
    auto const result = judge(Budgets {}, base, ModeSamples { filled(10, 90ms), filled(200, 4ms) });
    CHECK(result.startup == Verdict::Pass);
    CHECK(result.startupDelta == -10ms);
    CHECK(result.keystroke == Verdict::Pass);
}

TEST_CASE("thin samples on the in-process side are inconclusive too", "[vthost][latency]")
{
    auto const result = judge(Budgets {},
                              ModeSamples { filled(9, 100ms), filled(199, 5ms) },
                              ModeSamples { filled(10, 100ms), filled(200, 5ms) });
    CHECK(result.startup == Verdict::Inconclusive);
    CHECK(result.keystroke == Verdict::Inconclusive);
}
