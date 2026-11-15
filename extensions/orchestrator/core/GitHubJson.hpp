// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// What `gh` prints, as GitHub.cpp reads it; nothing else includes this header. Glaze reflects only types
/// with linkage, so these cannot sit in GitHub.cpp's anonymous namespace, and a named one there is what
/// misc-use-internal-linkage refuses. Every member is defaulted, so an absent key keeps its default.
namespace orchestrator::github::detail
{

/// One `statusCheckRollup` entry: a CheckRun has name, status and conclusion; a StatusContext has
/// context and state.
struct JsonCheck
{
    std::string typeName; ///< `__typename`, which is no member name: GitHub.cpp maps the key.
    std::string name;
    std::string status;
    std::string conclusion;
    std::string context;
    std::string state;
};

/// One `closingIssuesReferences` entry.
struct JsonIssueReference
{
    uint64_t number = 0;
    std::string url; ///< The issue's page, which names the repository it lives in.
};

/// One `gh pr list` entry. The two `bool` members are the wire's own booleans, converted to Readiness and
/// HeadOrigin as soon as they are read.
struct JsonPullRequest
{
    uint64_t number = 0;
    std::string title;
    std::string url;
    std::string headRefName;
    std::string baseRefName;
    bool isDraft = false;
    bool isCrossRepository = false;
    std::string reviewDecision;
    std::vector<JsonCheck> statusCheckRollup;
    std::vector<JsonIssueReference> closingIssuesReferences;
};

/// One `gh issue list` entry.
struct JsonIssue
{
    uint64_t number = 0;
    std::string title;
    std::string url;
};

/// `defaultBranchRef`.
struct JsonBranchRef
{
    std::string name;
};

/// `gh repo view --json defaultBranchRef`.
struct JsonRepository
{
    JsonBranchRef defaultBranchRef;
};

} // namespace orchestrator::github::detail
