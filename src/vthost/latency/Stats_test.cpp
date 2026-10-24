// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <vector>

#include <vthost/latency/Stats.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

TEST_CASE("summarize uses nearest-rank quantiles", "[vthost][latency]")
{
    auto samples = std::vector<core::platform::SteadyDuration> {};
    for (auto const ms: std::views::iota(1, 101))
        samples.emplace_back(std::chrono::milliseconds { ms });
    std::ranges::reverse(samples); // order must not matter
    auto const summary = summarize(samples);
    CHECK(summary.count == 100);
    CHECK(summary.p50 == 50ms);
    CHECK(summary.p90 == 90ms);
    CHECK(summary.p99 == 99ms);
    CHECK(summary.max == 100ms);

    auto const empty = summarize({});
    CHECK(empty.count == 0);
    CHECK(empty.p50 == core::platform::SteadyDuration {});
    CHECK(empty.max == core::platform::SteadyDuration {});
}

TEST_CASE("summarize of one sample is that sample throughout", "[vthost][latency]")
{
    auto const summary = summarize({ 7ms });
    CHECK(summary.count == 1);
    CHECK(summary.p50 == 7ms);
    CHECK(summary.p99 == 7ms);
    CHECK(summary.max == 7ms);
}
