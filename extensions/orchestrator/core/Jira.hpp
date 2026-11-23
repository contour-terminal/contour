// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Model.hpp>

/// The mapping from what Atlassian's `acli jira workitem search --json` prints to the model, tested against
/// the output of acli 1.3.41 (core/testdata/jira/). The JSON reader stays in Jira.cpp: this header must not
/// pull it in.
namespace orchestrator::jira
{

/// The most bytes a site's address may have.
inline constexpr std::size_t MaxSiteBytes = 256;

/// Checks a configured Jira site: `https://`, a host of letters, digits, `.` and `-` with an optional
/// `:port`, and no path but a trailing `/`.
/// @param text Such as `https://example.atlassian.net/`.
/// @return The site without its trailing `/`; std::nullopt if @p text is not one.
[[nodiscard]] std::optional<std::string_view> siteOf(std::string_view text) noexcept;

/// @param site A site siteOf() returned.
/// @param key A Jira key (isJiraKey()).
/// @return The issue's page: `<site>/browse/<key>`.
[[nodiscard]] std::string browseUrl(std::string_view site, std::string_view key);

/// Maps the output of the declared `acli-issues` command: a JSON array of work items, each with a `key` and
/// `fields.summary`.
/// @param stdoutText What it printed.
/// @param extent Whether the host kept all of it; a cut output is read up to its last whole entry.
/// @param context The source's site, from which each issue's page is built: the URL is never read.
/// @return Its issues as JiraRef items, leaving out (and counting) each whose key is no Jira key; or why the
///         output was refused.
[[nodiscard]] std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText,
                                                              OutputExtent extent,
                                                              IssueSourceContext const& context);

} // namespace orchestrator::jira
