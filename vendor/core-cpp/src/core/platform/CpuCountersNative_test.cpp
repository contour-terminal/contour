// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

using core::platform::AllCpuCounters;
using core::platform::CpuCounter;
using core::platform::CpuCounterSample;
using core::platform::ICpuCounterSource;
using core::platform::makeDefaultCpuCounterSource;
using core::platform::measure;
using core::platform::toString;

namespace
{

constexpr std::uint64_t Iterations = 20'000'000;

/// How many small/large pairs are measured. Odd, so the median is one pair's own ratio.
constexpr auto Pairs = 3;

/// Runs a loop whose every iteration writes a volatile, so no compiler can remove or fold it.
void spin(std::uint64_t iterations)
{
    std::uint64_t volatile sink = 0;
    for (auto const index: std::views::iota(std::uint64_t { 0 }, iterations))
        sink = sink + index;
}

/// How much a measure grew when the work doubled: the median, over several pairs, of the large run's count
/// over the small run's, each pair measured back to back.
///
/// **Back to back, and compared within the pair.** A count of constant-rate ticks (Windows'
/// QueryProcessCycleTime) and a count of core cycles both measure how long the work took, not how much there
/// was, and how long it takes moves with what the rest of the machine is doing -- a busy SMT sibling, a
/// frequency step, a hypervisor taking the vCPU away. On a shared CI runner, with ctest running other test
/// binaries alongside, such a condition can last as long as all the small runs together. Taking the least of
/// all the small runs and then the least of all the large runs therefore compared two different stretches of
/// time: when the whole small stretch was slowed, every small run was inflated and the least of them too, and
/// the ratio fell below the bound although the counter was fine. Measured that way on `windows
/// (cl-release-tls)`, reference cycles: 26485464 for the small work against 33259823 for twice it, a ratio
/// of 1.26. A pair is two adjacent runs, so both see the same conditions unless a change falls between them,
/// and the median discards a pair a change did split.
///
/// A pair in which the measure is absent (Linux drops one the kernel multiplexed away meanwhile) does not
/// take part; when no pair reports it, the source broke its promise and the case fails here.
/// @param source The source under test.
/// @param counter The measure.
/// @param work Performs the given number of iterations of work.
/// @return The median ratio.
double scalingOf(ICpuCounterSource& source,
                 CpuCounter counter,
                 std::function<void(std::uint64_t)> const& work)
{
    auto ratios = std::vector<double> {};
    for ([[maybe_unused]] auto const pair: std::views::iota(0, Pairs))
    {
        auto const small = measure(source, [&] { work(Iterations); }).get(counter);
        auto const large = measure(source, [&] { work(2 * Iterations); }).get(counter);
        if (!small || !large)
            continue;
        CAPTURE(toString(counter), *small, *large);
        REQUIRE(*small > 0);
        REQUIRE(*large > 0);
        ratios.push_back(static_cast<double>(*large) / static_cast<double>(*small));
    }
    INFO(toString(counter) << " is supported, yet none of " << Pairs << " pairs reported it");
    REQUIRE_FALSE(ratios.empty());
    std::ranges::sort(ratios);
    return ratios[ratios.size() / 2];
}

/// The smallest growth a measure must show when the work doubles.
double requiredRatio(CpuCounter counter)
{
    return counter == CpuCounter::Instructions ? 1.8 : 1.3;
}

/// A machine that counts reference cycles for the work it is given, at a cost the test chooses per run --
/// so a scenario says what each run of work costs and when, whatever order the runs are measured in.
class SimulatedSource final: public ICpuCounterSource
{
  public:
    /// What one run of work counts.
    /// @param iterations The work the run was given.
    /// @param run The run's position among all runs on this source, from 0.
    using Cost = std::function<std::uint64_t(std::uint64_t iterations, int run)>;

    /// @param cost What each run of work counts.
    explicit SimulatedSource(Cost cost): _cost { std::move(cost) } {}

    [[nodiscard]] CpuCounterSample read() override
    {
        auto sample = CpuCounterSample {};
        sample.set(CpuCounter::ReferenceCycles, _count);
        return sample;
    }

    [[nodiscard]] std::string_view unavailableReason(CpuCounter counter) const noexcept override
    {
        return counter == CpuCounter::ReferenceCycles ? std::string_view {} : "simulated: unsupported";
    }

    [[nodiscard]] std::string_view note() const noexcept override { return {}; }

    /// Performs one run of work: advances the count by what the scenario says this run costs.
    /// @param iterations The work.
    void work(std::uint64_t iterations) { _count += _cost(iterations, _runs++); }

  private:
    Cost _cost;
    std::uint64_t _count = 0;
    int _runs = 0;
};

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
        auto const ratio = scalingOf(*source, counter, spin);
        CAPTURE(toString(counter), ratio);
        CHECK(ratio >= requiredRatio(counter));
    }
}

TEST_CASE("the scaling check survives a slowed stretch of runs, not a counter that does not scale",
          "[cpucounters]")
{
    // 100 ticks per Iterations of work, except that the first three runs were slowed by 1.6x -- the ratio
    // CI saw. Measured all small runs first and all large ones after, those three are three small runs:
    // least small 160, least large 200, a ratio of 1.25 and a failure. Paired, the slowdown covers the first
    // pair whole and splits the second; the third is untouched, so the median is 2.
    SECTION("a slowed stretch")
    {
        constexpr auto SlowedRuns = 3;
        auto source = SimulatedSource { [](std::uint64_t iterations, int run) {
            auto const ticks = 100 * iterations / Iterations;
            return run < SlowedRuns ? ticks * 16 / 10 : ticks;
        } };
        CHECK(scalingOf(source, CpuCounter::ReferenceCycles, [&](std::uint64_t n) { source.work(n); })
              == 2.0);
    }
    // A counter that reports the same however much work it is given must still fail.
    SECTION("a counter that does not scale")
    {
        auto source = SimulatedSource { [](std::uint64_t, int) { return std::uint64_t { 100 }; } };
        CHECK(scalingOf(source, CpuCounter::ReferenceCycles, [&](std::uint64_t n) { source.work(n); })
              < requiredRatio(CpuCounter::ReferenceCycles));
    }
}
