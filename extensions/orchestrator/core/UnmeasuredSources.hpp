// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vector>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Provider.hpp>

/// Test support: the tables a configuration is checked against, with one row of each marked Unmeasured, so
/// that the refusal of a tool whose output was never recorded stays tested once every shipped row is
/// Recorded.
namespace orchestrator
{

/// @return The shipped providers() and issueSources(), except that GitLab's provider row and Jira's issue
///         source row are Unmeasured, as the rows of a tool whose output was never recorded.
[[nodiscard]] inline SourceTables unmeasuredSources()
{
    static auto const providerRows = [] {
        auto rows = std::vector<Provider>(providers().begin(), providers().end());
        for (auto& row: rows)
            if (row.forge == Forge::GitLab)
                row.verification = Verification::Unmeasured;
        return rows;
    }();
    static auto const issueSourceRows = [] {
        auto rows = std::vector<IssueSourceRow>(issueSources().begin(), issueSources().end());
        for (auto& row: rows)
            if (row.name == "jira")
                row.verification = Verification::Unmeasured;
        return rows;
    }();
    return SourceTables { .providers = providerRows, .issueSources = issueSourceRows };
}

} // namespace orchestrator
