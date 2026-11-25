// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
/// names no failing check, and it claims no issue. Each one's CI is Inferred until enriched() matches a
/// pipeline to it; its review always is.
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

/// @param status A REST pipeline `status`.
/// @return success, skipped: Passing; failed, canceled: Failing; every other value (running, pending,
///         created, waiting_for_resource, preparing, manual, scheduled, canceling, unknown): Pending.
[[nodiscard]] CiState pipelineStateOf(std::string_view status) noexcept;

/// Maps the output of the declared `glab-pipelines` command: a JSON array of `{id, sha, status, …}`.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its pipelines, in list order, leaving out each without an `id` or a `sha`; or why the output was
///         refused.
[[nodiscard]] std::expected<std::vector<PipelineInfo>, ParseError> pipelines(std::string_view stdoutText,
                                                                             OutputExtent extent);

/// Maps the output of the declared `glab-failed-jobs` command: a JSON array of `{name, pipeline: {id}, …}`.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @return Its jobs, in list order, names sanitised (displayText, MaxTitleChars), leaving out each without a
///         name or a pipeline id; or why the output was refused.
[[nodiscard]] std::expected<std::vector<FailedJob>, ParseError> failedJobs(std::string_view stdoutText,
                                                                           OutputExtent extent);

/// Joins pipelines and failed jobs into the REST list. A pipeline belongs to a merge request when it ran on
/// its `headSha`, or for its `refs/merge-requests/<iid>/head` or `refs/merge-requests/<iid>/merge` (a
/// merged-results pipeline, whose commit is not the head). A merge request with such pipelines takes the
/// highest-id one's ci and that pipeline's failed jobs (list order, each name once), and its ciSource
/// becomes Complete; its review stays as reviewStateOf() gave it, and Inferred. Without one it stays as
/// changeRequests() mapped it.
/// @param listed The REST list's merge requests.
/// @param pipelines What pipelines() read; std::nullopt: the call failed.
/// @param jobs What failedJobs() read; std::nullopt: the call failed (failingChecks stay empty).
/// @return @p listed, in its order, each matched merge request enriched.
[[nodiscard]] std::vector<ChangeRequest> enriched(std::vector<ChangeRequest> listed,
                                                  std::optional<std::span<PipelineInfo const>> pipelines,
                                                  std::optional<std::span<FailedJob const>> jobs);

} // namespace orchestrator::gitlab
