// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <orchestrator/core/Model.hpp>

/// The mapping from what `glab` prints to the model. Written from the GitLab REST API's documented shapes and
/// not yet checked against glab's own output: the provider row stays Unmeasured until it is. The JSON reader
/// stays in GitLab.cpp: this header must not pull it in.
namespace orchestrator::gitlab
{

/// @param detailedMergeStatus A merge request's `detailed_merge_status`.
/// @return Its review state: Required while approvals are missing, ChangesRequested when a reviewer asked for
///         changes, Approved once it can be merged, and None for every other value.
[[nodiscard]] ReviewState reviewStateOf(std::string_view detailedMergeStatus) noexcept;

/// Maps the output of the declared `glab-mrs` command. GitLab's list carries no pipeline status and no
/// closing issues: every change request maps to CiState::None and claims no issue.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its merge requests, leaving out (and counting) each without an `iid` or whose target branch is not
/// a
///         Ref argument; or why the output was refused.
[[nodiscard]] std::expected<Listed<ChangeRequest>, ParseError> changeRequests(std::string_view stdoutText,
                                                                              OutputExtent extent);

/// Maps the output of the declared `glab-issues` command.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its issues, leaving out (and counting) each without an `iid`; or why the output was refused.
[[nodiscard]] std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText,
                                                              OutputExtent extent);

/// Maps the output of the declared `glab-default-branch` command.
/// @param stdoutText What it printed.
/// @return The project's `default_branch`, which is a Ref argument; or why the output was refused.
[[nodiscard]] std::expected<std::string, ParseError> defaultBranch(std::string_view stdoutText);

} // namespace orchestrator::gitlab
