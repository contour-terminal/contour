// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarMenuState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace contour::window;

namespace
{
[[nodiscard]] DirectoryWorkspace workspace(uint64_t id, std::string label = {})
{
    return DirectoryWorkspace { .id = id, .label = std::move(label), .tabs = {} };
}
} // namespace

TEST_CASE("move to workspace lists the other workspaces", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10, "api"), workspace(11, ""), workspace(12, "web") },
                             .selected = 11 };
    auto const choices = moveTargetsOf(directory);
    REQUIRE(choices.size() == 2);
    CHECK(choices[0].position == 1);
    CHECK(choices[0].title == "api");
    CHECK(choices[1].position == 3);
    CHECK(choices[1].title == "web");
}

TEST_CASE("an unlabelled workspace is named by its position", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10), workspace(11) }, .selected = 11 };
    auto const choices = moveTargetsOf(directory);
    REQUIRE(choices.size() == 1);
    CHECK(choices[0].title == "Workspace 1");
}

TEST_CASE("a window that follows no workspace can move a tab to any", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10), workspace(11) }, .selected = std::nullopt };
    CHECK(moveTargetsOf(directory).size() == 2);
}

TEST_CASE("merge into tab lists every other tab, grouped by workspace", "[sidebarmenu]")
{
    auto api = workspace(10, "api");
    api.tabs = { DirectoryTab { .id = 1, .title = {}, .label = "server", .panes = {} },
                 DirectoryTab { .id = 2, .title = {}, .label = {}, .panes = {} } };
    auto web = workspace(11);
    web.tabs = { DirectoryTab { .id = 3, .title = {}, .label = "dev", .panes = {} } };
    auto const directory = WorkspaceDirectory { .workspaces = { api, web }, .selected = 10 };

    auto const groups = joinTargetsOf(directory, TabRef { .workspace = 11, .tab = 3 });
    REQUIRE(groups.size() == 1); // workspace 11 has nothing else to join
    CHECK(groups[0].workspace == "api");
    REQUIRE(groups[0].tabs.size() == 2);
    CHECK(groups[0].tabs[0].title == "server");
    CHECK(groups[0].tabs[1].title == "Tab 2");
    CHECK(groups[0].tabs[1].workspace == 1);
    CHECK(groups[0].tabs[1].tab == 2);
    CHECK(tabAtPlaces(directory, 1, 2) == TabRef { .workspace = 10, .tab = 2 });

    auto const all = joinTargetsOf(directory, std::nullopt);
    REQUIRE(all.size() == 2);
    CHECK(all[1].workspace == "Workspace 2");
    CHECK(tabAtPlaces(directory, 2, 1) == TabRef { .workspace = 11, .tab = 3 });
    CHECK_FALSE(tabAtPlaces(directory, 2, 2));
    CHECK_FALSE(tabAtPlaces(directory, 0, 1));
    CHECK_FALSE(tabAtPlaces(directory, 3, 1));
}

TEST_CASE("a menu's tab is the one it offered, whatever moves while it is open", "[sidebarmenu]")
{
    auto api = workspace(10, "api");
    api.tabs = { DirectoryTab { .id = 1, .title = {}, .label = "server", .panes = {} },
                 DirectoryTab { .id = 2, .title = {}, .label = {}, .panes = {} } };
    auto web = workspace(11);
    web.tabs = { DirectoryTab { .id = 3, .title = {}, .label = "dev", .panes = {} } };
    auto directory = WorkspaceDirectory { .workspaces = { api, web }, .selected = 10 };
    auto const offered = offeredTabsOf(directory);
    REQUIRE(offered.size() == 3);

    // Tab 1 closes while the menu is open: the row for api's second tab still names tab 2, where the places
    // now name nothing.
    directory.workspaces[0].tabs.erase(directory.workspaces[0].tabs.begin());
    CHECK(offeredTabAt(offered, 1, 2) == TabRef { .workspace = 10, .tab = 2 });
    CHECK_FALSE(tabAtPlaces(directory, 1, 2));
    CHECK(offeredTabAt(offered, 2, 1) == TabRef { .workspace = 11, .tab = 3 });
    CHECK_FALSE(offeredTabAt(offered, 2, 2));
    CHECK_FALSE(offeredTabAt({}, 1, 1));
}

TEST_CASE("the sidebar's menus offer what the window's host moves", "[sidebarmenu]")
{
    CHECK(menuMovesOf(HostMoves::Nodes) == contour::command::MenuMoves::Nodes);
    CHECK(menuMovesOf(HostMoves::Tabs) == contour::command::MenuMoves::Tabs);
    CHECK(menuMovesOf(HostMoves::Nothing) == contour::command::MenuMoves::Nothing);
}
