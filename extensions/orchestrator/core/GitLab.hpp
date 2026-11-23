// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <orchestrator/core/Model.hpp>

/// The mapping from what `glab ... --output json` prints to the model: the REST API's objects, unprojected.
/// Tested against the output of glab 1.119.0 (core/testdata/gitlab/README.md). The JSON reader stays in
/// GitLab.cpp: this header must not pull it in.
namespace orchestrator::gitlab
{

/// @param detailedMergeStatus A merge request's `detailed_merge_status`.
/// @return Its review state: Required while approvals are missing, ChangesRequested when a reviewer asked for
///         changes, Approved once it can be merged, and None for every other value.
[[nodiscard]] ReviewState reviewStateOf(std::string_view detailedMergeStatus) noexcept;

/// `detailed_merge_status` names a single blocker, and it names the pipeline only when the project requires
/// pipelines to succeed (or auto-merge is set) and no higher blocker, such as a draft or a conflict, outranks
/// it. None therefore means "not known", not "passing"; `ci_must_pass` also covers a cancelled, skipped,
/// manual or missing pipeline.
/// @param detailedMergeStatus A merge request's `detailed_merge_status`.
/// @return Its CI state: Pending for `ci_still_running`, Failing for `ci_must_pass`, and None for every other
///         value.
[[nodiscard]] CiState ciStateOf(std::string_view detailedMergeStatus) noexcept;

/// Maps the output of the declared `glab-mrs` command. GitLab's list carries no pipeline object and no
/// closing issues: a change request's CI state is only what its `detailed_merge_status` says (ciStateOf), it
/// names no failing check, and it claims no issue.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its merge requests, leaving out (and counting) each without an `iid` or whose target branch is not
///         a Ref argument; or why the output was refused.
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
