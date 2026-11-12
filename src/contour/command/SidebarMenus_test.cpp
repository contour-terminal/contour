// SPDX-License-Identifier: Apache-2.0
//
// The sidebar's workspace, tab and pane menus are pure functions of a snapshot, so each row and each rule
// about when it is greyed is asserted with no window and no Qt.

#include <contour/command/SidebarMenus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace contour::command;
namespace actions = contour::actions;

namespace
{
[[nodiscard]] std::vector<std::string> titles(std::vector<ContextMenuEntry> const& entries)
{
    auto result = std::vector<std::string> {};
    for (auto const& entry: entries)
        result.push_back(entry.title);
    return result;
}

[[nodiscard]] ContextMenuEntry const& row(std::vector<ContextMenuEntry> const& entries,
                                          std::string_view title)
{
    auto const it = std::ranges::find(entries, title, &ContextMenuEntry::title);
    REQUIRE(it != entries.end());
    return *it;
}
} // namespace

TEST_CASE("workspace menu rows", "[sidebarmenu]")
{
    auto const menu = buildWorkspaceMenu(WorkspaceMenuState { .position = 1, .count = 3 });
    CHECK(titles(menu)
          == std::vector<std::string> {
              "Rename…", "Mark Seen", "New Tab Here", "", "Move Up", "Move Down", "", "Close Workspace…" });
    CHECK(std::holds_alternative<actions::RenameWorkspace>(row(menu, "Rename…").action));
    CHECK(std::holds_alternative<actions::MarkSeen>(row(menu, "Mark Seen").action));
    CHECK(std::holds_alternative<actions::CreateNewTab>(row(menu, "New Tab Here").action));
    CHECK(std::holds_alternative<actions::CloseWorkspace>(row(menu, "Close Workspace…").action));
    for (auto const& entry: menu)
        CHECK(entry.availability == RowAvailability::Enabled);
}

TEST_CASE("move up and move down grey out at the ends of the list", "[sidebarmenu]")
{
    auto const first = buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 3 });
    CHECK(row(first, "Move Up").availability == RowAvailability::Disabled);
    CHECK(row(first, "Move Down").availability == RowAvailability::Enabled);

    auto const last = buildWorkspaceMenu(WorkspaceMenuState { .position = 2, .count = 3 });
    CHECK(row(last, "Move Up").availability == RowAvailability::Enabled);
    CHECK(row(last, "Move Down").availability == RowAvailability::Disabled);
}

TEST_CASE("close is disabled for the last workspace", "[sidebarmenu]")
{
    auto const last = buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 1 });
    CHECK(row(last, "Close Workspace…").availability == RowAvailability::Disabled);

    auto const several = buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2 });
    CHECK(row(several, "Close Workspace…").availability == RowAvailability::Enabled);
}

TEST_CASE("the sidebar tab menu is the tab strip's menu plus Move to Workspace", "[sidebarmenu]")
{
    auto const state = SidebarTabMenuState {
        .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
        .others = { WorkspaceChoice { .position = 2, .title = "api" },
                    WorkspaceChoice { .position = 3, .title = "Workspace 3" } },
    };
    auto const menu = buildSidebarTabMenu(state);
    auto const strip = buildTabContextMenu(state.tab);
    REQUIRE(menu.size() == strip.size() + 2);
    CHECK(titles(menu).front() == titles(strip).front());
    // Its close row says it asks.
    CHECK(std::ranges::contains(titles(menu), "Close Tab…"));
    CHECK_FALSE(std::ranges::contains(titles(menu), "Close Tab"));
    CHECK(titles(menu).back() == "Move to Workspace");

    auto const& move = menu.back();
    CHECK(move.kind == ContextMenuEntryKind::Submenu);
    CHECK(move.availability == RowAvailability::Enabled);
    REQUIRE(move.children.size() == 2);
    CHECK(move.children[0].title == "api");
    auto const* target = std::get_if<actions::MoveTabToWorkspace>(&move.children[0].action);
    REQUIRE(target != nullptr);
    CHECK(target->index == 2);
    CHECK(move.children[1].title == "Workspace 3");
}

TEST_CASE("Move to Workspace is absent when there is no other workspace", "[sidebarmenu]")
{
    auto const menu =
        buildSidebarTabMenu(SidebarTabMenuState { .tab = TabContextMenuState { .tabCount = 1 } });
    // A submenu with nothing in it is not drawn.
    CHECK(std::ranges::none_of(titles(menu), [](auto const& title) { return title == "Move to Workspace"; }));
}

TEST_CASE("the sidebar pane menu rows", "[sidebarmenu]")
{
    auto const menu = buildSidebarPaneMenu(SidebarPaneMenuState {});
    CHECK(titles(menu) == std::vector<std::string> { "Go to Pane", "Mark Seen", "", "Close Pane…" });
    CHECK(std::holds_alternative<actions::GoToPane>(row(menu, "Go to Pane").action));
    CHECK(std::holds_alternative<actions::ClosePane>(row(menu, "Close Pane…").action));
}

TEST_CASE("every close row of the sidebar's menus ends in an ellipsis, as it always asks first",
          "[sidebarmenu]")
{
    auto const workspace = buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2 });
    auto const tab = buildSidebarTabMenu(
        SidebarTabMenuState { .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 } });
    auto const pane = buildSidebarPaneMenu(SidebarPaneMenuState {});
    CHECK(std::holds_alternative<actions::CloseWorkspace>(row(workspace, "Close Workspace…").action));
    CHECK(std::holds_alternative<actions::CloseTab>(row(tab, "Close Tab…").action));
    CHECK(std::holds_alternative<actions::ClosePane>(row(pane, "Close Pane…").action));
    // The tab strip's own menu keeps its plain "Close Tab".
    CHECK(std::holds_alternative<actions::CloseTab>(
        row(buildTabContextMenu(TabContextMenuState { .tabIndex = 0, .tabCount = 2 }), "Close Tab").action));
}
