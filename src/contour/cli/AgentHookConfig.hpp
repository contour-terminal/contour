// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The edits `contour agent setup` makes to the agents' own hook files (Claude Code's settings.json, Codex's
/// hooks.json) so that their hooks call `contour agent report`: text in, text out, so that every rule is
/// testable without a file system.

#include <contour/cli/AgentIntegrations.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace contour::cli
{

/// What a merge or removal did to a configuration file's text.
enum class HookMergeResult : uint8_t
{
    Unchanged = 0, ///< The text already says what was asked; HookMerge::text is the input.
    Changed,       ///< HookMerge::text is the new text.
    Malformed, ///< The input cannot be read as its format; nothing may be written, HookMerge::text is empty.
    DirectoryMissing, ///< The directory a variable relocates the file to is absent and was not created.
};

/// A configuration file's text after an edit.
struct HookMerge
{
    HookMergeResult result = HookMergeResult::Unchanged; ///< What happened.
    std::string text;                                    ///< The text to write; see HookMergeResult.
    std::string detail; ///< Why, for Malformed and DirectoryMissing; else empty.
};

/// Makes every hook event of @p integration run `contour agent report`.
///
/// One matcher group per event is ensured; our entries are recognized by `agent report --agent <id>` in
/// their command, and everything else in the file is kept, in its order (comments and formatting are not).
/// @param integration The agent whose events are installed (AgentIntegration::hookEvents).
/// @param settingsJson The agent's hook file; empty for no file.
/// @param contourExe The absolute path of the `contour` binary the hooks run.
/// @return The new text.
[[nodiscard]] HookMerge mergeHooks(AgentIntegration const& integration,
                                   std::string_view settingsJson,
                                   std::string_view contourExe);

/// Removes @p integration's entries from its hook file, and the groups and event arrays that held only them.
/// @param integration The agent.
/// @param settingsJson The agent's hook file.
/// @return The new text.
[[nodiscard]] HookMerge removeHooks(AgentIntegration const& integration, std::string_view settingsJson);

} // namespace contour::cli
