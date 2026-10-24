// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The latency budgets and the verdict on a daemon-mode run against an in-process one.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vthost::latency
{

/// How much worse than in-process the daemon path may be.
struct Budgets
{
    /// Launch to first prompt pixel, daemon already running: median over in-process.
    core::platform::SteadyDuration startup = std::chrono::milliseconds { 10 };
    /// Keystroke to pixel over the local socket: median over in-process.
    core::platform::SteadyDuration keystrokeMedian = std::chrono::milliseconds { 1 };
};

/// Fewer startup samples per side than this prove nothing.
inline constexpr std::size_t MinimumStartupSamples = 10;
/// Fewer keystroke samples per side than this prove nothing.
inline constexpr std::size_t MinimumKeystrokeSamples = 200;

/// The outcome of one budget.
enum class Verdict : uint8_t
{
    Pass = 0,
    Fail = 1,
    Inconclusive = 2,
};

/// What one mode (in-process or daemon) measured.
struct ModeSamples
{
    std::vector<core::platform::SteadyDuration> startup;
    std::vector<core::platform::SteadyDuration> keystroke;
};

/// The verdicts, and by how much the daemon path was slower.
struct GateResult
{
    Verdict startup = Verdict::Inconclusive;
    Verdict keystroke = Verdict::Inconclusive;
    core::platform::SteadyDuration startupDelta {};
    core::platform::SteadyDuration keystrokeDelta {};
};

/// Judges the daemon path against in-process.
/// @param budgets What the daemon path may cost on top of in-process.
/// @param inProcess The in-process run.
/// @param daemon The run against an already running daemon.
/// @return Per budget Pass iff the daemon's median minus the in-process median is within it, Fail if
///         above it, Inconclusive when either side has too few samples.
[[nodiscard]] GateResult judge(Budgets const& budgets,
                               ModeSamples const& inProcess,
                               ModeSamples const& daemon);

} // namespace vthost::latency
