// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/CpuCounters.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace vtbackend::testing
{

namespace detail
{
    /// The unit each CpuCounter is reported in, indexed by CpuCounter.
    inline constexpr std::array<std::string_view, core::platform::CpuCounterCount> CounterUnits {
        "instr", // Instructions
        "cyc",   // Cycles
        "refcyc" // ReferenceCycles
    };
    static_assert(core::platform::CpuCounterCount == 3,
                  "CounterUnits must have one entry per CpuCounter, in enumerator order");

    /// Formats @p value as a right-aligned column: two decimals at an SI prefix, then the prefixed @p unit,
    /// e.g. "   1.23 Ginstr". The prefix is picked after rounding, so that 999'995 reads "1.00 M" and never
    /// "1000.00 k"; an unprefixed unit is indented by the width of a prefix, so that the units line up.
    [[nodiscard]] inline std::string scaled(double value, std::string_view unit)
    {
        struct Prefix
        {
            double factor;
            std::string_view name;
        };
        constexpr auto Prefixes = std::array {
            Prefix { .factor = 1.0, .name = " " },  Prefix { .factor = 1e3, .name = "k" },
            Prefix { .factor = 1e6, .name = "M" },  Prefix { .factor = 1e9, .name = "G" },
            Prefix { .factor = 1e12, .name = "T" },
        };
        constexpr auto Digits = 100000.0; // the hundredths of the largest number a column holds: 999.99
        for (auto const& prefix: Prefixes | std::views::take(Prefixes.size() - 1))
            if (std::round(value / prefix.factor * 100.0) < Digits)
                return std::format(
                    " {:>7} {}{}", std::format("{:.2f}", value / prefix.factor), prefix.name, unit);
        auto const& largest = Prefixes.back();
        return std::format(" {:>7} {}{}", std::format("{:.2f}", value / largest.factor), largest.name, unit);
    }
} // namespace detail

/// Formats the CPU work of a run as table columns, one per measure the source supports, in
/// core::platform::AllCpuCounters order: " {:>7} {}" with the value scaled to an SI prefix and two decimals,
/// e.g. "    1.23 Ginstr". A supported measure absent from the sample prints "—" in place of the value.
/// @param delta The work done by the run.
/// @param source Tells which measures are supported.
/// @param divisor What to divide each value by, e.g. the number of frames; at least 1.
/// @return The columns; empty when the source supports no measure.
[[nodiscard]] inline std::string formatCounterColumns(core::platform::CpuCounterSample const& delta,
                                                      core::platform::ICpuCounterSource const& source,
                                                      std::uint64_t divisor)
{
    auto columns = std::string {};
    for (auto const counter: core::platform::AllCpuCounters)
    {
        if (!source.supports(counter))
            continue;
        auto const unit = detail::CounterUnits[std::to_underlying(counter)];
        if (auto const value = delta.get(counter))
            columns += detail::scaled(static_cast<double>(*value) / static_cast<double>(divisor), unit);
        else
            columns += std::format(" {:>7} {}", "—", unit);
    }
    return columns;
}

/// Says why a run reports no CPU work.
/// @param source The counter source.
/// @return "CPU counters unavailable: <reason>" when no measure is supported; empty otherwise.
[[nodiscard]] inline std::string formatCounterAvailability(core::platform::ICpuCounterSource const& source)
{
    auto reason = std::string_view {};
    for (auto const counter: core::platform::AllCpuCounters)
    {
        if (source.supports(counter))
            return {};
        if (reason.empty())
            reason = source.unavailableReason(counter);
    }
    return std::format("CPU counters unavailable: {}", reason);
}

} // namespace vtbackend::testing
