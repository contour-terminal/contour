// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>

using core::platform::AllCpuCounters;
using core::platform::CpuCounter;
using core::platform::ICpuCounterSource;
using core::platform::makeDefaultCpuCounterSource;
using core::platform::measure;
using core::platform::toString;

namespace
{

constexpr std::uint64_t Iterations = 20'000'000;
constexpr auto Runs = 3;

/// Runs a loop whose every iteration writes a volatile, so no compiler can remove or fold it.
void spin(std::uint64_t iterations)
{
    std::uint64_t volatile sink = 0;
    for (auto const index: std::views::iota(std::uint64_t { 0 }, iterations))
        sink = sink + index;
}

/// The least work a measure counted over several runs of the loop; the least is the run least disturbed.
/// A run in which a supported measure is absent (Linux drops one the kernel multiplexed away meanwhile) does
/// not take part; when no run reports it, the source broke its promise and the case fails here.
std::uint64_t bestOf(ICpuCounterSource& source, CpuCounter counter, std::uint64_t iterations)
{
    auto best = std::optional<std::uint64_t> {};
    for ([[maybe_unused]] auto const run: std::views::iota(0, Runs))
        if (auto const count = measure(source, [&] { spin(iterations); }).get(counter))
            best = best ? std::min(*best, *count) : *count;
    INFO(toString(counter) << " is supported, yet none of " << Runs << " runs reported it");
    REQUIRE(best.has_value());
    return *best;
}

/// The smallest growth a measure must show when the work doubles.
double requiredRatio(CpuCounter counter)
{
    return counter == CpuCounter::Instructions ? 1.8 : 1.3;
}

} // namespace

TEST_CASE("supported measures grow with the work", "[cpucounters]")
{
    auto const source = makeDefaultCpuCounterSource();
    auto anySupported = false;
    for (auto const counter: AllCpuCounters)
        anySupported = anySupported || source->supports(counter);
    if (!anySupported)
        SKIP(source->unavailableReason(CpuCounter::Instructions));

    for (auto const counter: AllCpuCounters)
    {
        if (!source->supports(counter))
            continue;
        auto const small = bestOf(*source, counter, Iterations);
        auto const large = bestOf(*source, counter, 2 * Iterations);
        CAPTURE(toString(counter), small, large);
        REQUIRE(small > 0);
        REQUIRE(large > 0);
        auto const ratio = static_cast<double>(large) / static_cast<double>(small);
        CHECK(ratio >= requiredRatio(counter));
    }
}
