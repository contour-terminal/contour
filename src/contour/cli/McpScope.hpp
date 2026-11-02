// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp`: which panes an agent may address. The agent runs inside one pane, and by default may
// only reach the panes of that pane's tab; seeing or typing into any other tab needs `--allow-all-tabs`.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <vtcontrol/Api.hpp>

namespace contour::cli
{

/// Which panes a target may name.
enum class TabScope : uint8_t
{
    OwnTab = 0, ///< Only the panes of the caller's tab.
    AllTabs,    ///< Every pane of the workspace.
};

/// Why a target could not be resolved.
struct ScopeError
{
    /// The reasons.
    enum class Kind : uint8_t
    {
        NoCaller = 0, ///< No caller pane is known, so there is no default target.
        CallerGone,   ///< The caller pane is not in the workspace any more.
        OutsideTab,   ///< The target exists, but only in another tab.
        NotFound,     ///< No pane matches.
        Ambiguous,    ///< More than one pane matches.
    };

    Kind kind = Kind::NotFound; ///< The reason.
    std::string message;        ///< What to tell the agent.
};

/// Resolves @p target to a full pane id, by label, full id or hex id suffix of at least four digits.
///
/// Under `OwnTab` the target is matched against the caller's tab only: a label or suffix that would
/// match a pane of another tab does not resolve, so it can neither be ambiguous with nor stand in
/// for one of the caller's own panes.
/// @param workspace The `list` reply.
/// @param callerPane The full id of the pane the agent runs in; empty when unknown.
/// @param target What the agent wrote; empty names the caller's own pane.
/// @param scope Which panes may be named.
/// @return The pane's full id, or why it cannot be named.
[[nodiscard]] std::expected<std::string, ScopeError> resolvePane(vtcontrol::ListReply const& workspace,
                                                                 std::string_view callerPane,
                                                                 std::string_view target,
                                                                 TabScope scope);

/// @param workspace The `list` reply.
/// @param callerPane The full id of the pane the agent runs in.
/// @return The id of the tab holding @p callerPane, or nothing when no tab does.
[[nodiscard]] std::optional<std::string> ownTabOf(vtcontrol::ListReply const& workspace,
                                                  std::string_view callerPane);

/// @param workspace The `list` reply.
/// @param callerPane The full id of the pane the agent runs in.
/// @return The reply as JSON, plus a top-level `self` (`pane`, `tab`, `window`), `isOwnTab` on the
///         caller's tab, `isSelf` on the caller's pane, and the caller's tab first within its window.
[[nodiscard]] std::string annotateWorkspace(vtcontrol::ListReply const& workspace,
                                            std::string_view callerPane);

} // namespace contour::cli
