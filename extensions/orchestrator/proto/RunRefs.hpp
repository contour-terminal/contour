// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Run.hpp>

namespace orchestrator::proto
{

/// What the user asks the prompt to do with one run.
enum class RunAction : uint8_t
{
    End,     ///< `end`: close the agent's tab, then try to remove the worktree.
    Retry,   ///< `retry`: check a kept worktree again.
    Dismiss, ///< `dismiss`: drop a finished run whose worktree is gone.
};

/// Why a word names no run the action may take.
enum class RunRefProblem : uint8_t
{
    Unknown,       ///< No run's id, row or item matches.
    NoSuchRow,     ///< A number beyond the rows the last `runs` printed.
    RowGone,       ///< The row's run is no longer listed; RunRefError::candidates holds its id.
    Ambiguous,     ///< Several runs the action may take match; RunRefError::candidates holds their ids.
    NotApplicable, ///< Only runs the action may not take match; RunRefError::candidates holds their ids.
};

/// Why resolveRun() found no run.
struct RunRefError
{
    RunRefProblem problem = RunRefProblem::Unknown; ///< What went wrong.
    std::vector<std::string> candidates;            ///< The runs concerned, by id; see RunRefProblem.

    [[nodiscard]] bool operator==(RunRefError const&) const = default;
};

/// Whether @p action may take @p run: whether the engine acts on it rather than ignoring it.
/// @param action The action.
/// @param run The run.
/// @return Whether the run is in a state the action changes.
[[nodiscard]] bool actionApplies(RunAction action, Run const& run) noexcept;

/// The run a word names, for an action.
///
/// The word is, in this order: a run's exact id; a row number of the last `runs`; or, case-insensitively, a
/// prefix of run ids and an item's spelling (`#1847`, `pr-1847`, `PR#1847`, `issue#12`, a Jira key), whose
/// matches the action may take must be exactly one.
/// @param word What the user typed.
/// @param action What is to be done with the run.
/// @param rows The run ids the last `runs` printed, row 1 first.
/// @param runs The engine's runs.
/// @return The run's id; or why there is none.
[[nodiscard]] std::expected<std::string, RunRefError> resolveRun(std::string_view word,
                                                                 RunAction action,
                                                                 std::span<std::string const> rows,
                                                                 std::span<Run const> runs);

/// Why a word was refused, as the user reads it.
/// @param word What the user typed.
/// @param action What was to be done.
/// @param error What resolveRun() reported.
/// @param runs The engine's runs, whose states a NotApplicable refusal names.
/// @return One line.
[[nodiscard]] std::string refusalText(std::string_view word,
                                      RunAction action,
                                      RunRefError const& error,
                                      std::span<Run const> runs);

/// What an accepted action will do, said before it is done.
/// @param action The action.
/// @param runId The run's id.
/// @return One line, such as "ending pr-1847-…: closing its tab, then checking whether its worktree can be
///         removed".
[[nodiscard]] std::string confirmationText(RunAction action, std::string_view runId);

} // namespace orchestrator::proto
