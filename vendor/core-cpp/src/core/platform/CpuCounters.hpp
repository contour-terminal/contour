// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace core::platform
{

/// A CPU work measure a counter source may be able to report.
///
/// What each platform reports (this process, every thread of it; privilege mode per row):
///
/// | Platform | Instructions | Cycles | ReferenceCycles | Scope                         |
/// |----------|--------------|--------|-----------------|-------------------------------|
/// | Linux    | yes          | yes    | no              | user mode only (perf_event)   |
/// | macOS    | yes          | yes    | no              | user and kernel (proc_pid_rusage) |
/// | Windows  | no           | no     | yes             | user and kernel (QueryProcessCycleTime) |
/// | other    | no           | no     | no              | -                             |
///
/// Linux needs a permissive perf_event_paranoid and a hardware PMU, so it too may report nothing; macOS
/// reports nothing under virtualization. Every unsupported measure says why.
enum class CpuCounter : std::uint8_t
{
    Instructions,    ///< Retired instructions.
    Cycles,          ///< Core clock cycles, which follow frequency scaling.
    ReferenceCycles, ///< Ticks of a constant-rate clock accounted to the CPU time of the process; not core
                     ///< cycles.
    Last, ///< Not a measure: the count of them, which sets the extent of every table indexed by CpuCounter.
};

/// The number of measures.
inline constexpr std::size_t CpuCounterCount = std::to_underlying(CpuCounter::Last);

/// Every CpuCounter, in enumerator order; the index of a measure in CpuCounterSample::values.
inline constexpr std::array<CpuCounter, CpuCounterCount> AllCpuCounters {
    CpuCounter::Instructions,
    CpuCounter::Cycles,
    CpuCounter::ReferenceCycles,
};

namespace detail
{
    /// True when AllCpuCounters lists every enumerator before Last, in order, so a counter's value is its
    /// index.
    [[nodiscard]] constexpr bool cpuCountersAreInOrder() noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, AllCpuCounters.size()))
            if (std::to_underlying(AllCpuCounters[index]) != index)
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::cpuCountersAreInOrder(),
              "AllCpuCounters must list the CpuCounter enumerators in order: it is the index of "
              "CpuCounterSample::values");

/// Returns the human-readable name of a measure.
/// @param counter The measure.
/// @return "instructions", "cycles" or "reference cycles".
[[nodiscard]] constexpr std::string_view toString(CpuCounter counter) noexcept
{
    switch (counter)
    {
        case CpuCounter::Instructions: return "instructions";
        case CpuCounter::Cycles: return "cycles";
        case CpuCounter::ReferenceCycles: return "reference cycles";
        case CpuCounter::Last: break;
    }
    return "unknown";
}

/// One reading of every measure; a measure the source cannot report is absent.
struct CpuCounterSample
{
    /// Returns the value of a measure, or nothing when it was not reported.
    /// @param counter The measure.
    /// @return The value; nothing for a measure not reported or for CpuCounter::Last.
    [[nodiscard]] constexpr std::optional<std::uint64_t> get(CpuCounter counter) const noexcept
    {
        auto const index = std::to_underlying(counter);
        return index < CpuCounterCount ? values[index] : std::nullopt;
    }

    /// Records the value of a measure.
    /// @param counter The measure.
    /// @param value Its value. Ignored for CpuCounter::Last, which is not a measure.
    constexpr void set(CpuCounter counter, std::uint64_t value) noexcept
    {
        if (auto const index = std::to_underlying(counter); index < CpuCounterCount)
            values[index] = value;
    }

    /// The measures, indexed by CpuCounter.
    std::array<std::optional<std::uint64_t>, CpuCounterCount> values {};

    [[nodiscard]] bool operator==(CpuCounterSample const&) const = default;
};

/// The work done between two samples.
/// @param end The later sample.
/// @param start The earlier sample.
/// @return A sample whose measure is present only if present in both and end >= start.
[[nodiscard]] constexpr CpuCounterSample operator-(CpuCounterSample const& end,
                                                   CpuCounterSample const& start) noexcept
{
    auto delta = CpuCounterSample {};
    for (auto const counter: AllCpuCounters)
    {
        auto const last = end.get(counter);
        auto const first = start.get(counter);
        if (last && first && *last >= *first)
            delta.set(counter, *last - *first);
    }
    return delta;
}

/// Where CPU work counts come from: the operating system's counters, or a test double.
class ICpuCounterSource
{
  public:
    ICpuCounterSource() = default;
    ICpuCounterSource(ICpuCounterSource const&) = delete;
    ICpuCounterSource(ICpuCounterSource&&) = delete;
    ICpuCounterSource& operator=(ICpuCounterSource const&) = delete;
    ICpuCounterSource& operator=(ICpuCounterSource&&) = delete;
    virtual ~ICpuCounterSource() = default;

    /// Reads the counters now; unsupported measures are absent. A measure that is supported can still be
    /// absent from one sample (on Linux when the kernel multiplexed it away over the interval since the
    /// previous read), and then so is its delta. That check covers only the interval between two consecutive
    /// read() calls of one source; a delta between non-consecutive samples (an outer measure() around a
    /// nested one on the same source) is not checked and may undercount, so use one source per measured
    /// region and do not nest measure() on one source. Calls must not overlap: a source keeps state between
    /// reads.
    [[nodiscard]] virtual CpuCounterSample read() = 0;

    /// Why a measure is not reported.
    /// @param counter The measure.
    /// @return Empty when the measure is supported. The view lives as long as the source.
    [[nodiscard]] virtual std::string_view unavailableReason(CpuCounter counter) const noexcept = 0;

    /// A runtime-detected deviation from the scope documented on CpuCounter (e.g. "child processes are
    /// counted (kernel < 5.13)"); empty when the source counts what is documented, which is the usual case.
    [[nodiscard]] virtual std::string_view note() const noexcept = 0;

    /// Whether a measure is reported; derived from unavailableReason(), so they cannot disagree.
    /// @param counter The measure.
    [[nodiscard]] bool supports(CpuCounter counter) const noexcept
    {
        return unavailableReason(counter).empty();
    }
};

/// Creates the platform's counter source.
/// The platform support table is on CpuCounter. In short: Linux counts instructions and cycles in user mode
/// only, macOS the same two in user and kernel mode together, Windows reference cycles in user and kernel
/// mode together; so figures from different platforms are not comparable.
/// Create it before starting worker threads: on Linux the counters follow threads started after the source
/// was opened, and miss the ones that were already running.
/// @return A source; never null, never throws. Where the platform cannot count, every measure is
///         unsupported and says why.
[[nodiscard]] std::unique_ptr<ICpuCounterSource> makeDefaultCpuCounterSource();

/// Runs work and returns the counters it spent.
/// @param source Where the counts come from.
/// @param work Called exactly once, between two reads.
/// @return The delta between the two reads.
template <std::invocable Work>
[[nodiscard]] CpuCounterSample measure(ICpuCounterSource& source, Work&& work)
{
    auto const start = source.read();
    std::forward<Work>(work)();
    auto const end = source.read();
    return end - start;
}

} // namespace core::platform
