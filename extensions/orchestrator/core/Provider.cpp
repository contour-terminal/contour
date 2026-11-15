// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Provider.hpp>

#include <algorithm>
#include <array>
#include <span>

#include <orchestrator/core/GitHub.hpp>

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
                   .defaultBranchOf = &github::defaultBranch },
    };
} // namespace

std::span<Provider const> providers() noexcept
{
    return ProviderTable;
}

Provider const* findProvider(Forge forge) noexcept
{
    auto const row = std::ranges::find(ProviderTable, forge, &Provider::forge);
    return row != ProviderTable.end() ? &*row : nullptr;
}

} // namespace orchestrator
