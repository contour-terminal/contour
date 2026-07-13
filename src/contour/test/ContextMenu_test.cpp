// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Command.h>
#include <contour/ContextMenu.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace contour;

namespace
{

/// The entry titled @p title, or nullptr. Recurses into submenus, so a row's identity is its title
/// wherever it lives.
[[nodiscard]] ContextMenuEntry const* find(std::vector<ContextMenuEntry> const& entries,
                                           std::string const& title)
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

/// Every command id in the menu, submenus included.
[[nodiscard]] std::vector<std::string> commandIds(std::vector<ContextMenuEntry> const& entries)
{
    auto ids = std::vector<std::string> {};
    for (auto const& entry: entries)
    {
        if (entry.kind == ContextMenuEntryKind::Command)
            ids.push_back(commandId(entry.action));
        for (auto const& id: commandIds(entry.children))
            ids.push_back(id);
    }
    return ids;
}

/// A state with everything switched on, so a test can switch exactly one thing off.
[[nodiscard]] ContextMenuState fullyEnabledState()
{
    return ContextMenuState {
        .hasSelection = true,
        .clipboardHasText = true,
        .hasLastCommand = true,
        .hasHyperlinkUnderCursor = true,
        .hasWorkingDirectory = true,
        .hasSplits = true,
        .activeProfile = "dark",
        .profileNames = { "dark", "light", "work" },
    };
}

} // namespace

TEST_CASE("ContextMenu.copy.needsASelection", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.hasSelection = true;
    auto const selected = buildContextMenu(state);
    auto const* withSelection = find(selected, "Copy");
    REQUIRE(withSelection != nullptr);
    CHECK(withSelection->enabled);

    state.hasSelection = false;
    auto const unselected = buildContextMenu(state);
    auto const* withoutSelection = find(unselected, "Copy");
    // Present but grayed out -- Copy is always a meaningful thing for this terminal to offer; it just has
    // nothing to copy right now.
    REQUIRE(withoutSelection != nullptr);
    CHECK_FALSE(withoutSelection->enabled);
}

TEST_CASE("ContextMenu.paste.needsAClipboard", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.clipboardHasText = false;
    auto const empty = buildContextMenu(state);
    auto const* whenEmpty = find(empty, "Paste");
    REQUIRE(whenEmpty != nullptr);
    CHECK_FALSE(whenEmpty->enabled);

    state.clipboardHasText = true;
    auto const filled = buildContextMenu(state);
    auto const* whenFilled = find(filled, "Paste");
    REQUIRE(whenFilled != nullptr);
    CHECK(whenFilled->enabled);
}

TEST_CASE("ContextMenu.lastCommand.hiddenWithoutShellIntegration", "[contextmenu]")
{
    auto state = fullyEnabledState();

    SECTION("present when the scrollback holds a finished command")
    {
        state.hasLastCommand = true;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Copy Last Command") != nullptr);
        CHECK(find(menu, "Copy Last Command Output") != nullptr);
        CHECK(find(menu, "Copy Last Command and Output") != nullptr);
    }

    SECTION("gone -- not merely grayed out -- when it does not")
    {
        // A permanently dead row would teach the user the feature is broken, when in truth their shell
        // simply never told the terminal where one command ended and the next began.
        state.hasLastCommand = false;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Copy Last Command") == nullptr);
        CHECK(find(menu, "Copy Last Command Output") == nullptr);
        CHECK(find(menu, "Copy Last Command and Output") == nullptr);
    }
}

TEST_CASE("ContextMenu.hyperlink.onlyUnderTheCursor", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.hasHyperlinkUnderCursor = true;
    auto const hovering = buildContextMenu(state);
    CHECK(find(hovering, "Open Link") != nullptr);
    CHECK(find(hovering, "Copy Link Address") != nullptr);

    state.hasHyperlinkUnderCursor = false;
    auto const elsewhere = buildContextMenu(state);
    CHECK(find(elsewhere, "Open Link") == nullptr);
    CHECK(find(elsewhere, "Copy Link Address") == nullptr);
}

TEST_CASE("ContextMenu.openCurrentFolder.needsAWorkingDirectory", "[contextmenu]")
{
    auto state = fullyEnabledState();

    state.hasWorkingDirectory = false;
    auto const unknown = buildContextMenu(state);
    auto const* whenUnknown = find(unknown, "Open Current Folder");
    REQUIRE(whenUnknown != nullptr);
    CHECK_FALSE(whenUnknown->enabled);

    state.hasWorkingDirectory = true;
    auto const known = buildContextMenu(state);
    auto const* whenKnown = find(known, "Open Current Folder");
    REQUIRE(whenKnown != nullptr);
    CHECK(whenKnown->enabled);
}

TEST_CASE("ContextMenu.paneRows.dependOnThereBeingSplits", "[contextmenu]")
{
    auto state = fullyEnabledState();

    SECTION("a lone pane offers no zoom and no orientation flip")
    {
        state.hasSplits = false;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Toggle Pane Zoom") == nullptr);
        CHECK(find(menu, "Toggle Split Orientation") == nullptr);
        // Splitting and closing are still on offer: closing the only pane closes the tab.
        CHECK(find(menu, "Split Vertically") != nullptr);
        CHECK(find(menu, "Split Horizontally") != nullptr);
        CHECK(find(menu, "Close Pane") != nullptr);
    }

    SECTION("a split tab offers both")
    {
        state.hasSplits = true;
        auto const menu = buildContextMenu(state);
        CHECK(find(menu, "Toggle Pane Zoom") != nullptr);
        CHECK(find(menu, "Toggle Split Orientation") != nullptr);
    }
}

TEST_CASE("ContextMenu.profiles.oneRowEachWithTheActiveOneChecked", "[contextmenu]")
{
    auto state = fullyEnabledState();
    state.profileNames = { "dark", "light", "work" };
    state.activeProfile = "light";

    auto const menu = buildContextMenu(state);
    auto const* profiles = find(menu, "Change Profile");
    REQUIRE(profiles != nullptr);
    REQUIRE(profiles->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(profiles->children.size() == 3);

    for (auto const& child: profiles->children)
    {
        CHECK(child.checkable);
        CHECK(child.checked == (child.title == "light"));
        // The row carries the concrete, argument-bearing action; nothing has to re-parse its name later.
        CHECK(commandId(child.action) == "ChangeProfile:" + child.title);
    }
}

TEST_CASE("ContextMenu.profiles.submenuVanishesWhenThereAreNone", "[contextmenu]")
{
    // A submenu that opens onto nothing is worse than no submenu.
    auto state = fullyEnabledState();
    state.profileNames.clear();

    auto const menu = buildContextMenu(state);
    CHECK(find(menu, "Change Profile") == nullptr);
}

TEST_CASE("ContextMenu.advanced.holdsBothResets", "[contextmenu]")
{
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const* advanced = find(menu, "Advanced");

    REQUIRE(advanced != nullptr);
    REQUIRE(advanced->kind == ContextMenuEntryKind::Submenu);
    REQUIRE(advanced->children.size() == 2);

    CHECK(commandId(advanced->children[0].action) == "SoftReset");
    CHECK(advanced->children[0].title == "Soft Reset Terminal");
    CHECK(commandId(advanced->children[1].action) == "ClearHistoryAndReset");
    CHECK(advanced->children[1].title == "Hard Reset Terminal");
}

TEST_CASE("ContextMenu.titles.derivedFromTheActionUnlessOverridden", "[contextmenu]")
{
    auto const menu = buildContextMenu(fullyEnabledState());

    // "Select All" is not authored anywhere: it falls out of splitting the action's catalog name. That is
    // what keeps the menu from growing a second title table that can drift from the first.
    auto const* selectAll = find(menu, "Select All");
    REQUIRE(selectAll != nullptr);
    CHECK(commandId(selectAll->action) == "SelectAll");

    // ... and an override wins where the derived name would read badly ("Copy Selection", "Paste
    // Clipboard", "Create New Tab").
    CHECK(find(menu, "Copy") != nullptr);
    CHECK(find(menu, "Paste") != nullptr);
    CHECK(find(menu, "New Tab") != nullptr);
    CHECK(find(menu, "Copy Selection") == nullptr);
    CHECK(find(menu, "Paste Clipboard") == nullptr);
}

TEST_CASE("ContextMenu.everyCommandIdResolvesToARealAction", "[contextmenu]")
{
    // Pins the menu to the action catalog: rename an action and this fails loudly at test time rather
    // than quietly handing the GUI a row that does nothing.
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const ids = commandIds(menu);

    REQUIRE_FALSE(ids.empty());

    for (auto const& id: ids)
    {
        auto const name = id.substr(0, id.find(':'));
        INFO("command id: " << id);
        CHECK(actions::fromString(name).has_value());
    }
}

TEST_CASE("ContextMenu.separators.neverLeadingTrailingOrDoubled", "[contextmenu]")
{
    // Hiding rows leaves separators stranded, and every predicate added later is a fresh chance to strand
    // one. Sweep the whole state space rather than trusting the eye.
    auto const flags = { false, true };

    for (auto const selection: flags)
        for (auto const clipboard: flags)
            for (auto const lastCommand: flags)
                for (auto const hyperlink: flags)
                    for (auto const splits: flags)
                        for (auto const profiles: flags)
                        {
                            auto state = ContextMenuState {
                                .hasSelection = selection,
                                .clipboardHasText = clipboard,
                                .hasLastCommand = lastCommand,
                                .hasHyperlinkUnderCursor = hyperlink,
                                .hasWorkingDirectory = true,
                                .hasSplits = splits,
                                .activeProfile = profiles ? "dark" : "",
                                .profileNames = profiles ? std::vector<std::string> { "dark" }
                                                         : std::vector<std::string> {},
                            };

                            auto const menu = buildContextMenu(state);
                            INFO("selection=" << selection << " clipboard=" << clipboard
                                              << " lastCommand=" << lastCommand << " hyperlink=" << hyperlink
                                              << " splits=" << splits << " profiles=" << profiles);

                            REQUIRE_FALSE(menu.empty());
                            CHECK(menu.front().kind != ContextMenuEntryKind::Separator);
                            CHECK(menu.back().kind != ContextMenuEntryKind::Separator);

                            auto const doubled =
                                std::ranges::adjacent_find(menu, [](auto const& a, auto const& b) {
                                    return a.kind == ContextMenuEntryKind::Separator
                                           && b.kind == ContextMenuEntryKind::Separator;
                                });
                            CHECK(doubled == menu.end());
                        }
}
