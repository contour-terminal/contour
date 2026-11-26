// SPDX-License-Identifier: Apache-2.0
//
// The sidebar's workspace, tab and pane menus are pure functions of a snapshot, so each row and each rule
// about when it is greyed is asserted with no window and no Qt.

#include <contour/command/SidebarMenus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <ranges>
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
          == std::vector<std::string> { "Open in New Window",
                                        "",
                                        "Workspace Color",
                                        "Rename…",
                                        "Mark Seen",
                                        "New Tab Here",
                                        "",
                                        "Move Up",
                                        "Move Down",
                                        "",
                                        "Close Workspace…" });
    CHECK(std::holds_alternative<actions::RenameWorkspace>(row(menu, "Rename…").action));
    CHECK(std::holds_alternative<actions::MarkSeen>(row(menu, "Mark Seen").action));
    CHECK(std::holds_alternative<actions::CreateNewTab>(row(menu, "New Tab Here").action));
    CHECK(std::holds_alternative<actions::CloseWorkspace>(row(menu, "Close Workspace…").action));
    for (auto const& entry: menu)
        CHECK(entry.availability == RowAvailability::Enabled);
}

TEST_CASE("the workspace menu starts with Open in New Window, then Workspace Color and Rename",
          "[sidebarmenu]")
{
    constexpr auto Red = vtbackend::RGBColor { 0xC0, 0x39, 0x2B };
    constexpr auto Magenta = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    constexpr auto Blue = vtbackend::RGBColor { 0x29, 0x80, 0xB9 };
    auto const plain = buildWorkspaceMenu(WorkspaceMenuState { .position = 1, .count = 3 });
    auto const menu = buildWorkspaceMenu(WorkspaceMenuState {
        .position = 1, .count = 3, .palette = { Red, Magenta, Blue }, .color = Magenta });

    REQUIRE(menu.size() >= 4);
    CHECK(menu[0].title == "Open in New Window");
    CHECK(menu[1].kind == ContextMenuEntryKind::Separator);
    auto const& colour = menu[2];
    CHECK(colour.kind == ContextMenuEntryKind::Submenu);
    CHECK(colour.title == "Workspace Color");
    CHECK(colour.availability == RowAvailability::Enabled);
    CHECK(menu[3].title == "Rename…");
    CHECK(std::holds_alternative<actions::RenameWorkspace>(menu[3].action));
    // The rest is as it was.
    CHECK(std::ranges::equal(titles(menu) | std::views::drop(3), titles(plain) | std::views::drop(3)));

    // The palette's swatches, the workspace's own one checked, then a custom colour and a reset.
    REQUIRE(colour.children.size() == 6);
    auto const swatches = std::array { Red, Magenta, Blue };
    for (auto const index: std::views::iota(size_t { 0 }, swatches.size()))
    {
        auto const& swatch = colour.children[index];
        CAPTURE(index);
        auto const* const set = std::get_if<actions::SetWorkspaceColor>(&swatch.action);
        REQUIRE(set != nullptr);
        CHECK(set->color == swatches[index]);
        CHECK(swatch.swatch == swatches[index]);
        CHECK(swatch.check == (swatches[index] == Magenta ? RowCheck::Checked : RowCheck::Unchecked));
    }
    CHECK(colour.children[3].kind == ContextMenuEntryKind::Separator);
    auto const& custom = colour.children[4];
    CHECK(custom.title == "Custom…");
    auto const* const bare = std::get_if<actions::SetWorkspaceColor>(&custom.action);
    REQUIRE(bare != nullptr);
    CHECK_FALSE(bare->color.has_value());
    auto const& reset = colour.children[5];
    CHECK(reset.title == "Reset");
    CHECK(std::holds_alternative<actions::ResetWorkspaceColor>(reset.action));
    CHECK(reset.availability == RowAvailability::Enabled);
}

TEST_CASE("an uncoloured workspace's Reset is greyed out and nothing is checked", "[sidebarmenu]")
{
    constexpr auto Red = vtbackend::RGBColor { 0xC0, 0x39, 0x2B };
    auto const menu =
        buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 1, .palette = { Red } });
    auto const& colour = row(menu, "Workspace Color");
    REQUIRE(colour.children.size() == 4);
    CHECK(colour.children[0].check == RowCheck::Unchecked);
    CHECK(colour.children[3].title == "Reset");
    CHECK(colour.children[3].availability == RowAvailability::Disabled);
}

TEST_CASE("Workspace Color is greyed out where the host keeps no colour", "[sidebarmenu]")
{
    constexpr auto Red = vtbackend::RGBColor { 0xC0, 0x39, 0x2B };
    auto const menu = buildWorkspaceMenu(WorkspaceMenuState {
        .position = 0, .count = 2, .palette = { Red }, .coloring = MenuColoring::Unavailable });
    CHECK(row(menu, "Workspace Color").availability == RowAvailability::Disabled);
    // Everything else is offered as before.
    CHECK(row(menu, "Rename…").availability == RowAvailability::Enabled);
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

TEST_CASE("the sidebar tab menu is the tab strip's menu plus the splits and Move to Workspace",
          "[sidebarmenu]")
{
    auto const state = SidebarTabMenuState {
        .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
        .others = { WorkspaceChoice { .position = 2, .title = "api" },
                    WorkspaceChoice { .position = 3, .title = "Workspace 3" } },
    };
    auto const menu = buildSidebarTabMenu(state);
    auto const strip = buildTabContextMenu(state.tab);
    REQUIRE(menu.size() == strip.size() + 8);
    CHECK(titles(menu).front() == "Open in New Window");
    CHECK(titles(menu)[2] == titles(strip).front());
    // Its close row says it asks.
    CHECK(std::ranges::contains(titles(menu), "Close Tab…"));
    CHECK_FALSE(std::ranges::contains(titles(menu), "Close Tab"));
    // Nothing to merge into: no Merge into Tab.
    CHECK(std::ranges::equal(titles(menu) | std::views::drop(strip.size() + 5),
                             std::vector<std::string> { "", "Move to Workspace", "Move to New Workspace" }));

    auto const& move = row(menu, "Move to Workspace");
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
    CHECK(titles(menu)
          == std::vector<std::string> { "Go to Pane",
                                        "Open in New Window",
                                        "",
                                        "Mark Seen",
                                        "Split Vertically",
                                        "Split Horizontally",
                                        "",
                                        "Move to New Tab",
                                        "Move to New Workspace",
                                        "",
                                        "Close Pane…" });
    CHECK(std::holds_alternative<actions::GoToPane>(row(menu, "Go to Pane").action));
    CHECK(std::holds_alternative<actions::MoveToNewTab>(row(menu, "Move to New Tab").action));
    CHECK(std::holds_alternative<actions::MoveToNewWorkspace>(row(menu, "Move to New Workspace").action));
    CHECK(std::holds_alternative<actions::ClosePane>(row(menu, "Close Pane…").action));
}

TEST_CASE("the pane menu offers both splits after Mark Seen", "[sidebarmenu]")
{
    auto const menu = buildSidebarPaneMenu(SidebarPaneMenuState {});
    CHECK(std::ranges::equal(
        titles(menu) | std::views::drop(3) | std::views::take(4),
        std::vector<std::string> { "Mark Seen", "Split Vertically", "Split Horizontally", "" }));
    CHECK(std::holds_alternative<actions::SplitVertical>(row(menu, "Split Vertically").action));
    CHECK(std::holds_alternative<actions::SplitHorizontal>(row(menu, "Split Horizontally").action));
}

TEST_CASE("the sidebar's tab menu offers both splits", "[sidebarmenu]")
{
    auto const state = SidebarTabMenuState {
        .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
        .others = { WorkspaceChoice { .position = 2, .title = "api" } },
    };
    auto const menu = buildSidebarTabMenu(state);
    auto const strip = buildTabContextMenu(state.tab);
    CHECK(std::ranges::equal(
        titles(menu) | std::views::drop(strip.size() + 2) | std::views::take(5),
        std::vector<std::string> { "", "Split Vertically", "Split Horizontally", "", "Move to Workspace" }));
    CHECK(std::holds_alternative<actions::SplitVertical>(row(menu, "Split Vertically").action));
    CHECK(std::holds_alternative<actions::SplitHorizontal>(row(menu, "Split Horizontally").action));
}

TEST_CASE("the workspace menu offers no split", "[sidebarmenu]")
{
    auto const menu = buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2 });
    CHECK_FALSE(std::ranges::contains(titles(menu), "Split Vertically"));
    CHECK_FALSE(std::ranges::contains(titles(menu), "Split Horizontally"));
}

namespace
{
/// Two workspaces' joinable tabs: api's server and its second tab, and docs' notes.
[[nodiscard]] std::vector<TabChoiceGroup> joinable()
{
    return {
        TabChoiceGroup { .workspace = "api",
                         .tabs = { TabChoice { .workspace = 1, .tab = 1, .title = "server" },
                                   TabChoice { .workspace = 1, .tab = 2, .title = "Tab 2" } } },
        TabChoiceGroup { .workspace = "docs",
                         .tabs = { TabChoice { .workspace = 3, .tab = 1, .title = "notes" } } },
    };
}

/// Checks @p submenu lists joinable(): each workspace's name as a header, its tabs as MoveToTab rows.
void checkJoinRows(ContextMenuEntry const& submenu)
{
    CHECK(submenu.kind == ContextMenuEntryKind::Submenu);
    CHECK(titles(submenu.children) == std::vector<std::string> { "api", "server", "Tab 2", "docs", "notes" });
    CHECK(submenu.children[0].kind == ContextMenuEntryKind::Header);
    CHECK(submenu.children[3].kind == ContextMenuEntryKind::Header);
    auto const* notes = std::get_if<actions::MoveToTab>(&submenu.children[4].action);
    REQUIRE(notes != nullptr);
    CHECK(notes->workspace == 3);
    CHECK(notes->tab == 1);
}
} // namespace

TEST_CASE("Move to Tab and Merge into Tab list the other tabs under their workspaces", "[sidebarmenu]")
{
    checkJoinRows(row(buildSidebarPaneMenu(SidebarPaneMenuState { .joinable = joinable() }), "Move to Tab"));
    checkJoinRows(row(
        buildSidebarTabMenu(SidebarTabMenuState { .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
                                                  .others = {},
                                                  .joinable = joinable(),
                                                  .workspaceTabs = 2 }),
        "Merge into Tab"));
    checkJoinRows(
        row(buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2, .joinable = joinable() }),
            "Merge into Tab"));
    // A workspace of several tabs merges into nothing.
    CHECK_FALSE(std::ranges::contains(
        titles(buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2 })), "Merge into Tab"));
}

TEST_CASE("Move to New Workspace greys out for a workspace's only tab", "[sidebarmenu]")
{
    auto const only = buildSidebarTabMenu(SidebarTabMenuState {
        .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 1 }, .workspaceTabs = 1 });
    CHECK(row(only, "Move to New Workspace").availability == RowAvailability::Disabled);
    auto const several = buildSidebarTabMenu(SidebarTabMenuState {
        .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 }, .workspaceTabs = 2 });
    CHECK(row(several, "Move to New Workspace").availability == RowAvailability::Enabled);
    CHECK(std::holds_alternative<actions::MoveToNewWorkspace>(row(several, "Move to New Workspace").action));
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

TEST_CASE("the pane menu greys out a move that would only rebuild what the pane already is", "[sidebarmenu]")
{
    // The only pane of its tab is a tab of its own already, and the only pane of its workspace a workspace.
    auto const alone = buildSidebarPaneMenu(SidebarPaneMenuState { .tabPanes = 1, .workspaceTabs = 1 });
    CHECK(row(alone, "Move to New Tab").availability == RowAvailability::Disabled);
    CHECK(row(alone, "Move to New Workspace").availability == RowAvailability::Disabled);
    // Its tab's only pane, beside other tabs: a new workspace moves it; a new tab would not.
    auto const ownTab = buildSidebarPaneMenu(SidebarPaneMenuState { .tabPanes = 1, .workspaceTabs = 2 });
    CHECK(row(ownTab, "Move to New Tab").availability == RowAvailability::Disabled);
    CHECK(row(ownTab, "Move to New Workspace").availability == RowAvailability::Enabled);
    // A pane of a split tab moves either way.
    auto const split = buildSidebarPaneMenu(SidebarPaneMenuState { .tabPanes = 2, .workspaceTabs = 1 });
    CHECK(row(split, "Move to New Tab").availability == RowAvailability::Enabled);
    CHECK(row(split, "Move to New Workspace").availability == RowAvailability::Enabled);
}

TEST_CASE("the sidebar menus grey out the moves the window's host does not make", "[sidebarmenu]")
{
    auto const paneOn = [](MenuMoves moves) {
        return buildSidebarPaneMenu(SidebarPaneMenuState {
            .joinable = joinable(), .tabPanes = 2, .workspaceTabs = 2, .moves = moves });
    };
    auto const tabOn = [](MenuMoves moves) {
        return buildSidebarTabMenu(
            SidebarTabMenuState { .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
                                  .others = { WorkspaceChoice { .position = 2, .title = "api" } },
                                  .joinable = joinable(),
                                  .workspaceTabs = 2,
                                  .moves = moves });
    };
    auto const workspaceOn = [](MenuMoves moves) {
        return buildWorkspaceMenu(
            WorkspaceMenuState { .position = 0, .count = 2, .joinable = joinable(), .moves = moves });
    };
    struct Case
    {
        MenuMoves moves;
        RowAvailability nodes; ///< A pane's moves, a join and a new workspace.
    };
    // An older daemon moves nothing a menu offers: its remote path refuses every move short of MoveNode.
    for (auto const& [moves, nodes]: { Case { MenuMoves::Nodes, RowAvailability::Enabled },
                                       Case { MenuMoves::Tabs, RowAvailability::Disabled },
                                       Case { MenuMoves::Nothing, RowAvailability::Disabled } })
    {
        INFO(static_cast<int>(moves));
        auto const pane = paneOn(moves);
        CHECK(row(pane, "Move to Tab").availability == nodes);
        CHECK(row(pane, "Move to New Tab").availability == nodes);
        CHECK(row(pane, "Move to New Workspace").availability == nodes);
        auto const tab = tabOn(moves);
        CHECK(row(tab, "Move to Workspace").availability == nodes);
        CHECK(row(tab, "Merge into Tab").availability == nodes);
        CHECK(row(tab, "Move to New Workspace").availability == nodes);
        CHECK(row(workspaceOn(moves), "Merge into Tab").availability == nodes);
        // What moves nothing stays: Go to Pane, Mark Seen, the closes.
        CHECK(row(pane, "Go to Pane").availability == RowAvailability::Enabled);
        CHECK(row(pane, "Close Pane…").availability == RowAvailability::Enabled);
    }
}

namespace
{
/// @return How often @p entries hold a row titled @p title.
[[nodiscard]] std::ptrdiff_t countTitled(std::vector<ContextMenuEntry> const& entries, std::string_view title)
{
    return std::ranges::count(entries, title, &ContextMenuEntry::title);
}

/// @return @p entries' titles, the separators left out.
[[nodiscard]] std::vector<std::string> commandTitles(std::vector<ContextMenuEntry> const& entries)
{
    auto result = std::vector<std::string> {};
    for (auto const& entry: entries)
        if (entry.kind != ContextMenuEntryKind::Separator)
            result.push_back(entry.title);
    return result;
}
} // namespace

TEST_CASE("every sidebar row menu offers Open in New Window", "[sidebarmenu]")
{
    auto const menus = std::array {
        buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2 }),
        buildSidebarTabMenu(
            SidebarTabMenuState { .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 } }),
        buildSidebarPaneMenu(SidebarPaneMenuState {}),
        buildSidebarDaemonMenu(
            SidebarDaemonMenuState { .verbs = { DaemonRowVerb::Connect, DaemonRowVerb::None } }),
    };
    for (auto const& menu: menus)
    {
        CHECK(countTitled(menu, "Open in New Window") == 1);
        auto const& open = row(menu, "Open in New Window");
        CHECK(std::holds_alternative<actions::OpenInNewWindow>(open.action));
        CHECK(open.availability == RowAvailability::Enabled);
    }
}

TEST_CASE("a link daemon's menu offers no Open in New Window", "[sidebarmenu]")
{
    auto const menus = std::array {
        buildWorkspaceMenu(WorkspaceMenuState { .position = 0, .count = 2, .open = OpenOffer::NotOffered }),
        buildSidebarTabMenu(SidebarTabMenuState { .tab = TabContextMenuState { .tabIndex = 0, .tabCount = 2 },
                                                  .open = OpenOffer::NotOffered }),
        buildSidebarPaneMenu(SidebarPaneMenuState { .open = OpenOffer::NotOffered }),
        buildSidebarDaemonMenu(SidebarDaemonMenuState {
            .verbs = { DaemonRowVerb::Connect, DaemonRowVerb::None }, .open = OpenOffer::NotOffered }),
    };
    for (auto const& menu: menus)
    {
        CHECK(countTitled(menu, "Open in New Window") == 0);
        // Nor a separator left where it stood.
        REQUIRE_FALSE(menu.empty());
        CHECK(menu.front().kind != ContextMenuEntryKind::Separator);
    }
}

TEST_CASE("a daemon row no window is on offers Hide from Sidebar, set apart with Remove", "[sidebarmenu]")
{
    auto const menu = buildSidebarDaemonMenu(SidebarDaemonMenuState {
        .verbs = { DaemonRowVerb::Connect, DaemonRowVerb::Remove }, .hide = HideOffer::Offered });
    CHECK(titles(menu)
          == std::vector<std::string> { "Open in New Window",
                                        "New Workspace",
                                        "",
                                        "Connect",
                                        "",
                                        "Hide from Sidebar",
                                        "Remove from Sidebar" });
    CHECK(std::holds_alternative<actions::HideDaemon>(row(menu, "Hide from Sidebar").action));
}

TEST_CASE("a daemon a window is on offers no Hide", "[sidebarmenu]")
{
    auto const menu = buildSidebarDaemonMenu(SidebarDaemonMenuState {
        .verbs = { DaemonRowVerb::None, DaemonRowVerb::None }, .hide = HideOffer::NotOffered });
    CHECK(countTitled(menu, "Hide from Sidebar") == 0);
    // Nor a separator left where it stood.
    REQUIRE_FALSE(menu.empty());
    CHECK(menu.back().kind != ContextMenuEntryKind::Separator);
}

TEST_CASE("the hidden row's menu shows each hidden daemon in order", "[sidebarmenu]")
{
    auto const names = std::vector<std::string> { "work", "starfive" };
    auto const menu = buildSidebarHiddenMenu(names);
    CHECK(titles(menu) == std::vector<std::string> { "Show work", "Show starfive" });
    REQUIRE(std::holds_alternative<actions::ShowHiddenDaemon>(row(menu, "Show starfive").action));
    CHECK(std::get<actions::ShowHiddenDaemon>(row(menu, "Show work").action).index == 1);
    CHECK(std::get<actions::ShowHiddenDaemon>(row(menu, "Show starfive").action).index == 2);
}

TEST_CASE("a daemon row's menu offers what its buttons do, in order", "[sidebarmenu]")
{
    SECTION("not running: Start, and Remove set apart")
    {
        auto const menu = buildSidebarDaemonMenu(
            SidebarDaemonMenuState { .verbs = { DaemonRowVerb::Start, DaemonRowVerb::Remove } });
        CHECK(titles(menu)
              == std::vector<std::string> {
                  "Open in New Window", "New Workspace", "", "Start Daemon", "", "Remove from Sidebar" });
        CHECK(std::holds_alternative<actions::StartDaemon>(row(menu, "Start Daemon").action));
        CHECK(std::holds_alternative<actions::RemoveDaemon>(row(menu, "Remove from Sidebar").action));
    }
    SECTION("connecting: Cancel Connecting")
    {
        auto const menu = buildSidebarDaemonMenu(
            SidebarDaemonMenuState { .verbs = { DaemonRowVerb::Cancel, DaemonRowVerb::None } });
        CHECK(commandTitles(menu)
              == std::vector<std::string> { "Open in New Window", "New Workspace", "Cancel Connecting" });
        CHECK(std::holds_alternative<actions::CancelSwitch>(row(menu, "Cancel Connecting").action));
    }
    SECTION("a jump into it pending: Cancel Jump")
    {
        auto const menu = buildSidebarDaemonMenu(SidebarDaemonMenuState {
            .verbs = { DaemonRowVerb::Cancel, DaemonRowVerb::None }, .cancels = DaemonCancelTarget::Jump });
        CHECK(commandTitles(menu)
              == std::vector<std::string> { "Open in New Window", "New Workspace", "Cancel Jump" });
        CHECK(std::holds_alternative<actions::CancelSwitch>(row(menu, "Cancel Jump").action));
    }
    SECTION("not connected: Connect, and Remove")
    {
        auto const menu = buildSidebarDaemonMenu(
            SidebarDaemonMenuState { .verbs = { DaemonRowVerb::Connect, DaemonRowVerb::Remove } });
        CHECK(commandTitles(menu)
              == std::vector<std::string> {
                  "Open in New Window", "New Workspace", "Connect", "Remove from Sidebar" });
        CHECK(std::holds_alternative<actions::ConnectDaemon>(row(menu, "Connect").action));
    }
    SECTION("no buttons: Open in New Window alone")
    {
        auto const menu = buildSidebarDaemonMenu(SidebarDaemonMenuState {});
        CHECK(titles(menu) == std::vector<std::string> { "Open in New Window", "New Workspace" });
    }
}

// {{{ Other daemons' rows
namespace
{
/// The workspace, tab and pane menus over another daemon's rows, with everything an own row would offer.
[[nodiscard]] WorkspaceMenuState otherWorkspace()
{
    return WorkspaceMenuState { .position = 1,
                                .count = 3,
                                .joinable = joinable(),
                                .palette = { vtbackend::RGBColor { 0xC0, 0x39, 0x2B } },
                                .origin = RowOrigin::OtherDaemon };
}

[[nodiscard]] SidebarTabMenuState otherTab()
{
    return SidebarTabMenuState { .tab =
                                     TabContextMenuState { .tabIndex = 1,
                                                           .tabCount = 3,
                                                           .palette = { vtbackend::RGBColor { 1, 2, 3 } } },
                                 .others = { WorkspaceChoice { .position = 2, .title = "api" } },
                                 .joinable = joinable(),
                                 .workspaceTabs = 3,
                                 .origin = RowOrigin::OtherDaemon };
}

[[nodiscard]] SidebarPaneMenuState otherPane()
{
    return SidebarPaneMenuState {
        .joinable = joinable(), .tabPanes = 2, .workspaceTabs = 2, .origin = RowOrigin::OtherDaemon
    };
}

/// Every command row of @p entries, submenus' included.
void collectActions(std::vector<ContextMenuEntry> const& entries, std::vector<actions::Action>& into)
{
    for (auto const& entry: entries)
    {
        if (entry.kind == ContextMenuEntryKind::Command)
            into.push_back(entry.action);
        collectActions(entry.children, into);
    }
}
} // namespace

TEST_CASE("another daemon's workspace menu offers Open, Color, Rename, Mark Seen, New Tab Here and Close",
          "[sidebarmenu]")
{
    auto const menu = buildWorkspaceMenu(otherWorkspace());
    CHECK(titles(menu)
          == std::vector<std::string> { "Open in New Window",
                                        "",
                                        "Workspace Color",
                                        "Rename…",
                                        "Mark Seen",
                                        "New Tab Here",
                                        "",
                                        "Close Workspace…" });
    // Its colour is kept there: the submenu is open to it.
    CHECK(row(menu, "Workspace Color").availability == RowAvailability::Enabled);
}

TEST_CASE("another daemon's tab menu offers Open, Color, Rename, the splits and Close Tab, last",
          "[sidebarmenu]")
{
    // Close Tab… last, as the workspace and pane menus end with their close (Phase 6 review, M5).
    auto const menu = buildSidebarTabMenu(otherTab());
    CHECK(titles(menu)
          == std::vector<std::string> { "Open in New Window",
                                        "",
                                        "Tab Color",
                                        "Rename…",
                                        "",
                                        "Split Vertically",
                                        "Split Horizontally",
                                        "",
                                        "Close Tab…" });
}

// Review (Phase 6, M4): a last-known row's verbs that go through its daemon's section find nothing to carry
// them; they are shown disabled, and the jumps stay.
TEST_CASE("another daemon's last-known rows disable what goes through its section", "[sidebarmenu]")
{
    auto const enabledOf = [](std::vector<ContextMenuEntry> const& menu, std::string_view title) {
        return row(menu, title).availability == RowAvailability::Enabled;
    };
    auto workspace = otherWorkspace();
    workspace.freshness = RowFreshness::LastKnown;
    auto const workspaceMenu = buildWorkspaceMenu(workspace);
    CHECK(enabledOf(workspaceMenu, "Open in New Window"));
    CHECK(enabledOf(workspaceMenu, "New Tab Here"));
    for (auto const title: { "Workspace Color", "Rename…", "Mark Seen", "Close Workspace…" })
    {
        INFO(title);
        CHECK_FALSE(enabledOf(workspaceMenu, title));
    }

    auto tab = otherTab();
    tab.freshness = RowFreshness::LastKnown;
    auto const tabMenu = buildSidebarTabMenu(tab);
    CHECK(enabledOf(tabMenu, "Split Vertically"));
    for (auto const title: { "Tab Color", "Rename…", "Close Tab…" })
    {
        INFO(title);
        CHECK_FALSE(enabledOf(tabMenu, title));
    }

    auto pane = otherPane();
    pane.freshness = RowFreshness::LastKnown;
    auto const paneMenu = buildSidebarPaneMenu(pane);
    CHECK(enabledOf(paneMenu, "Go to Pane"));
    CHECK_FALSE(enabledOf(paneMenu, "Mark Seen"));
    CHECK_FALSE(enabledOf(paneMenu, "Close Pane…"));

    // A live row of the same daemon keeps them, and own rows know no freshness.
    CHECK(enabledOf(buildWorkspaceMenu(otherWorkspace()), "Rename…"));
    auto own = otherWorkspace();
    own.origin = RowOrigin::Own;
    own.freshness = RowFreshness::LastKnown;
    CHECK(enabledOf(buildWorkspaceMenu(own), "Rename…"));
}

TEST_CASE("another daemon's pane menu offers Go to Pane, Open, Mark Seen, the splits and Close Pane",
          "[sidebarmenu]")
{
    auto const menu = buildSidebarPaneMenu(otherPane());
    CHECK(titles(menu)
          == std::vector<std::string> { "Go to Pane",
                                        "Open in New Window",
                                        "",
                                        "Mark Seen",
                                        "Split Vertically",
                                        "Split Horizontally",
                                        "",
                                        "Close Pane…" });
}

TEST_CASE("every action routes once for another daemon's rows", "[sidebarmenu]")
{
    // The table names each action once.
    auto const routes = otherDaemonRoutes();
    for (auto const [index, entry]: std::views::zip(std::views::iota(size_t { 0 }), routes))
    {
        CAPTURE(index);
        CHECK(std::ranges::count_if(routes,
                                    [&](OtherDaemonRouteRow const& other) {
                                        return other.action.index() == entry.action.index();
                                    })
              == 1);
        CHECK(otherDaemonRouteOf(entry.action) == entry.route);
    }
    // Every action another daemon's menus offer has a way to run there.
    auto offered = std::vector<actions::Action> {};
    collectActions(buildWorkspaceMenu(otherWorkspace()), offered);
    collectActions(buildSidebarTabMenu(otherTab()), offered);
    collectActions(buildSidebarPaneMenu(otherPane()), offered);
    REQUIRE_FALSE(offered.empty());
    for (auto const& action: offered)
    {
        CAPTURE(action.index());
        CHECK(otherDaemonRouteOf(action) != OtherDaemonRoute::NotOffered);
    }
    // And every action an own row's menus offer is named by the table, offered there or not.
    auto own = std::vector<actions::Action> {};
    auto ownWorkspace = otherWorkspace();
    ownWorkspace.origin = RowOrigin::Own;
    auto ownTab = otherTab();
    ownTab.origin = RowOrigin::Own;
    auto ownPane = otherPane();
    ownPane.origin = RowOrigin::Own;
    collectActions(buildWorkspaceMenu(ownWorkspace), own);
    collectActions(buildSidebarTabMenu(ownTab), own);
    collectActions(buildSidebarPaneMenu(ownPane), own);
    for (auto const& action: own)
    {
        CAPTURE(action.index());
        CHECK(std::ranges::any_of(routes, [&](OtherDaemonRouteRow const& entry) {
            return entry.action.index() == action.index();
        }));
    }
}

TEST_CASE("own rows' menus are unchanged", "[sidebarmenu]")
{
    auto workspace = otherWorkspace();
    workspace.origin = RowOrigin::Own;
    CHECK(titles(buildWorkspaceMenu(workspace))
          == std::vector<std::string> { "Open in New Window",
                                        "",
                                        "Workspace Color",
                                        "Rename…",
                                        "Mark Seen",
                                        "New Tab Here",
                                        "",
                                        "Move Up",
                                        "Move Down",
                                        "Merge into Tab",
                                        "",
                                        "Close Workspace…" });
    auto tab = otherTab();
    tab.origin = RowOrigin::Own;
    CHECK(titles(buildSidebarTabMenu(tab))
          == std::vector<std::string> { "Open in New Window",
                                        "",
                                        "Tab Color",
                                        "Rename…",
                                        "Move Tab to New Window",
                                        "",
                                        "Close Tab…",
                                        "Close Other Tabs",
                                        "Close Tabs to the Right",
                                        "",
                                        "Split Vertically",
                                        "Split Horizontally",
                                        "",
                                        "Move to Workspace",
                                        "Merge into Tab",
                                        "Move to New Workspace" });
    auto pane = otherPane();
    pane.origin = RowOrigin::Own;
    CHECK(titles(buildSidebarPaneMenu(pane))
          == std::vector<std::string> { "Go to Pane",
                                        "Open in New Window",
                                        "",
                                        "Mark Seen",
                                        "Split Vertically",
                                        "Split Horizontally",
                                        "",
                                        "Move to Tab",
                                        "Move to New Tab",
                                        "Move to New Workspace",
                                        "",
                                        "Close Pane…" });
}

TEST_CASE("a daemon row's menu offers New Workspace above its buttons", "[sidebarmenu]")
{
    auto const menu = buildSidebarDaemonMenu(
        SidebarDaemonMenuState { .verbs = { DaemonRowVerb::Connect, DaemonRowVerb::None } });
    CHECK(titles(menu) == std::vector<std::string> { "Open in New Window", "New Workspace", "", "Connect" });
    CHECK(std::holds_alternative<actions::NewWorkspace>(row(menu, "New Workspace").action));
}

TEST_CASE("a link daemon of another window offers no New Workspace", "[sidebarmenu]")
{
    auto const menu =
        buildSidebarDaemonMenu(SidebarDaemonMenuState { .verbs = {},
                                                        .open = OpenOffer::NotOffered,
                                                        .cancels = DaemonCancelTarget::Connect,
                                                        .newWorkspace = NewWorkspaceOffer::NotOffered });
    CHECK(countTitled(menu, "New Workspace") == 0);
    CHECK(menu.empty());
}
// }}}
