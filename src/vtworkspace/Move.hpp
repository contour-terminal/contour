// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include <vtworkspace/Primitives.hpp>

namespace vtworkspace
{

/// A move source naming one pane: the leaf carrying @c session.
struct PaneSource
{
    SessionId session; //!< The session of the leaf being moved.
};

/// A move source naming a whole tab.
///
/// A tab with exactly one leaf is a pane payload: the model treats it like a PaneSource wherever that
/// makes a difference (a swap), and as a whole tab where that is the cheaper, identity-preserving move.
struct TabSource
{
    WorkspaceId workspace; //!< The workspace holding the tab.
    TabId tab;             //!< The tab being moved.
};

/// What is being dragged.
using MoveSource = std::variant<PaneSource, TabSource>;

/// Drop beside a pane: the source is grafted next to the leaf at an even split.
struct EdgeTarget
{
    SessionId pane; //!< The session of the leaf the source lands beside.
    SplitSide side; //!< The side of that leaf it lands on.
};

/// Drop onto the middle of a pane: the two panes trade their sessions.
struct SwapTarget
{
    SessionId pane; //!< The session of the leaf to trade with.
};

/// Drop onto a workspace's tab strip: the source becomes a tab at @c index.
struct TabWellTarget
{
    WorkspaceId workspace; //!< The workspace whose tab strip receives it.
    int index;             //!< The tab position; clamped into the workspace's tab range.
};

/// Drop onto nothing: the source becomes the only tab of a new workspace.
struct NewWorkspaceTarget
{
};

/// Drop onto a tab as a whole: the source joins it as a split, beside its active pane on JoinTabSide.
struct TabTarget
{
    WorkspaceId workspace; //!< The workspace holding the tab.
    TabId tab;             //!< The tab the source joins.
};

/// The side of a tab's active pane that a node joining the tab (TabTarget) lands on: where a drop on a tab of
/// the tab strip has always put it.
inline constexpr SplitSide JoinTabSide = SplitSide::Right;

/// Where the source is dropped.
using MoveTarget = std::variant<EdgeTarget, SwapTarget, TabWellTarget, NewWorkspaceTarget, TabTarget>;

/// Wire/host-facing names of the MoveSource alternatives, shared by vthost and contour.
enum class MoveSourceKind : uint8_t
{
    Pane = 0,
    Tab,
};

/// Wire/host-facing names of the MoveTarget alternatives, shared by vthost and contour.
enum class MoveTargetKind : uint8_t
{
    Edge = 0,
    Swap,
    TabWell,
    NewWorkspace,
    Tab, //!< TabTarget: the wire names the tab by any session of it.
};

/// Why SessionModel::moveNode refused. On any of these the model is untouched and no event fired.
enum class MoveError : uint8_t
{
    UnknownSource = 0, //!< The dragged session, workspace or tab does not exist.
    UnknownTarget,     //!< The drop pane or workspace does not exist.
    SelfTarget,        //!< The source is dropped onto itself (or onto a pane inside the dragged tab).
    TabIntoSwap,       //!< A multi-pane tab cannot trade places with a single pane.
    NoOp,              //!< The drop would leave everything where it is.
};

/// @return A short, stable description of @p error, for logs and replies.
[[nodiscard]] constexpr std::string_view describe(MoveError error) noexcept
{
    switch (error)
    {
        case MoveError::UnknownSource: return "unknown move source";
        case MoveError::UnknownTarget: return "unknown move target";
        case MoveError::SelfTarget: return "cannot move onto itself";
        case MoveError::TabIntoSwap: return "a multi-pane tab cannot be swapped with a pane";
        case MoveError::NoOp: return "move would change nothing";
    }
    return "unknown move error";
}

/// Where focus lands after a successful move.
struct MoveResult
{
    WorkspaceId workspace; //!< The workspace now showing the moved node.
    TabId tab;             //!< The tab now holding it (the active tab of @c workspace).
    SessionId focused;     //!< The moved session, which is the tab's active pane.
};

} // namespace vtworkspace
