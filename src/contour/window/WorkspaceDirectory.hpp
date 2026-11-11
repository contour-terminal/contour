// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/PaneKey.hpp>

#include <vtbackend/vt/ProgramStatus.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace contour::window
{

/// What a window lists of the workspaces it can show: every workspace, its tabs and their panes, as plain
/// values. It is built from what the client already holds (the daemon's layouts, its status and the labels
/// of its workspaces), so the list never depends on which workspace is mirrored.
///
/// A workspace will also carry the git state of its panes' working directories; its slot is
/// DirectoryWorkspace.

/// One pane of a tab, with what is known of it.
struct DirectoryPane
{
    PaneKey key;                                        ///< The pane's session.
    std::string title;                                  ///< The pane's title, empty when unknown.
    std::string cwd;                                    ///< The pane's working directory, empty when unknown.
    std::vector<vtbackend::ProgramStatusRecord> status; ///< The program-status records the pane reports.
    vthost::proto::OutputActivity activity = vthost::proto::OutputActivity::None; ///< Output since seen.
    vthost::proto::PaneBell bell = vthost::proto::PaneBell::Silent;               ///< A bell since seen.
    std::optional<vthost::proto::FinishedCommand> finished; ///< The last command that finished, if known.
    bool operator==(DirectoryPane const&) const = default;
};

/// One tab: its panes in tree order (depth-first), so a split tab lists several.
struct DirectoryTab
{
    uint64_t id = 0;                  ///< The tab's id within its workspace.
    std::string title;                ///< The tab's own title as the daemon holds it; empty when it has none.
    std::string label;                ///< What a list shows: the tab's title, else the first pane's.
    std::vector<DirectoryPane> panes; ///< Depth-first.
    bool operator==(DirectoryTab const&) const = default;
};

/// One workspace and its tabs.
struct DirectoryWorkspace
{
    uint64_t id = 0;   ///< The daemon's workspace id.
    std::string label; ///< The workspace's label; empty when it has none.
    std::vector<DirectoryTab> tabs;
    bool operator==(DirectoryWorkspace const&) const = default;
};

/// Every workspace a window can list, in the daemon's order.
struct WorkspaceDirectory
{
    std::vector<DirectoryWorkspace> workspaces;
    std::optional<uint64_t> selected; ///< The workspace the window shows, if it follows one.
    bool operator==(WorkspaceDirectory const&) const = default;
};

/// What is known of one pane besides its place in a layout.
struct PaneStatus
{
    std::string title;
    std::string cwd;
    std::vector<vtbackend::ProgramStatusRecord> status;
    vthost::proto::OutputActivity activity = vthost::proto::OutputActivity::None;
    vthost::proto::PaneBell bell = vthost::proto::PaneBell::Silent;
    std::optional<vthost::proto::FinishedCommand> finished;
};

/// Where a directory gets the status of a pane from.
class PaneStatusLookup
{
  public:
    PaneStatusLookup() = default;
    PaneStatusLookup(PaneStatusLookup const&) = delete;
    PaneStatusLookup& operator=(PaneStatusLookup const&) = delete;
    PaneStatusLookup(PaneStatusLookup&&) = delete;
    PaneStatusLookup& operator=(PaneStatusLookup&&) = delete;
    virtual ~PaneStatusLookup() = default;

    /// @param pane The pane's session.
    /// @return What is known of it; nothing when nothing is.
    [[nodiscard]] virtual std::optional<PaneStatus> statusOf(PaneKey pane) const = 0;
};

/// Where a directory gets the label of a workspace from.
class LabelLookup
{
  public:
    LabelLookup() = default;
    LabelLookup(LabelLookup const&) = delete;
    LabelLookup& operator=(LabelLookup const&) = delete;
    LabelLookup(LabelLookup&&) = delete;
    LabelLookup& operator=(LabelLookup&&) = delete;
    virtual ~LabelLookup() = default;

    /// @param workspace The daemon's workspace id.
    /// @return Its label, empty when it has none.
    [[nodiscard]] virtual std::string labelOf(uint64_t workspace) const = 0;
};

/// Visits the leaves (the panes) of @p node, depth-first.
/// @param node A node of a layout.
/// @param visit Called with each leaf.
template <typename Visit>
    requires std::invocable<Visit const&, vthost::proto::WirePane const&>
void forEachLeaf(vthost::proto::WirePane const& node, Visit const& visit)
{
    if (!node.isSplit())
    {
        visit(node);
        return;
    }
    for (auto const& child: node.children)
        forEachLeaf(child, visit);
}

namespace detail
{
    /// Appends the leaves of @p node to @p panes, depth-first.
    inline void collectPanes(vthost::proto::WirePane const& node,
                             PaneStatusLookup const& status,
                             std::vector<DirectoryPane>& panes)
    {
        forEachLeaf(node, [&](vthost::proto::WirePane const& leaf) {
            auto pane = DirectoryPane {};
            pane.key = PaneKey { leaf.session };
            if (auto known = status.statusOf(pane.key))
            {
                pane.title = std::move(known->title);
                pane.cwd = std::move(known->cwd);
                pane.status = std::move(known->status);
                pane.activity = known->activity;
                pane.bell = known->bell;
                pane.finished = std::move(known->finished);
            }
            panes.push_back(std::move(pane));
        });
    }
} // namespace detail

/// Describes every workspace of a daemon as the directory a window lists.
/// @param inDaemonOrder Each workspace's layout, in the order the daemon lists them.
/// @param status Where the status of a pane comes from.
/// @param labels Where the label of a workspace comes from.
/// @param selected The workspace the window shows, if any.
/// @return The directory.
[[nodiscard]] inline WorkspaceDirectory directoryFromLayouts(
    std::span<vthost::proto::LayoutState const> inDaemonOrder,
    PaneStatusLookup const& status,
    LabelLookup const& labels,
    std::optional<uint64_t> selected)
{
    auto directory = WorkspaceDirectory {};
    directory.selected = selected;
    directory.workspaces.reserve(inDaemonOrder.size());
    for (auto const& layout: inDaemonOrder)
    {
        auto workspace = DirectoryWorkspace {};
        workspace.id = layout.workspace;
        workspace.label = labels.labelOf(layout.workspace);
        workspace.tabs.reserve(layout.tabs.size());
        for (auto const& wireTab: layout.tabs)
        {
            auto tab = DirectoryTab {};
            tab.id = wireTab.tabId;
            tab.title = wireTab.title;
            detail::collectPanes(wireTab.root, status, tab.panes);
            if (!tab.title.empty())
                tab.label = tab.title;
            else if (!tab.panes.empty())
                tab.label = tab.panes.front().title;
            workspace.tabs.push_back(std::move(tab));
        }
        directory.workspaces.push_back(std::move(workspace));
    }
    return directory;
}

} // namespace contour::window
