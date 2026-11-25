// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Provider.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>

#include <orchestrator/core/GitHub.hpp>
#include <orchestrator/core/GitLab.hpp>

namespace orchestrator
{

namespace
{
    constexpr auto ProviderTable = std::array {
        Provider { .forge = Forge::GitHub,
                   .name = "GitHub",
                   .authCheck = "gh-auth",
                   .listChangeRequests = "gh-prs",
                   .listIssues = "gh-issues",
                   .defaultBranch = "gh-default-branch",
                   .fetch = "git-fetch",
                   .addWorktree = "git-worktree-add",
                   .addBranchWorktree = "git-worktree-add-branch",
                   .checkout = "gh-pr-checkout",
                   .changeRequests = &github::changeRequests,
                   .issues = &github::issues,
                   .defaultBranchOf = &github::defaultBranch,
                   .verification = Verification::Recorded },
        // Recorded from glab 1.119.0 (core/testdata/gitlab/). Its list carries no pipeline object and no
        // closing issues: the CI state is what `detailed_merge_status` says until the project's pipelines and
        // failed jobs are joined in by the head commit, and no merge request claims an issue.
        Provider { .forge = Forge::GitLab,
                   .name = "GitLab",
                   .authCheck = "glab-auth",
                   .listChangeRequests = "glab-mrs",
                   .listIssues = "glab-issues",
                   .defaultBranch = "glab-default-branch",
                   .fetch = "git-fetch",
                   .addWorktree = "git-worktree-add",
                   .addBranchWorktree = "git-worktree-add-branch",
                   .checkout = "glab-mr-checkout",
                   .changeRequests = &gitlab::changeRequests,
                   .issues = &gitlab::issues,
                   .defaultBranchOf = &gitlab::defaultBranch,
                   .verification = Verification::Recorded,
                   .listPipelines = "glab-pipelines",
                   .listFailedJobs = "glab-failed-jobs",
                   .mapPipelines = &gitlab::pipelines,
                   .mapFailedJobs = &gitlab::failedJobs,
                   .enrich = &gitlab::enriched },
    };
} // namespace

std::span<Provider const> providers() noexcept
{
    return ProviderTable;
}

Provider const* findProvider(Forge forge) noexcept
{
    return findProvider(ProviderTable, forge);
}

Provider const* findProvider(std::span<Provider const> rows, Forge forge) noexcept
{
    auto const row = std::ranges::find(rows, forge, &Provider::forge);
    return row != rows.end() ? &*row : nullptr;
}

std::string unrecordedText(Provider const& provider)
{
    return std::format("not yet supported: {} has no recorded fixture", provider.name);
}

} // namespace orchestrator
