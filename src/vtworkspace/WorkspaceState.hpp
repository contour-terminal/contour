// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace vtworkspace
{

/// The three id counters of a SessionModel: the next id each kind of node is minted with.
///
/// Captured with SessionModel::idState() and handed back to SessionModel::restore(), so that a model
/// rebuilt after a daemon live upgrade mints exactly the ids the previous one would have, and a client
/// holding an id from before never sees it name another object afterwards.
struct ModelIdState
{
    WorkspaceId nextWorkspace { 1 }; ///< The id the next workspace gets.
    TabId nextTab { 1 };             ///< The id the next tab gets.
    PaneId nextPane { 1 };           ///< The id the next pane (leaf or split node) gets.
};

/// One node of a tab's pane tree, with the ids it has.
struct PaneTreeState
{
    PaneId id {};                        ///< The node's id (a leaf's or a split's).
    SplitState split = SplitState::None; ///< None for a leaf; the axis of the split otherwise.
    double ratio = 0.5;                  ///< A split's first child's space share.
    SessionId session {};                ///< The session a leaf carries; unused for a split.
    std::vector<PaneTreeState> children; ///< A split's two children, first then second; none for a leaf.
};

/// Whether a tab's active pane alone fills the tab area.
enum class ZoomState : uint8_t
{
    Unzoomed = 0, ///< Every pane is shown.
    Zoomed,       ///< The active pane alone is shown.
};

/// What a tab remembers besides its tree.
struct TabViewState
{
    PaneId activePane {};                 ///< The leaf with focus.
    ZoomState zoom = ZoomState::Unzoomed; ///< Whether it is zoomed.
    std::vector<PaneId> recentPanes;      ///< The leaves, most recently used first.
    std::optional<std::string> title;     ///< The runtime title, where one was set.
    /// The tab's colour per TabColorSource, indexed by its underlying value.
    std::array<std::optional<vtbackend::RGBColor>, 3> colors {};
};

/// A tab with its id.
struct TabState
{
    TabId id {};
    PaneTreeState root;
    TabViewState view;
};

/// A workspace with its id and tabs.
struct WorkspaceState
{
    WorkspaceId id {};
    std::vector<TabState> tabs;
    int activeTabIndex = -1;         ///< -1 where the workspace holds no tab.
    int previousActiveTabIndex = -1; ///< -1 where there is none.
};

/// The whole workspace tree: every workspace, tab and pane, with the ids and session ids they have.
struct WorkspaceTreeState
{
    std::vector<WorkspaceState> workspaces;
};

/// Why SessionModel::restore() refused a state.
enum class ModelRestoreError : uint8_t
{
    DuplicateId =
        0, ///< Two workspaces, two tabs, two panes (model-wide) or two leaves' sessions share an id.
    IdBeyondCounter, ///< An id is at or above the counter it is minted from, so a later mint would repeat it.
    /// A tab's tree is malformed: a split without exactly two children or with a ratio that is not finite,
    /// a leaf with any child.
    MalformedTree,
    UnknownActivePane, ///< A tab's active pane is not one of its leaves.
    BadTabIndex,       ///< A workspace's active or previous tab index is out of range.
    MissingSession,    ///< A leaf carries SessionId{}, the sentinel for none.
};

} // namespace vtworkspace
