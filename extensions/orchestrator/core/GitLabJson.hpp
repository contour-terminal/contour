// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

/// What `glab … -F json` prints, as GitLab.cpp reads it; nothing else includes this header. Glaze reflects
/// only types with linkage, so these cannot sit in GitLab.cpp's anonymous namespace. Only the keys the
/// mapping reads are members: every other key of GitLab's objects is skipped. The snake_case keys are in
/// GitLab.cpp's glz::meta.
namespace orchestrator::gitlab::detail
{

/// One merge request of the REST API's list (`GET /projects/:id/merge_requests`). The `bool` member is the
/// wire's own, converted to Readiness as soon as it is read.
struct JsonMergeRequest
{
    uint64_t iid = 0;
    std::string title;
    std::string webUrl;
    std::string sourceBranch;
    std::string targetBranch;
    bool draft = false;
    uint64_t sourceProjectId = 0;
    uint64_t targetProjectId = 0;
    std::optional<std::string> detailedMergeStatus; ///< Absent from instances older than GitLab 15.6.
};

/// One issue of the REST API's list (`GET /projects/:id/issues`).
struct JsonIssue
{
    uint64_t iid = 0;
    std::string title;
    std::string webUrl;
};

/// A project (`GET /projects/:id`).
struct JsonProject
{
    std::string defaultBranch;
};

} // namespace orchestrator::gitlab::detail
