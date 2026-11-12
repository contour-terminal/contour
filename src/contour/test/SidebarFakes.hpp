// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// The window a SidebarModel sits in, faked: a directory source that lists what a test sets, and a host that
/// records what the sidebar asks of the window. Shared by the model's own tests and by the QML tests, which
/// drive the real model under the real Sidebar.qml.
#include <contour/window/SidebarModel.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <QtCore/QPointF>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace contour::test
{

/// A source that lists what the test sets and records what it is asked to select.
struct FakeDirectorySource final: window::IWorkspaceDirectorySource
{
    window::WorkspaceDirectory listed;
    std::vector<uint64_t> selected;
    mutable int reads = 0; ///< How often the directory was asked for.

    [[nodiscard]] window::WorkspaceDirectory directory() const override
    {
        ++reads;
        return listed;
    }
    window::SelectResult select(uint64_t workspace) override
    {
        selected.push_back(workspace);
        return window::SelectResult::Requested;
    }
};

/// A window that records what the sidebar asks of it.
struct FakeSidebarHost final: window::ISidebarHost
{
    /// @param listing The source the window lists; may be set later.
    explicit FakeSidebarHost(FakeDirectorySource* listing = nullptr): source { listing } {}

    FakeDirectorySource* source = nullptr;
    /// The host the window's ids are of.
    window::HostIdentity identity { .kind = window::HostKind::Process, .id = "fake-host" };
    /// What the window's host moves.
    window::HostMoves moves = window::HostMoves::Nodes;
    std::optional<window::TabRef> displayed;
    std::vector<window::PaneKey> shown;
    std::vector<uint64_t> shownWorkspaces;
    std::vector<window::PaneKey> seen;
    std::vector<std::pair<uint64_t, std::string>> renamed;
    std::vector<std::pair<window::TabRef, std::string>> renamedTabs;
    std::vector<std::pair<uint64_t, std::size_t>> movedWorkspaces;
    std::vector<std::pair<window::SidebarNode, window::NodeDestination>> movedNodes;
    std::vector<window::SidebarRowKey> menus;
    std::vector<QPointF> menuPositions; ///< Where each menu was asked for, in the window's coordinates.

    [[nodiscard]] window::IWorkspaceDirectorySource* directorySource() const override { return source; }
    [[nodiscard]] window::HostIdentity hostIdentity() const override { return identity; }
    [[nodiscard]] window::HostMoves hostMoves() const override { return moves; }
    [[nodiscard]] std::optional<window::TabRef> displayedTab(
        window::WorkspaceDirectory const& /*directory*/) const override
    {
        return displayed;
    }
    void showPane(window::PaneKey pane) override { shown.push_back(pane); }
    void showWorkspace(uint64_t workspace) override
    {
        shownWorkspaces.push_back(workspace);
        if (source != nullptr)
            std::ignore = source->select(workspace);
    }
    void markSeen(window::PaneKey pane) override { seen.push_back(pane); }
    void renameWorkspace(uint64_t workspace, std::string const& name) override
    {
        renamed.emplace_back(workspace, name);
    }
    void renameTab(window::TabRef tab, std::string const& title) override
    {
        renamedTabs.emplace_back(tab, title);
    }
    void moveWorkspace(uint64_t workspace, std::size_t to) override
    {
        movedWorkspaces.emplace_back(workspace, to);
    }
    void moveNode(window::SidebarNode const& node, window::NodeDestination const& destination) override
    {
        movedNodes.emplace_back(node, destination);
    }
    void openContextMenu(window::SidebarRow const& row, QPointF position) override
    {
        menus.push_back(row.key);
        menuPositions.push_back(position);
    }
};

/// @param key The pane's key value; also its tab's id, by workspaceOf().
/// @param title The pane's title.
/// @return A pane with nothing to report.
[[nodiscard]] inline window::DirectoryPane directoryPaneOf(uint64_t key, std::string title = "shell")
{
    auto pane = window::DirectoryPane {};
    pane.key = window::PaneKey { key };
    pane.title = std::move(title);
    return pane;
}

/// @param id The workspace's id.
/// @param label Its label.
/// @param panes Its panes, one tab each, the tab's id the pane's key.
/// @return The workspace.
[[nodiscard]] inline window::DirectoryWorkspace directoryWorkspaceOf(uint64_t id,
                                                                     std::string label,
                                                                     std::vector<window::DirectoryPane> panes)
{
    auto workspace = window::DirectoryWorkspace {};
    workspace.id = id;
    workspace.label = std::move(label);
    for (auto& pane: panes)
    {
        auto tab = window::DirectoryTab {};
        tab.id = pane.key.value;
        tab.label = pane.title;
        tab.panes.push_back(std::move(pane));
        workspace.tabs.push_back(std::move(tab));
    }
    return workspace;
}

} // namespace contour::test
