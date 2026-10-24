// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The text formats of latency measurement: the probe report a GUI run prints for the bench to read,
/// the TSV record of a summary, and the one-line gate verdict.

#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/latency/Gate.hpp>
#include <vthost/latency/KeystrokeProbe.hpp>
#include <vthost/latency/StartupTimeline.hpp>
#include <vthost/latency/Stats.hpp>

namespace vthost::latency
{

/// What a probe report holds, as parsed.
struct ProbeReport
{
    ProbePhase phase = ProbePhase::AwaitingPrompt;
    core::platform::SteadyTimePoint processStart {};
    std::optional<core::platform::SteadyTimePoint> firstPrompt;
    std::vector<core::platform::SteadyDuration> keystrokes;
    int awaitedColumn = -1;
};

/// Why a probe report did not parse.
enum class ReportError : uint8_t
{
    MissingField = 0, ///< A required line is absent.
    Malformed = 1,    ///< A line is not of a known shape, or a number does not parse.
    UnknownPhase = 2, ///< The phase line names no phase.
};

/// Renders a finished probe run: one record per line, `phase`, `process-start-ns`, `first-prompt-ns`
/// (when there was one), `awaited-column`, `keystroke-ns` (repeated) and `mark <name> <ns since
/// process start>` (repeated).
/// @param result What the probe measured.
/// @param timeline The startup timeline of the same run.
/// @return The report text.
[[nodiscard]] std::string formatProbeReport(ProbeResult const& result, StartupTimeline const& timeline);

/// Reads back what formatProbeReport() wrote. The `mark` lines are validated and otherwise ignored.
/// @param text A report.
/// @return The report's content, or why it is not one.
[[nodiscard]] std::expected<ProbeReport, ReportError> parseProbeReport(std::string_view text);

/// @param metric What was measured, such as `startup` or `keystroke`.
/// @param mode Which mode measured it, such as `in-process` or `daemon`.
/// @param summary The quantiles.
/// @return A TSV row `metric mode count p50_us p90_us p99_us max_us` ending in a newline, the times in
///         microseconds with one decimal.
[[nodiscard]] std::string formatRecord(std::string_view metric,
                                       std::string_view mode,
                                       Summary const& summary);

/// @param metric What was measured, such as `flood-mibps`.
/// @param mode Which mode measured it.
/// @param count How many runs the value summarizes.
/// @param value The figure, one decimal in the record.
/// @return A TSV row `metric mode count value` ending in a newline.
[[nodiscard]] std::string formatScalarRecord(std::string_view metric,
                                             std::string_view mode,
                                             std::size_t count,
                                             double value);

/// @return The header line of the table printed to a terminal, ending in a newline: `metric`, `mode`,
///         `count`, then the quantiles in microseconds. The columns line up with formatTableRow().
[[nodiscard]] std::string formatTableHeader();

/// @param metric What was measured, such as `startup` or `keystroke`.
/// @param mode Which mode measured it, such as `in-process` or `daemon`.
/// @param summary The quantiles.
/// @return One table row under formatTableHeader(), ending in a newline.
[[nodiscard]] std::string formatTableRow(std::string_view metric,
                                         std::string_view mode,
                                         Summary const& summary);

/// @param budgets The budgets judged against.
/// @param result The verdicts.
/// @return The one-line verdict, such as `gate: startup +3.2 ms (budget +10.0 ms) PASS; keystroke p50
///         +0.41 ms (budget +1.00 ms) PASS`.
[[nodiscard]] std::string formatGateLine(Budgets const& budgets, GateResult const& result);

} // namespace vthost::latency
