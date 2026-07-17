// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <vtconformance/Diagnostics.h>
#include <vtconformance/EscTestLog.h>
#include <vtconformance/Suite.h>
#include <vtconformance/VtTestLog.h>

namespace vtconformance
{

/// How a scenario came out.
enum class Outcome : std::uint8_t
{
    Passed,

    /// An oracle said no: a failing self-check, an unanswered query, a golden mismatch, or a
    /// diagnostic that is not on the known-gap ratchet.
    Failed,

    /// The scenario cannot be judged headlessly (it needs real key or mouse events). A skipped
    /// scenario is never reported as passed — a green wall that hides untested ground is worse than
    /// an honest gap.
    Skipped,
};

/// What every oracle had to say about one scenario.
struct ScenarioResult
{
    std::string id;
    std::string title;
    ScenarioKind kind = ScenarioKind::Visual;

    /// Whether a failure of this scenario fails the build. @see Scenario::gatesBuild.
    ///
    /// Carried on the result, not looked up from the table, so that judging a report stays a pure
    /// function of the report -- which is what lets `judge()` be unit-tested without a suite.
    bool gatesBuild = false;

    /// Oracle C: vttest's own self-check verdicts.
    std::vector<Verdict> verdicts;

    /// Oracle B: queries the terminal never answered.
    std::vector<Query> unansweredQueries;

    /// Oracle A: sequences the engine could not honour.
    std::vector<Diagnostic> diagnostics;

    /// Oracle E: golden screen dumps that did not match, as human-readable diffs.
    std::vector<std::string> goldenDiffs;

    /// Oracle F: a self-asserting suite's own per-test verdicts. Empty for a suite that has none.
    EscTestSummary escTest;

    /// Set when the harness could not drive the scenario at all (child died, prompt never came).
    std::string harnessError;
};

/// A ratchet: the things Contour is known not to do yet, and is tolerated for.
///
/// A conformance gate that is red on the day it lands teaches everyone to ignore it. So an entry
/// listed here does not fail the build — but one that is *not* listed does, and so does a listed
/// entry that no longer occurs. The list can therefore only shrink, and it cannot silently go stale
/// while hiding a regression behind itself.
///
/// Used twice, with the same discipline: for the sequences the engine ignores (keyed by `gapKey()`),
/// and for the esctest cases that fail.
struct Ratchet
{
    /// One entry per tolerated item: a rendered VT sequence, or a test name.
    std::set<std::string> entries;

    /// @return Parsed from a `#`-commented, one-entry-per-line text file.
    [[nodiscard]] static Ratchet parse(std::string_view text);

    /// @return The file's text, for writing an updated ratchet back out.
    ///
    /// @param header The comment banner to lead with. Passed in rather than baked in, because the
    ///               two ratchets hold different things and each file has to say what it is.
    [[nodiscard]] std::string render(std::string_view header = {}) const;
};

/// A whole suite run.
struct Report
{
    std::string suite;
    std::vector<ScenarioResult> scenarios;

    /// Gaps that were tolerated. Entries here that never occurred are themselves a failure: they
    /// mean the ratchet has gone stale and is now hiding regressions.
    Ratchet knownGaps;

    /// esctest cases that are tolerated as failing, under the same rule.
    Ratchet knownFailures;
};

/// The ratchet's key for a diagnostic, e.g. `unknown: ESC Z`.
///
/// One function so that the ratchet file, the report and the updater can never disagree about what
/// names a gap.
[[nodiscard]] std::string gapKey(Diagnostic const& diagnostic);

/// Judges one scenario. Pure: the same inputs always give the same verdict.
[[nodiscard]] Outcome judge(ScenarioResult const& result,
                            Ratchet const& knownGaps,
                            Ratchet const& knownFailures = {});

/// @return Every distinct diagnostic across the run, deduplicated and summed.
[[nodiscard]] std::vector<Diagnostic> allDiagnostics(Report const& report);

/// @return Ratchet entries that no scenario actually hit, i.e. gaps that appear to be fixed.
[[nodiscard]] std::vector<std::string> staleGaps(Report const& report);

/// @return Failure-ratchet entries for esctest cases that now pass.
[[nodiscard]] std::vector<std::string> staleFailures(Report const& report);

/// @return esctest cases that failed and are not on the failure ratchet.
[[nodiscard]] std::vector<std::string> newFailures(Report const& report);

/// The banner written atop the ignored-sequence ratchet file.
inline constexpr auto KnownGapsHeader =
    std::string_view { "# VT sequences Contour does not yet honour, as reported by the conformance harness.\n"
                       "#\n"
                       "# This file is a RATCHET, not a suppression list:\n"
                       "#   * a diagnostic that is NOT listed here fails the build (a new gap);\n"
                       "#   * an entry here that no longer occurs ALSO fails the build (a stale gap).\n"
                       "#\n"
                       "# So the list can only shrink, and it cannot quietly stop describing reality.\n"
                       "# Regenerate with: vtconformance run --suite=vttest --update-known-gaps\n"
                       "\n" };

/// The banner written atop the esctest failure ratchet file.
inline constexpr auto KnownFailuresHeader =
    std::string_view { "# esctest cases Contour does not pass yet.\n"
                       "#\n"
                       "# A RATCHET, under the same rule as the ignored-sequence list:\n"
                       "#   * a test that fails and is NOT listed here fails the build (a regression);\n"
                       "#   * an entry here that now passes ALSO fails the build (a stale entry).\n"
                       "#\n"
                       "# Regenerate with: vtconformance run --suite=esctest --update-known-failures\n"
                       "\n" };

/// Renders a short console summary, one line per scenario plus a gap listing.
[[nodiscard]] std::string renderSummary(Report const& report);

/// Renders the report as Markdown, for embedding in a status document so its tables cannot go stale.
[[nodiscard]] std::string renderMarkdown(Report const& report);

/// @return EXIT_SUCCESS when every scenario passed or was skipped and the ratchet is exact.
[[nodiscard]] int exitCode(Report const& report);

} // namespace vtconformance
