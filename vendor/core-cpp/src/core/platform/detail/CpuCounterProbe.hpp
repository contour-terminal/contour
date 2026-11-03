// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::platform::detail
{

/// Whether a counter source that was asked to count a known amount of work actually did.
enum class CounterProbe : std::uint8_t
{
    NotPopulated, ///< The counters stayed at zero: a virtualized machine that exposes none.
    Populated,    ///< The counters moved.
};

/// Judges a probe run: the work done between two reads of a counter that is supposed to count it.
/// Unit-free on purpose: macOS reports CPU time in Mach ticks on Apple Silicon, so only the
/// instruction count takes part in the decision.
/// @param instructionDelta Instructions counted across a run of busy work.
/// @return Populated iff the delta is greater than zero.
[[nodiscard]] constexpr CounterProbe probeCounters(std::uint64_t instructionDelta) noexcept
{
    return instructionDelta > 0 ? CounterProbe::Populated : CounterProbe::NotPopulated;
}

} // namespace core::platform::detail
