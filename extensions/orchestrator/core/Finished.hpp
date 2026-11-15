// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <orchestrator/core/Run.hpp>

namespace orchestrator
{

/// The most bytes Summary::detail may have.
inline constexpr std::size_t MaxSummaryDetailBytes = std::size_t { 16 } * 1024;

/// The most characters one run's line of Summary::detail keeps, once sanitised.
inline constexpr std::size_t MaxSummaryLineChars = 200;

/// Whether a finished run is old enough to be purged from the Finished list.
///
/// A retention of zero (or less) means "never purge": the Finished list then keeps every entry until the user
/// dismisses it.
/// @param run The run.
/// @param nowWallMs Now, in wall-clock milliseconds.
/// @param retention How long a finished run is kept; zero keeps it for ever.
/// @return Whether @p run is Finished with its worktree removed, and ended more than @p retention before
///         @p nowWallMs. A kept run, or a kept run being checked again, is never purgeable.
[[nodiscard]] bool isPurgeable(Run const& run,
                               uint64_t nowWallMs,
                               std::chrono::milliseconds retention) noexcept;

/// How many finished runs the user has not opened yet.
/// @param runs The runs.
/// @return The Finished runs marked Seen::New, and the kept ones among them being checked again: a re-check
///         keeps the mark it had, so it neither drops out of the count nor returns to it.
[[nodiscard]] std::size_t newFinishedCount(std::span<Run const> runs) noexcept;

/// The numbers the summary shows.
struct Counts
{
    uint32_t active = 0;      ///< The runs countsAsActive() accepts.
    uint32_t max = 0;         ///< The configured `max_agents`.
    uint32_t failingCi = 0;   ///< The user's change requests whose checks fail.
    uint32_t blocked = 0;     ///< The runs whose agent waits for the user.
    uint32_t newFinished = 0; ///< newFinishedCount().
};

/// Counts the runs: the active ones by countsAsActive(), never by isUnfinished().
/// @param runs The runs.
/// @param maxAgents The configured `max_agents`.
/// @param failingCi The user's change requests whose checks fail.
/// @return The counts summarize() shows.
[[nodiscard]] Counts countRuns(std::span<Run const> runs, uint32_t maxAgents, uint32_t failingCi) noexcept;

/// What the extension publishes about itself.
struct Summary
{
    std::string shortText; ///< One line for the status line.
    std::string detail;    ///< The short text and the run list, one line each, at most MaxSummaryDetailBytes.
};

/// The summary of the extension's work.
/// @param counts The numbers to show.
/// @param runs The runs to list.
/// @return The short text, such as "⬢ agents 3/4 · ✗1 · ●1 blocked · ✓2 new" with its zero terms omitted, and
///         the detail: the short text and one sanitised line per run, cut at a line boundary with a last line
///         counting the runs left out.
[[nodiscard]] Summary summarize(Counts counts, std::span<Run const> runs);

} // namespace orchestrator
