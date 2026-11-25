// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

/// What a source's mapping needs besides the command's output.
struct IssueSourceContext
{
    std::string_view
        site; ///< The Jira source's `site`, such as `https://example.atlassian.net`; empty for the forge.
};

/// Maps an issue list command's output, whole or cut at the host's limit.
using MapIssueSource = std::expected<Listed<Issue>, ParseError> (*)(std::string_view,
                                                                    OutputExtent,
                                                                    IssueSourceContext const&);

/// Whether a source's entry in the configuration names a site.
enum class SiteUse : uint8_t
{
    None,     ///< It has no `site`: its items live on the repository's forge.
    Required, ///< It must name an `https://` site, from which its items' pages are built.
};

/// One place a repository's issues may come from, as `repos[].issues[].source` names it. Adding a source is
/// adding a row.
struct IssueSourceRow
{
    std::string_view name;     ///< Its `source` spelling, such as `jira`.
    std::string_view title;    ///< Its name as the user reads it, such as `Jira`.
    std::string_view command;  ///< The command that lists its issues; empty: the forge provider's listIssues.
    std::string_view argument; ///< The command's placeholder the query fills.
    std::string_view configKey;   ///< The configuration key that holds the query, such as `jql`.
    std::string_view progressKey; ///< The configuration key that holds its in-progress query:
                                  ///< `in_progress_jql` for Jira; empty for the forge.
    SiteUse site; ///< Whether the configuration names its site.
    std::string_view
        authCheck; ///< The command that succeeds only when its CLI is logged in; empty: the forge's.
    Verification verification; ///< Only a Recorded source is accepted by the configuration.
    MapIssueSource map;        ///< Reads command's output; nullptr: the forge provider's issues mapping.
};

/// The issue sources, in the order the configuration's message lists them.
[[nodiscard]] std::span<IssueSourceRow const> issueSources() noexcept;

/// @param name A `source` spelling.
/// @return Its row in @p rows, or nullptr if none spells it.
[[nodiscard]] IssueSourceRow const* findIssueSource(std::span<IssueSourceRow const> rows,
                                                    std::string_view name) noexcept;

} // namespace orchestrator
