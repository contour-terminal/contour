// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarMenuState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>

using namespace contour::window;

namespace
{
[[nodiscard]] DirectoryWorkspace workspace(uint64_t id, std::string name = {})
{
    return DirectoryWorkspace { .id = id, .name = std::move(name), .tabs = {} };
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
    api.tabs = { DirectoryTab { .id = 1, .title = {}, .name = "server", .panes = {} },
                 DirectoryTab { .id = 2, .title = {}, .name = {}, .panes = {} } };
    auto web = workspace(11);
    web.tabs = { DirectoryTab { .id = 3, .title = {}, .name = "dev", .panes = {} } };
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
    api.tabs = { DirectoryTab { .id = 1, .title = {}, .name = "server", .panes = {} },
                 DirectoryTab { .id = 2, .title = {}, .name = {}, .panes = {} } };
    auto web = workspace(11);
    web.tabs = { DirectoryTab { .id = 3, .title = {}, .name = "dev", .panes = {} } };
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

TEST_CASE("a workspace's menu state is its place, the count, and what its only tab can join", "[sidebarmenu]")
{
    auto one = workspace(10, "api");
    one.tabs = { DirectoryTab { .id = 1, .title = {}, .name = "shell", .panes = {} } };
    auto two = workspace(11, "web");
    two.tabs = { DirectoryTab { .id = 2, .title = {}, .name = "a", .panes = {} },
                 DirectoryTab { .id = 3, .title = {}, .name = "b", .panes = {} } };
    two.color = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    auto const directory = WorkspaceDirectory { .workspaces = { one, two }, .selected = 10 };
    auto const palette =
        std::array { vtbackend::RGBColor { 0xC0, 0x39, 0x2B }, vtbackend::RGBColor { 0xC2, 0x54, 0x8F } };

    auto const single = workspaceMenuStateOf(directory,
                                             10,
                                             contour::command::MenuMoves::Nodes,
                                             contour::command::MenuColoring::Available,
                                             palette);
    CHECK(single.position == 0);
    CHECK(single.count == 2);
    REQUIRE(single.joinable.size() == 1);
    CHECK(single.joinable[0].tabs.size() == 2);
    CHECK(std::ranges::equal(single.palette, palette));
    CHECK_FALSE(single.color.has_value());
    CHECK(single.coloring == contour::command::MenuColoring::Available);

    auto const several = workspaceMenuStateOf(directory,
                                              11,
                                              contour::command::MenuMoves::Tabs,
                                              contour::command::MenuColoring::Unavailable,
                                              palette);
    CHECK(several.position == 1);
    CHECK(several.moves == contour::command::MenuMoves::Tabs);
    CHECK(several.joinable.empty());
    CHECK(several.color == vtbackend::RGBColor { 0xC2, 0x54, 0x8F });
    CHECK(several.coloring == contour::command::MenuColoring::Unavailable);

    auto const unlisted = workspaceMenuStateOf(directory,
                                               99,
                                               contour::command::MenuMoves::Nothing,
                                               contour::command::MenuColoring::Unavailable,
                                               palette);
    CHECK(unlisted.position == 0);
    CHECK(unlisted.moves == contour::command::MenuMoves::Nothing);
    CHECK_FALSE(unlisted.color.has_value());
    CHECK(unlisted.coloring == contour::command::MenuColoring::Unavailable);
}

TEST_CASE("a daemon row's menu state mirrors its buttons", "[sidebarmenu]")
{
    auto daemonRow =
        [](DaemonRef daemon, DaemonLinkState state, DaemonLocality locality, RowPending pending) {
            auto row = SidebarRow {};
            row.key = SidebarRowKey { .kind = SidebarRowKind::Daemon,
                                      .daemon = std::move(daemon),
                                      .instance = {},
                                      .workspace = 0,
                                      .tab = 0,
                                      .pane = {} };
            row.linkState = state;
            row.locality = locality;
            row.link = DaemonRowLink::Observer;
            row.pending = pending;
            return row;
        };
    auto const verbsOf = [](SidebarRow const& row) {
        auto const buttons = daemonRowButtons(daemonRowFactsOf(row), row.pending);
        auto verbs = std::array<contour::command::DaemonRowVerb, 2> {};
        std::ranges::transform(buttons, verbs.begin(), [](DaemonRowButton button) {
            switch (button)
            {
                case DaemonRowButton::None: return contour::command::DaemonRowVerb::None;
                case DaemonRowButton::Connect: return contour::command::DaemonRowVerb::Connect;
                case DaemonRowButton::Start: return contour::command::DaemonRowVerb::Start;
                case DaemonRowButton::Cancel: return contour::command::DaemonRowVerb::Cancel;
                case DaemonRowButton::Remove: return contour::command::DaemonRowVerb::Remove;
            }
            return contour::command::DaemonRowVerb::None;
        });
        return verbs;
    };
    using contour::command::DaemonRowVerb;
    using contour::command::OpenOffer;
    constexpr auto OtherDaemon = contour::command::RowOrigin::OtherDaemon;

    auto const stopped =
        daemonRow(localDaemonRef("work"), DaemonLinkState::NotRunning, DaemonLocality::Local, RowPending::No);
    auto const stoppedState = sidebarDaemonMenuStateOf(stopped, OtherDaemon);
    CHECK(stoppedState.verbs == std::array { DaemonRowVerb::Start, DaemonRowVerb::Remove });
    CHECK(stoppedState.verbs == verbsOf(stopped));
    // A window opened on a daemon that is not running could only fail to show.
    CHECK(stoppedState.open == OpenOffer::NotOffered);

    auto const connecting = daemonRow(
        hostDaemonRef("starfive"), DaemonLinkState::Connecting, DaemonLocality::Remote, RowPending::No);
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).verbs == verbsOf(connecting));
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).verbs[0] == DaemonRowVerb::Cancel);
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).cancels
          == contour::command::DaemonCancelTarget::Connect);
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).open == OpenOffer::Offered);

    auto const jumping = daemonRow(
        hostDaemonRef("starfive"), DaemonLinkState::Connected, DaemonLocality::Remote, RowPending::Yes);
    CHECK(sidebarDaemonMenuStateOf(jumping, OtherDaemon).verbs
          == std::array { DaemonRowVerb::Cancel, DaemonRowVerb::None });
    CHECK(sidebarDaemonMenuStateOf(jumping, OtherDaemon).cancels
          == contour::command::DaemonCancelTarget::Jump);

    // Hide from Sidebar: only for a daemon no window of this process is on, and never for a link.
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).hide == contour::command::HideOffer::Offered);
    for (auto const link: { DaemonRowLink::OwnWindow, DaemonRowLink::OtherWindow })
    {
        auto onIt = connecting;
        onIt.link = link;
        CHECK(sidebarDaemonMenuStateOf(onIt, OtherDaemon).hide == contour::command::HideOffer::NotOffered);
    }
    CHECK(sidebarDaemonMenuStateOf(
              daemonRow(linkDaemonRef(4), DaemonLinkState::Connected, DaemonLocality::Remote, RowPending::No),
              OtherDaemon)
              .hide
          == contour::command::HideOffer::NotOffered);

    // Open in New Window, by the daemon's state: offered wherever a window on it could show.
    for (auto const state: { DaemonLinkState::TooOld, DaemonLinkState::Failed, DaemonLinkState::NotRunning })
        CHECK(sidebarDaemonMenuStateOf(
                  daemonRow(hostDaemonRef("starfive"), state, DaemonLocality::Remote, RowPending::No),
                  OtherDaemon)
                  .open
              == OpenOffer::NotOffered);
    for (auto const state: { DaemonLinkState::NotConnected,
                             DaemonLinkState::Connected,
                             DaemonLinkState::Reconnecting,
                             DaemonLinkState::NeedsInput })
        CHECK(sidebarDaemonMenuStateOf(
                  daemonRow(hostDaemonRef("starfive"), state, DaemonLocality::Remote, RowPending::No),
                  OtherDaemon)
                  .open
              == OpenOffer::Offered);

    // A daemon no switch can name: reached by a link, or none at all.
    auto const linked =
        daemonRow(linkDaemonRef(3), DaemonLinkState::Connected, DaemonLocality::Remote, RowPending::No);
    CHECK(sidebarDaemonMenuStateOf(linked, OtherDaemon).open == OpenOffer::NotOffered);
    CHECK(openOfferOf(DaemonRef {}) == OpenOffer::NotOffered);
    CHECK(openOfferOf(hostDaemonRef("starfive")) == OpenOffer::Offered);

    // New Workspace: made on the window's own daemon whatever it is; on another, where a window could open.
    using contour::command::NewWorkspaceOffer;
    constexpr auto Own = contour::command::RowOrigin::Own;
    CHECK(sidebarDaemonMenuStateOf(connecting, OtherDaemon).newWorkspace == NewWorkspaceOffer::Offered);
    CHECK(sidebarDaemonMenuStateOf(stopped, OtherDaemon).newWorkspace == NewWorkspaceOffer::NotOffered);
    CHECK(sidebarDaemonMenuStateOf(linked, OtherDaemon).newWorkspace == NewWorkspaceOffer::NotOffered);
    CHECK(sidebarDaemonMenuStateOf(linked, Own).newWorkspace == NewWorkspaceOffer::Offered);
}

TEST_CASE("an other daemon's workspace menu state counts that daemon's workspaces", "[sidebarmenu]")
{
    auto build = workspace(5, "build");
    build.color = vtbackend::RGBColor { 0x29, 0x80, 0xB9 };
    auto const section = WorkspaceDirectory { .workspaces = { workspace(4, "api"), build, workspace(6) },
                                              .selected = std::nullopt };
    auto const palette = std::array { vtbackend::RGBColor { 0xC0, 0x39, 0x2B } };
    auto const row = SidebarRowKey { .kind = SidebarRowKind::Workspace,
                                     .daemon = hostDaemonRef("starfive"),
                                     .instance = DaemonInstance { 9 },
                                     .workspace = 5,
                                     .tab = 0,
                                     .pane = {} };
    auto const state = otherDaemonWorkspaceMenuStateOf(section, row, palette);
    CHECK(state.position == 1);
    CHECK(state.count == 3);
    CHECK(state.color == build.color);
    // An observed daemon keeps the workspace's colour (spec §2).
    CHECK(state.coloring == contour::command::MenuColoring::Available);
    CHECK(state.origin == contour::command::RowOrigin::OtherDaemon);
    CHECK(state.open == contour::command::OpenOffer::Offered);
    CHECK(std::ranges::equal(state.palette, palette));
    // Whose row it is: another daemon's, unless the window is on it or on none.
    CHECK(rowOriginOf(row.daemon, localDaemonRef("main")) == contour::command::RowOrigin::OtherDaemon);
    CHECK(rowOriginOf(row.daemon, row.daemon) == contour::command::RowOrigin::Own);
    CHECK(rowOriginOf(row.daemon, std::nullopt) == contour::command::RowOrigin::Own);
}
