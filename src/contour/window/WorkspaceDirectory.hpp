// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/PaneKey.hpp>

#include <vtbackend/screen/GitHead.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace contour::window
{

/// What a window lists of the workspaces it can show: every workspace, its tabs and their panes, as plain
/// values. It is built from what the client already holds (the daemon's layouts, its status and what it
/// states of its workspaces: label, control id, git state), so the list never depends on which workspace is
/// mirrored.

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
    /// Which occurrence of each flag the daemon last stated, from one counter that only grows; 0 for none.
    /// A window acknowledges a flag by remembering its serial (SessionAttention), so an old event stays seen
    /// and a new one rings again. An in-process pane keeps them 0: its flags live in the window's ledger.
    uint64_t activitySerial = 0; ///< Serial of the last output the daemon stated.
    uint64_t bellSerial = 0;     ///< Serial of the last bell the daemon stated.
    uint64_t finishedSerial = 0; ///< Serial of the last finished command the daemon stated.
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

/// The git state of a workspace: the repository its active pane's working directory is in.
struct DirectoryGit
{
    std::string branch; ///< The branch or ref, or the short object id.
    vtbackend::GitHeadKind kind = vtbackend::GitHeadKind::Branch; ///< What @ref branch names.
    std::optional<uint32_t> ahead {};  ///< Commits ahead of its upstream, if it has one.
    std::optional<uint32_t> behind {}; ///< Commits behind its upstream, if it has one.
    bool operator==(DirectoryGit const&) const = default;
};

/// One workspace and its tabs.
struct DirectoryWorkspace
{
    uint64_t id = 0;   ///< The daemon's workspace id.
    std::string label; ///< The workspace's label; empty when it has none.
    std::vector<DirectoryTab> tabs;
    std::array<uint8_t, 16> controlId {}; ///< Its control-API UUID; all zero while the daemon stated none.
    std::optional<DirectoryGit> git {};   ///< Its git state; none when it is in no repository, or unknown.
    /// Its 0-based place in the daemon's model, as the daemon stated it (WorkspaceMeta::position); none until
    /// it did. The daemon's order may count workspaces this directory does not list (a tabless one), so a
    /// workspace's place in the directory need not be its place there.
    std::optional<uint32_t> position {};
    bool operator==(DirectoryWorkspace const&) const = default;
};

/// @param workspace A workspace of a directory.
/// @param listIndex Its 0-based place in that directory.
/// @return What a window calls it: its label, or "Workspace N" -- N its 1-based place in its host's order,
///         as `contour ls` and `workspace.list` number it, and its place in the directory while the host
///         stated none.
[[nodiscard]] inline std::string workspaceTitleOf(DirectoryWorkspace const& workspace, std::size_t listIndex)
{
    if (!workspace.label.empty())
        return workspace.label;
    return std::format("Workspace {}", workspace.position.value_or(static_cast<uint32_t>(listIndex)) + 1);
}

/// Every workspace a window can list, in the daemon's order.
struct WorkspaceDirectory
{
    std::vector<DirectoryWorkspace> workspaces;
    std::optional<uint64_t> selected; ///< The workspace the window shows, if it follows one.
    bool operator==(WorkspaceDirectory const&) const = default;
};

/// A tab of a directory with the workspace that lists it. Both point into the directory searched, so they
/// live as long as it does.
struct ListedTab
{
    DirectoryWorkspace const* workspace = nullptr; ///< The workspace listing it.
    DirectoryTab const* tab = nullptr;             ///< The tab.
};

/// @param directory A window's directory.
/// @param workspace A workspace's id.
/// @return The workspace @p directory lists under that id; null for none.
[[nodiscard]] inline DirectoryWorkspace const* findWorkspace(WorkspaceDirectory const& directory,
                                                             uint64_t workspace) noexcept
{
    auto const found = std::ranges::find(directory.workspaces, workspace, &DirectoryWorkspace::id);
    return found != directory.workspaces.end() ? &*found : nullptr;
}

/// @param workspace A workspace of a directory.
/// @param tab A tab's id.
/// @return The tab @p workspace lists under that id; null for none.
[[nodiscard]] inline DirectoryTab const* findTab(DirectoryWorkspace const& workspace, uint64_t tab) noexcept
{
    auto const found = std::ranges::find(workspace.tabs, tab, &DirectoryTab::id);
    return found != workspace.tabs.end() ? &*found : nullptr;
}

/// @param directory A window's directory.
/// @param workspace The id of the workspace listing the tab.
/// @param tab The tab's id.
/// @return The tab; none where either id names nothing listed.
[[nodiscard]] inline std::optional<ListedTab> findTab(WorkspaceDirectory const& directory,
                                                      uint64_t workspace,
                                                      uint64_t tab) noexcept
{
    auto const* const holder = findWorkspace(directory, workspace);
    auto const* const found = holder != nullptr ? findTab(*holder, tab) : nullptr;
    if (found == nullptr)
        return std::nullopt;
    return ListedTab { .workspace = holder, .tab = found };
}

/// @param directory A window's directory.
/// @param tab A tab's id, whichever workspace lists it.
/// @return The first tab listed under that id; none for none.
[[nodiscard]] inline std::optional<ListedTab> findTabAnywhere(WorkspaceDirectory const& directory,
                                                              uint64_t tab) noexcept
{
    for (auto const& workspace: directory.workspaces)
        if (auto const* const found = findTab(workspace, tab))
            return ListedTab { .workspace = &workspace, .tab = found };
    return std::nullopt;
}

/// @param directory A window's directory.
/// @param pane A pane's key.
/// @return The tab listing @p pane; none for none.
[[nodiscard]] inline std::optional<ListedTab> tabHoldingPane(WorkspaceDirectory const& directory,
                                                             PaneKey pane) noexcept
{
    for (auto const& workspace: directory.workspaces)
        for (auto const& tab: workspace.tabs)
            if (std::ranges::contains(tab.panes, pane, &DirectoryPane::key))
                return ListedTab { .workspace = &workspace, .tab = &tab };
    return std::nullopt;
}

// The lookups above point into the directory searched: one of a temporary would dangle at once, so a
// directory() result is held in a local first.
DirectoryWorkspace const* findWorkspace(WorkspaceDirectory const&& directory, uint64_t workspace) = delete;
DirectoryTab const* findTab(DirectoryWorkspace const&& workspace, uint64_t tab) = delete;
std::optional<ListedTab> findTab(WorkspaceDirectory const&& directory,
                                 uint64_t workspace,
                                 uint64_t tab) = delete;
std::optional<ListedTab> findTabAnywhere(WorkspaceDirectory const&& directory, uint64_t tab) = delete;
std::optional<ListedTab> tabHoldingPane(WorkspaceDirectory const&& directory, PaneKey pane) = delete;

/// @param tabs Tabs of a directory.
/// @return The session of every pane of them, in order.
[[nodiscard]] inline std::vector<uint64_t> sessionsOf(std::span<DirectoryTab const> tabs)
{
    return std::ranges::to<std::vector>(
        tabs | std::views::transform(&DirectoryTab::panes) | std::views::join
        | std::views::transform([](DirectoryPane const& pane) { return pane.key.value; }));
}

/// What is known of one pane besides its place in a layout.
struct PaneStatus
{
    std::string title;
    std::string cwd;
    std::vector<vtbackend::ProgramStatusRecord> status;
    vthost::proto::OutputActivity activity = vthost::proto::OutputActivity::None;
    vthost::proto::PaneBell bell = vthost::proto::PaneBell::Silent;
    std::optional<vthost::proto::FinishedCommand> finished;
    uint64_t activitySerial = 0; ///< @see DirectoryPane::activitySerial
    uint64_t bellSerial = 0;     ///< @see DirectoryPane::bellSerial
    uint64_t finishedSerial = 0; ///< @see DirectoryPane::finishedSerial
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

/// What a daemon states of one workspace besides its layout.
struct WorkspaceMetadata
{
    std::string label;                    ///< Its label; empty when it has none.
    std::array<uint8_t, 16> controlId {}; ///< Its control-API UUID; all zero when unknown.
    std::optional<DirectoryGit> git {};   ///< Its git state; none when it is in no repository, or unknown.
    /// Its 0-based place in the daemon's workspace order, once the daemon stated it; a directory lists the
    /// workspaces by it, those without one after the rest in id order.
    std::optional<uint32_t> position {};
    bool operator==(WorkspaceMetadata const&) const = default;
};

/// Where a directory gets what is stated of a workspace (its label, control id and git state) from.
class WorkspaceMetaLookup
{
  public:
    WorkspaceMetaLookup() = default;
    WorkspaceMetaLookup(WorkspaceMetaLookup const&) = delete;
    WorkspaceMetaLookup& operator=(WorkspaceMetaLookup const&) = delete;
    WorkspaceMetaLookup(WorkspaceMetaLookup&&) = delete;
    WorkspaceMetaLookup& operator=(WorkspaceMetaLookup&&) = delete;
    virtual ~WorkspaceMetaLookup() = default;

    /// @param workspace The daemon's workspace id.
    /// @return What is stated of it; empty fields where nothing is.
    [[nodiscard]] virtual WorkspaceMetadata metaOf(uint64_t workspace) const = 0;
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
                pane.activitySerial = known->activitySerial;
                pane.bellSerial = known->bellSerial;
                pane.finishedSerial = known->finishedSerial;
            }
            panes.push_back(std::move(pane));
        });
    }
} // namespace detail

/// Describes every workspace of a daemon as the directory a window lists.
/// @param inDaemonOrder Each workspace's layout, in the order the daemon lists them.
/// @param status Where the status of a pane comes from.
/// @param metadata Where the label, control id and git state of a workspace come from.
/// @param selected The workspace the window shows, if any.
/// @return The directory.
[[nodiscard]] inline WorkspaceDirectory directoryFromLayouts(
    std::span<vthost::proto::LayoutState const> inDaemonOrder,
    PaneStatusLookup const& status,
    WorkspaceMetaLookup const& metadata,
    std::optional<uint64_t> selected)
{
    auto directory = WorkspaceDirectory {};
    directory.selected = selected;
    directory.workspaces.reserve(inDaemonOrder.size());
    for (auto const& layout: inDaemonOrder)
    {
        auto workspace = DirectoryWorkspace {};
        workspace.id = layout.workspace;
        auto stated = metadata.metaOf(layout.workspace);
        workspace.label = std::move(stated.label);
        workspace.controlId = stated.controlId;
        workspace.git = std::move(stated.git);
        workspace.position = stated.position;
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
