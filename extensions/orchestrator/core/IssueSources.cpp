// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/IssueSources.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <orchestrator/core/Jira.hpp>

namespace orchestrator
{

namespace
{
    constexpr auto IssueSourceTable = std::array {
        // The repository's own forge: its provider row lists and maps the issues.
        IssueSourceRow { .name = "forge",
                         .title = "the forge",
                         .command = {},
                         .argument = "query",
                         .configKey = "query",
                         .site = SiteUse::None,
                         .authCheck = {},
                         .verification = Verification::Recorded,
                         .map = nullptr },
        // Written from Atlassian's documentation of `acli`; refused by the configuration until its output is
        // recorded.
        IssueSourceRow { .name = "jira",
                         .title = "Jira",
                         .command = "acli-issues",
                         .argument = "jql",
                         .configKey = "jql",
                         .site = SiteUse::Required,
                         .authCheck = "acli-auth",
                         .verification = Verification::Unmeasured,
                         .map = &jira::issues },
    };
} // namespace

std::span<IssueSourceRow const> issueSources() noexcept
{
    return IssueSourceTable;
}

IssueSourceRow const* findIssueSource(std::span<IssueSourceRow const> rows, std::string_view name) noexcept
{
    auto const row = std::ranges::find(rows, name, &IssueSourceRow::name);
    return row != rows.end() ? &*row : nullptr;
}

} // namespace orchestrator
