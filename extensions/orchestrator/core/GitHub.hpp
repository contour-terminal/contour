// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/core/Model.hpp>

/// The mapping from what `gh` prints to the model. The JSON reader stays in GitHub.cpp: this header must not
/// pull it in (scripts/include-budget.txt).
namespace orchestrator::github
{

/// One entry of a pull request's `statusCheckRollup`: a check run or a commit status.
struct Check
{
    std::string_view typeName;   ///< `__typename`: `CheckRun` or `StatusContext`.
    std::string_view status;     ///< A check run's status, such as `COMPLETED` or `IN_PROGRESS`.
    std::string_view conclusion; ///< A completed check run's conclusion, such as `SUCCESS` or `FAILURE`.
    std::string_view state;      ///< A commit status's state, such as `PENDING` or `ERROR`.
    std::string_view name = {};  ///< A check run's name, or a commit status's context.
};

/// What one check says.
/// @param check A check run or a commit status.
/// @return Passing or Failing for a result this mapping knows; Pending for one still running, and for any
///         value it does not know, which must never look green.
[[nodiscard]] CiState checkStateOf(Check const& check) noexcept;

/// What a pull request's checks say, taken together.
/// @param checks Its `statusCheckRollup`.
/// @return None without checks; otherwise Failing if one fails, else Pending if one is pending, else Passing.
[[nodiscard]] CiState ciStateOf(std::span<Check const> checks) noexcept;

/// @param reviewDecision A pull request's `reviewDecision`.
/// @return Its review state; Required for a value this mapping does not know.
[[nodiscard]] ReviewState reviewStateOf(std::string_view reviewDecision) noexcept;

/// Maps the output of the declared `gh-prs` command.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its pull requests, leaving out (and counting) each whose base branch is not a Ref argument; or why
///         the output was refused.
[[nodiscard]] std::expected<Listed<ChangeRequest>, ParseError> changeRequests(std::string_view stdoutText,
                                                                              OutputExtent extent);

/// Maps the output of the declared `gh-issues` command.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its issues, leaving out (and counting) each without a number; or why the output was refused.
[[nodiscard]] std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText,
                                                              OutputExtent extent);

/// Maps the output of the declared `gh-default-branch` command.
/// @param stdoutText What it printed.
/// @return The default branch's name, which is a Ref argument; or why the output was refused.
[[nodiscard]] std::expected<std::string, ParseError> defaultBranch(std::string_view stdoutText);

} // namespace orchestrator::github
