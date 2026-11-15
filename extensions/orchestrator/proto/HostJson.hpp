// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

/// What the prototype driver reads with glaze outside the pane listing: the configured paths, and the control
/// API's requests and replies. Glaze reflects only types with linkage, so these cannot sit in an anonymous
/// namespace. Every member is defaulted, so an absent key keeps its default.
namespace orchestrator::proto::detail
{

/// One `repos[]` entry, as far as its path.
struct JsonRepoPath
{
    std::optional<std::string> path; ///< Its directory, unresolved.
};

/// The configuration, as far as its paths.
struct JsonConfigPaths
{
    std::optional<std::string> worktreeRoot;        ///< `worktree_root`, unresolved.
    std::optional<std::vector<JsonRepoPath>> repos; ///< The repositories.
};

/// `tab.create`'s request.
struct JsonCreateTab
{
    std::string placement;            ///< `existing`: into the current workspace.
    std::string cwd;                  ///< The working directory.
    std::vector<std::string> command; ///< The program and its arguments.
    std::string label;                ///< The tab's label.
    std::string focus;                ///< `keep`: the user's focus stays where it is.
};

/// `tab.create`'s reply.
struct JsonCreatedTab
{
    std::string pane; ///< `pane:<uuid>`.
};

/// `pane.close`'s request.
struct JsonClosePane
{
    std::string pane; ///< `pane:<uuid>`.
};

} // namespace orchestrator::proto::detail
