// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `bench-latency headless`: the same echo child behind a SessionHost in this process and behind a daemon
/// on a local socket, typed at in turns, and the difference between the two.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace vthost::latency
{

/// What the keystrokes of a headless run compete with.
enum class KeystrokeLoad : uint8_t
{
    Idle = 0,    ///< Nothing: the terminal has nothing else to do.
    FloodedPane, ///< A second pane of the same daemon floods MixedFloodWorkload meanwhile.
};

/// The Workloads row the flooded pane of a KeystrokeLoad::FloodedPane run writes.
inline constexpr std::string_view MixedFloodWorkload = "ascii";

/// How `bench-latency headless` runs.
struct HeadlessOptions
{
    std::size_t runs = 10; ///< Launches per mode, alternating.
    std::size_t keys = 50; ///< Measured keys per launch (ten more are typed first and dropped).
    /// The Workloads row to flood the terminal with instead of typing keys; empty to type keys.
    std::string workload;
    /// With KeystrokeLoad::FloodedPane, keys are typed into one pane while a second one floods, and the run
    /// reports the daemon's keystroke latency and the flood's MiB/s; it excludes @ref workload.
    KeystrokeLoad keystrokeLoad = KeystrokeLoad::Idle;
    std::chrono::seconds floodDuration { 3 }; ///< How long a flood lasts.
    /// The executable whose `echo-child` and `serve` verbs the legs run; the running one if absent.
    std::optional<std::filesystem::path> executable;
    std::optional<std::filesystem::path> record; ///< Where to write the TSV record.
};

/// Runs the benchmark and prints its table.
/// @param options How it runs.
/// @param out Where the table goes.
/// @param err Where a failure is described.
/// @return EXIT_SUCCESS, or EXIT_FAILURE when a run failed (the reason is on @p err). The verdicts of the
///         gate are informational here: the gate is `e2e`'s.
[[nodiscard]] int runHeadlessBench(HeadlessOptions const& options, std::ostream& out, std::ostream& err);

} // namespace vthost::latency
