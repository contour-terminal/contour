// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// What `gh` prints, as GitHub.cpp reads it; nothing else includes this header. Glaze reflects only types
/// with linkage, so these cannot sit in GitHub.cpp's anonymous namespace, and a named one there is what
/// misc-use-internal-linkage refuses. Every member is defaulted, so an absent key keeps its default; the
/// `gh-prs` rows are positional, and their layout is in GitHub.cpp's glz::meta.
namespace orchestrator::github::detail
{

/// One `statusCheckRollup` entry, as the `gh-prs` projection prints it: `[__typename, name, context,
/// status, conclusion, state]`. A CheckRun has name, status and conclusion; a StatusContext has context and
/// state; the keys an entry lacks are projected as `null`, which reads as std::nullopt.
struct JsonCheck
{
    std::string typeName;
    std::optional<std::string> name;
    std::optional<std::string> context;
    std::optional<std::string> status;
    std::optional<std::string> conclusion;
    std::optional<std::string> state;
};

/// One `closingIssuesReferences` entry, projected as `[number, url]`.
struct JsonIssueReference
{
    uint64_t number = 0;
    std::optional<std::string> url; ///< The issue's page, which names the repository it lives in.
};

/// One `gh-prs` row: the command's `--jq` filter projects each pull request into an array, members in the
/// order they are declared here. The two `bool` members are the wire's own booleans, converted to Readiness
/// and HeadOrigin as soon as they are read.
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
    std::vector<JsonIssueReference> closingIssuesReferences;
    std::vector<JsonCheck> statusCheckRollup;
    std::string mergeable;
    std::string updatedAt;
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
