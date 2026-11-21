// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

/// Maps a change-request list command's output, whole or cut at the host's limit.
using MapChangeRequests = std::expected<Listed<ChangeRequest>, ParseError> (*)(std::string_view,
                                                                               OutputExtent);
/// Maps an issue list command's output, whole or cut at the host's limit.
using MapIssues = std::expected<Listed<Issue>, ParseError> (*)(std::string_view, OutputExtent);
/// Maps a default-branch command's output.
using MapDefaultBranch = std::expected<std::string, ParseError> (*)(std::string_view);

/// One forge: the declared commands that talk to it, and the mappings that read what they print. Adding a
/// forge is adding a row.
struct Provider
{
    Forge forge;                         ///< The forge this row stands for.
    std::string_view name;               ///< Its name, as the user reads it.
    std::string_view authCheck;          ///< The command that succeeds only when its CLI is logged in.
    std::string_view listChangeRequests; ///< The command that lists the user's open change requests.
    std::string_view listIssues;         ///< The command that lists open issues.
    std::string_view defaultBranch;      ///< The command that prints the default branch.
    // A dispatch prepares its worktree with these, in this order: fetch, then addWorktree and checkout for a
    // change request, or addBranchWorktree for an issue.
    std::string_view fetch;             ///< The command that fetches the remote the worktree starts from.
    std::string_view addWorktree;       ///< The command that adds a detached worktree at the base.
    std::string_view addBranchWorktree; ///< The command that adds a worktree on a new branch from the base.
    std::string_view checkout;          ///< The command that checks a change request out in its worktree.
    MapChangeRequests changeRequests;   ///< Reads listChangeRequests' output.
    MapIssues issues;                   ///< Reads listIssues' output.
    MapDefaultBranch defaultBranchOf;   ///< Reads defaultBranch's output.
    /// Whether its mappings were tested against recorded output: the configuration accepts only a Recorded
    /// forge, and each command the row names carries the same verification.
    Verification verification = Verification::Unmeasured;
};

/// Every provider, one row per forge.
[[nodiscard]] std::span<Provider const> providers() noexcept;

/// @param forge A forge.
/// @return Its row, or nullptr if no provider stands for it yet.
[[nodiscard]] Provider const* findProvider(Forge forge) noexcept;

/// @param rows Provider rows, such as a Config's sources().
/// @param forge A forge.
/// @return Its row in @p rows, or nullptr if none stands for it.
[[nodiscard]] Provider const* findProvider(std::span<Provider const> rows, Forge forge) noexcept;

/// What a step that would run an Unmeasured provider's command fails with, before asking the host.
/// @param provider A provider row.
/// @return Such as "not yet supported: GitLab has no recorded fixture".
[[nodiscard]] std::string unrecordedText(Provider const& provider);

} // namespace orchestrator
