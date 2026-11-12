// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/GitHead.hpp>

#include <filesystem>
#include <string>

namespace vtbackend
{

/// The name a new workspace starts with, chosen once at creation.
///
/// Inside a git working tree it is the name of the folder holding the `.git` -- the repository's root, or,
/// for a linked worktree (whose `.git` is a file), the worktree's own top folder, never the main
/// repository's. Elsewhere it is the last component of @p cwd.
/// @param fileSystem Where the repository is looked up.
/// @param cwd The folder the workspace's first pane starts in.
/// @return The name; empty for an empty or relative-less path, for the root, and never `/` or `.`.
[[nodiscard]] std::string defaultWorkspaceName(GitFileSystem const& fileSystem,
                                               std::filesystem::path const& cwd);

} // namespace vtbackend
