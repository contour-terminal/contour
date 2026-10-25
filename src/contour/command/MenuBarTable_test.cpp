// SPDX-License-Identifier: Apache-2.0
//
// The menu bar is data a function resolves against a POD snapshot, so every row, rule and chord is
// asserted with no window, no terminal and no Qt behind it.

#include <contour/command/Command.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/command/TabMenus.hpp>
#include <contour/command/TitleBarContextMenu.hpp>
#include <contour/config/Actions.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{

/// The entry titled @p title, or nullptr. Recurses into submenus.
[[nodiscard]] ContextMenuEntry const* find(std::vector<ContextMenuEntry> const& entries,
                                           std::string_view title)
{
    for (auto const& entry: entries)
    {
        if (entry.title == title)
            return &entry;
        if (auto const* nested = find(entry.children, title); nested != nullptr)
            return nested;
    }
    return nullptr;
}

/// The nodes titled @p title, across the whole tree.
void collectNodes(std::vector<MenuNode> const& nodes,
                  std::string_view title,
                  std::vector<MenuNode const*>& out)
{
    for (auto const& node: nodes)
    {
        if (node.title == title)
            out.push_back(&node);
        collectNodes(node.children, title, out);
    }
}

[[nodiscard]] MenuNode const* findNode(std::vector<MenuNode> const& nodes, std::string_view title)
{
    auto found = std::vector<MenuNode const*> {};
    collectNodes(nodes, title, found);
    return found.empty() ? nullptr : found.front();
}

/// Every chord shown anywhere in the tree.
void collectChords(std::vector<MenuNode> const& nodes, std::vector<ShortcutChord>& out)
{
    for (auto const& node: nodes)
    {
        if (node.chord)
            out.push_back(*node.chord);
        collectChords(node.children, out);
    }
}

/// The catalogue names of every command row, submenus included.
void collectActionNames(std::vector<ContextMenuEntry> const& entries, std::set<std::string>& out)
{
    for (auto const& entry: entries)
    {
        if (entry.kind == ContextMenuEntryKind::Command)
            out.emplace(actions::name(entry.action));
        collectActionNames(entry.children, out);
    }
}

[[nodiscard]] MenuBarState fullState()
{
    return MenuBarState { .window = WindowPresence::Present,
                          .selection = SelectionPresence::Present,
                          .speech = SpeechAvailability::Available,
                          .tabCount = 3,
                          .activeTab = 1,
                          .paneCount = 2,
                          .tabTitles = { "one", "two", "three" },
                          .activeProfile = "main",
                          .profileNames = { "main" },
                          .hostNames = { "server" },
                          .layoutNames = { "dev" } };
}

[[nodiscard]] std::set<std::string> reachableActionNames()
{
    auto names = std::set<std::string> {};
    collectActionNames(buildMenuBar(fullState()), names);

    collectActionNames(buildContextMenu(ContextMenuState {
                           .hasSelection = true,
                           .clipboardHasText = true,
                           .hasLastCommand = true,
                           .hasLocalWorkingDirectory = true,
                           .hasSplits = true,
                           .canSpeak = true,
                           .hyperlinkUnderCursor = "https://contour-terminal.org/",
                           .foldLine = vtbackend::LineOffset(3),
                           .blockUnderCursor = PointedBlock { .id = vtbackend::CommandBlockId(5),
                                                              .output = OutputPresence::Present,
                                                              .commandLine = CommandLinePresence::Present },
                           .livePrompt = PromptPresence::Present,
                           .lastCommandLine = CommandLinePresence::Present,
                           .activeProfile = "main",
                           .profileNames = { "main" },
                           .hostNames = { "server" } }),
                       names);
    collectActionNames(
        buildTitleBarContextMenu(TitleBarContextMenuState {
            .tabCount = 2, .activeProfile = "main", .profileNames = { "main" }, .hostNames = { "server" } }),
        names);
    collectActionNames(buildTabContextMenu(TabContextMenuState { .tabIndex = 0,
                                                                 .tabCount = 2,
                                                                 .palette = { vtbackend::RGBColor {} },
                                                                 .color = vtbackend::RGBColor {},
                                                                 .userColor = UserColorPresence::Present }),
                       names);
    collectActionNames(buildNewTabMenu(NewTabMenuState { .profileNames = { "main" } }), names);
    return names;
}

} // namespace

TEST_CASE("menuBar.sixMenusInOrder", "[menubar]")
{
    constexpr auto titles = std::array<std::string_view, MenuBarMenuCount> { "Contour", "Shell",  "Edit",
                                                                             "View",    "Window", "Help" };

    for (auto const& entries: { buildMenuBar(fullState()), buildMenuBar(MenuBarState {}) })
    {
        REQUIRE(entries.size() == MenuBarMenuCount);
        for (auto const index: std::views::iota(std::size_t { 0 }, MenuBarMenuCount))
        {
            CHECK(entries[index].kind == ContextMenuEntryKind::Submenu);
            CHECK(entries[index].title == titles[index]);
        }
    }
}

TEST_CASE("menuBar.everyCatalogueActionIsReachable", "[menubar]")
{
    auto const reachable = reachableActionNames();

    for (auto const& entry: actions::actionCatalog())
    {
        auto const exempt = std::ranges::any_of(MenuReachabilityExemptions,
                                                [&](auto const& e) { return e.action == entry.name; });
        INFO("action " << entry.name);
        CHECK((reachable.contains(std::string(entry.name)) || exempt));
    }

    CHECK(reachable.size() + MenuReachabilityExemptions.size() == actions::actionCatalog().size());
}

TEST_CASE("menuBar.exemptActionsAppearNowhere", "[menubar]")
{
    auto const reachable = reachableActionNames();
    for (auto const& exemption: MenuReachabilityExemptions)
    {
        INFO("exempt " << exemption.action << ": " << exemption.reason);
        CHECK_FALSE(reachable.contains(std::string(exemption.action)));
    }
}

TEST_CASE("menuBar.eachRoleOnce", "[menubar]")
{
    auto const bar = buildMenuBar(fullState());
    auto const tree = buildMenuTree(bar, ChordIndex {}, ShortcutStyle::Text);

    auto const countRole =
        [](this auto const& self, std::vector<MenuNode> const& nodes, MenuItemRole role) -> std::size_t {
        auto count = std::size_t { 0 };
        for (auto const& node: nodes)
            count += (node.role == role ? 1 : 0) + self(node.children, role);
        return count;
    };

    for (auto const role: { MenuItemRole::About, MenuItemRole::Preferences, MenuItemRole::Quit })
    {
        CHECK(countRole(tree.nodes, role) == 1);
        CHECK(countRole(tree.nodes.front().children, role) == 1);
    }
    CHECK(countRole(tree.nodes, MenuItemRole::Minimize) == 1);
    CHECK(
        countRole(tree.nodes[static_cast<std::size_t>(MenuBarMenu::Window)].children, MenuItemRole::Minimize)
        == 1);
}

TEST_CASE("menuBar.noChordTwice", "[menubar]")
{
    auto const check = [](config::Config const& config) {
        auto const tree =
            buildMenuTree(buildMenuBar(fullState()), chordIndex(config), ShortcutStyle::MacGlyphs);
        auto chords = std::vector<ShortcutChord> {};
        collectChords(tree.nodes, chords);
        for (auto const index: std::views::iota(std::size_t { 0 }, chords.size()))
            for (auto const other: std::views::iota(index + 1, chords.size()))
                CHECK_FALSE(chords[index] == chords[other]);
        return tree;
    };

    auto const defaults = check(config::Config {});
    check(config::Config { .defaultsEdition = config::DefaultsEdition::Classic });

    auto const* copy = findNode(defaults.nodes, "Copy");
    REQUIRE(copy != nullptr);
    CHECK(copy->shortcut == "⌃C");
    auto const* clear = findNode(defaults.nodes, "Clear Selection");
    REQUIRE(clear != nullptr);
    CHECK(clear->shortcut == "⎋");

    // Two bindings of one chord under different modes: the same chord, two commands.
    auto const crafted = contour::test::loadConfigFromYaml(contour::test::profileConfigYaml(
        "main",
        "input_mapping:\n"
        "  - { mods: [Control, Shift], key: 'N', action: CreateNewTab }\n"
        "  - { mods: [Control, Shift], key: 'N', action: NewTerminal, mode: 'Insert' }\n"));
    auto const tree = buildMenuTree(buildMenuBar(fullState()), chordIndex(crafted), ShortcutStyle::MacGlyphs);
    auto const* newWindow = findNode(tree.nodes, "New Window");
    auto const* newTab = findNode(tree.nodes, "New Tab");
    REQUIRE(newWindow != nullptr);
    REQUIRE(newTab != nullptr);
    CHECK(newWindow->chord.has_value());
    CHECK_FALSE(newTab->chord.has_value());
}

TEST_CASE("menuBar.tabBarMirrorsTheTitleBar", "[menubar]")
{
    auto state = fullState();
    state.tabBarVisibility = config::TabBarVisibility::Always;
    state.tabBarPosition = config::TabBarPosition::Bottom;

    auto const bar = buildMenuBar(state);
    auto const titleBar = buildTitleBarContextMenu(TitleBarContextMenuState {
        .tabCount = 3, .tabBarVisibility = state.tabBarVisibility, .tabBarPosition = state.tabBarPosition });

    auto const* view = find(bar, "View");
    REQUIRE(view != nullptr);
    auto const* mine = find(view->children, "Tab Bar");
    auto const* theirs = find(titleBar, "Tab Bar");
    REQUIRE(mine != nullptr);
    REQUIRE(theirs != nullptr);

    auto const same =
        [](this auto const& self, ContextMenuEntry const& a, ContextMenuEntry const& b) -> bool {
        if (a.kind != b.kind || a.title != b.title || a.check != b.check
            || a.children.size() != b.children.size())
            return false;
        if (a.kind == ContextMenuEntryKind::Command && commandId(a.action) != commandId(b.action))
            return false;
        return std::ranges::equal(a.children, b.children, self);
    };
    CHECK(same(*mine, *theirs));
}

TEST_CASE("menuBar.windowRowsNeedAWindow", "[menubar]")
{
    auto const bare = buildMenuBar(MenuBarState {});
    for (auto const title: { "Split Vertically", "Close Tab", "Rename Tab…", "Tab Bar", "Pane" })
    {
        INFO(title);
        auto const* row = find(bare, title);
        REQUIRE(row != nullptr);
        CHECK(row->availability == RowAvailability::Disabled);
    }
    for (auto const title: { "New Window", "Quit Contour" })
    {
        INFO(title);
        auto const* row = find(bare, title);
        REQUIRE(row != nullptr);
        CHECK(row->availability == RowAvailability::Enabled);
    }

    auto state = fullState();
    state.paneCount = 1;
    auto const single = buildMenuBar(state);
    CHECK(find(single, "Tab Bar")->availability == RowAvailability::Enabled);
    CHECK(find(single, "Pane")->availability == RowAvailability::Disabled);
}

TEST_CASE("menuBar.windowStartsWithMinimizeAndZoom", "[menubar]")
{
    for (auto const& state: { fullState(), MenuBarState {} })
    {
        auto const bar = buildMenuBar(state);
        auto const& window = bar[static_cast<std::size_t>(MenuBarMenu::Window)].children;
        REQUIRE(window.size() >= 3);

        CHECK(window[0].title == "Minimize");
        CHECK(std::holds_alternative<actions::MinimizeWindow>(window[0].action));
        CHECK(window[0].role == MenuItemRole::Minimize);
        CHECK(window[1].title == "Zoom");
        CHECK(std::holds_alternative<actions::ZoomWindow>(window[1].action));
        CHECK(window[1].role == MenuItemRole::None);
        CHECK(window[2].kind == ContextMenuEntryKind::Separator);

        // They act on the key window, so without one they are there, greyed out.
        auto const expected =
            state.window == WindowPresence::Present ? RowAvailability::Enabled : RowAvailability::Disabled;
        CHECK(window[0].availability == expected);
        CHECK(window[1].availability == expected);
    }
}

TEST_CASE("menuBar.tabList", "[menubar]")
{
    auto const bar = buildMenuBar(fullState());
    auto const& window = bar[static_cast<std::size_t>(MenuBarMenu::Window)].children;
    REQUIRE(window.size() >= 3);

    auto const tabs = std::span(window).last(3);
    for (auto const index: std::views::iota(std::size_t { 0 }, tabs.size()))
    {
        auto const* switchTo = std::get_if<actions::SwitchToTab>(&tabs[index].action);
        REQUIRE(switchTo != nullptr);
        CHECK(switchTo->position == static_cast<int>(index) + 1);
        CHECK(tabs[index].check == (index == 1 ? RowCheck::Checked : RowCheck::Unchecked));
    }
    CHECK(tabs[0].title == "one");
}

/// Every command row of @p entries, submenus included.
void collectCommands(std::vector<ContextMenuEntry> const& entries, std::vector<ContextMenuEntry const*>& out)
{
    for (auto const& entry: entries)
    {
        if (entry.kind == ContextMenuEntryKind::Command)
            out.push_back(&entry);
        collectCommands(entry.children, out);
    }
}

TEST_CASE("menuBar.scopes", "[menubar]")
{
    auto const bar = buildMenuBar(fullState());

    // The application menu acts on the application alone: every one of its rows runs with no window.
    auto application = std::vector<ContextMenuEntry const*> {};
    collectCommands(bar[static_cast<std::size_t>(MenuBarMenu::Application)].children, application);
    REQUIRE_FALSE(application.empty());
    for (auto const* row: application)
    {
        INFO(row->title);
        CHECK(row->scope == ActionScope::Application);
    }

    for (auto const title: { "New Window", "Contour Documentation", "Report an Issue" })
    {
        INFO(title);
        REQUIRE(find(bar, title) != nullptr);
        CHECK(find(bar, title)->scope == ActionScope::Application);
    }

    // What acts on a terminal or a window needs the window: the Window menu's tab list, and the rest.
    auto const& window = bar[static_cast<std::size_t>(MenuBarMenu::Window)].children;
    for (auto const& tab: std::span(window).last(3))
    {
        INFO(tab.title);
        REQUIRE(std::holds_alternative<actions::SwitchToTab>(tab.action));
        CHECK(tab.scope == ActionScope::Window);
    }
    for (auto const title: { "New Tab", "Close Tab", "Copy", "Paste", "Bigger", "Minimize", "Next Tab" })
    {
        INFO(title);
        REQUIRE(find(bar, title) != nullptr);
        CHECK(find(bar, title)->scope == ActionScope::Window);
    }

    // An application row needs no window, so none waits for one to be enabled: at startup, with none yet,
    // a key equivalent of a disabled row would fall through to the terminal.
    auto bareApplication = std::vector<ContextMenuEntry const*> {};
    auto const bare = buildMenuBar(MenuBarState {});
    collectCommands(bare, bareApplication);
    for (auto const* row: bareApplication)
    {
        INFO(row->title);
        if (row->scope == ActionScope::Application)
            CHECK(row->availability == RowAvailability::Enabled);
    }

    // And the scope reaches the tree every presenter draws.
    auto const tree = buildMenuTree(bar, ChordIndex {}, ShortcutStyle::Text);
    REQUIRE(findNode(tree.nodes, "Quit Contour") != nullptr);
    CHECK(findNode(tree.nodes, "Quit Contour")->scope == ActionScope::Application);
    CHECK(findNode(tree.nodes, "Copy")->scope == ActionScope::Window);
}

TEST_CASE("menuBar.placement", "[menubar]")
{
    CHECK(menuBarPlacementFor(config::HostPlatform::MacOS) == MenuBarPlacement::Global);
    CHECK(menuBarPlacementFor(config::HostPlatform::Windows) == MenuBarPlacement::None);
    CHECK(menuBarPlacementFor(config::HostPlatform::KdePlasma) == MenuBarPlacement::None);
    CHECK(menuBarPlacementFor(config::HostPlatform::Other) == MenuBarPlacement::None);
}

TEST_CASE("menuBar.stateOfConfig", "[menubar]")
{
    auto const config = contour::test::loadConfigFromYaml("profiles:\n  zeta: {}\n  alpha: {}\n");
    auto const state = menuBarStateOf(config);
    CHECK(state.window == WindowPresence::Absent);
    CHECK(std::ranges::is_sorted(state.profileNames));
    CHECK(std::ranges::find(state.profileNames, "alpha") != state.profileNames.end());
    CHECK(std::ranges::find(state.profileNames, "zeta") != state.profileNames.end());
}
