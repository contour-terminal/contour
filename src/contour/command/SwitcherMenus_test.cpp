// SPDX-License-Identifier: Apache-2.0
//
// The switcher's row menu is a pure function of a snapshot: its two leading rows, and the sidebar's workspace
// rows after them for the window's own daemon, asserted with no window and no Qt.

#include <contour/command/SwitcherMenus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>
#include <vector>

using namespace contour::command;

namespace
{
[[nodiscard]] std::vector<std::string> titlesOf(std::vector<SwitcherMenuRow> const& rows)
{
    auto titles = std::vector<std::string> {};
    for (auto const& row: rows)
        titles.push_back(std::visit([](auto const& each) { return each.title; }, row));
    return titles;
}

[[nodiscard]] std::string enterText(ShortcutStyle style, vtbackend::Modifiers modifiers = {})
{
    return shortcutText(ShortcutChord { .modifiers = modifiers, .input = vtbackend::Key::Enter }, style);
}
} // namespace

TEST_CASE("the switcher's row menu leads with switch and open, then the sidebar's workspace rows",
          "[switcher][sidebarmenu]")
{
    auto const state = SwitcherRowMenuState { .name = "docs",
                                              .owner = SwitcherMenuOwner::WindowDaemon,
                                              .workspace = WorkspaceMenuState { .position = 1, .count = 3 } };
    for (auto const style: { ShortcutStyle::MacGlyphs, ShortcutStyle::Text })
    {
        auto const menu = buildSwitcherRowMenu(state, style);
        auto expected = std::vector<std::string> { "Switch to docs", "Open in New Window", "" };
        for (auto const& entry: buildWorkspaceMenu(state.workspace))
            expected.push_back(entry.title);
        CHECK(titlesOf(menu) == expected);

        REQUIRE(menu.size() > 3);
        CHECK(std::get<SwitcherChoiceRow>(menu[0])
              == SwitcherChoiceRow { .choice = SwitcherMenuChoice::Switch,
                                     .title = "Switch to docs",
                                     .shortcut = enterText(style),
                                     .availability = RowAvailability::Enabled });
        CHECK(std::get<SwitcherChoiceRow>(menu[1])
              == SwitcherChoiceRow { .choice = SwitcherMenuChoice::OpenInNewWindow,
                                     .title = "Open in New Window",
                                     .shortcut = enterText(style, vtbackend::Modifier::Shift),
                                     .availability = RowAvailability::Enabled });
        // The rest are the sidebar's, and only they carry an action.
        REQUIRE(std::holds_alternative<ContextMenuEntry>(menu[2]));
        CHECK(std::get<ContextMenuEntry>(menu[2]).kind == ContextMenuEntryKind::Separator);
        CHECK(std::holds_alternative<ContextMenuEntry>(menu[3]));
    }
}

TEST_CASE("another daemon's row offers switch and open only", "[switcher][sidebarmenu]")
{
    auto const menu = buildSwitcherRowMenu(
        SwitcherRowMenuState { .name = "api-docs", .owner = SwitcherMenuOwner::OtherDaemon, .workspace = {} },
        ShortcutStyle::MacGlyphs);
    CHECK(titlesOf(menu) == std::vector<std::string> { "Switch to api-docs", "Open in New Window" });
}

TEST_CASE("the workspace this window shows offers no switch to it", "[switcher][sidebarmenu]")
{
    auto const menu = buildSwitcherRowMenu(SwitcherRowMenuState { .name = "contour",
                                                                  .owner = SwitcherMenuOwner::OtherDaemon,
                                                                  .workspace = {},
                                                                  .returnMode = SwitcherMenuReturn::Switch,
                                                                  .shown = SwitcherMenuShown::ThisWindow },
                                           ShortcutStyle::MacGlyphs);
    REQUIRE(menu.size() == 2);
    CHECK(std::get<SwitcherChoiceRow>(menu[0]).availability == RowAvailability::Disabled);
    CHECK(std::get<SwitcherChoiceRow>(menu[1]).availability == RowAvailability::Enabled);
}

TEST_CASE("where Return opens a new window, the menu leads with that alone", "[switcher][sidebarmenu]")
{
    auto const menu = buildSwitcherRowMenu(SwitcherRowMenuState { .name = "docs",
                                                                  .owner = SwitcherMenuOwner::OtherDaemon,
                                                                  .workspace = {},
                                                                  .returnMode = SwitcherMenuReturn::Open,
                                                                  .shown = SwitcherMenuShown::Elsewhere },
                                           ShortcutStyle::MacGlyphs);
    REQUIRE(menu.size() == 1);
    CHECK(std::get<SwitcherChoiceRow>(menu[0])
          == SwitcherChoiceRow { .choice = SwitcherMenuChoice::OpenInNewWindow,
                                 .title = "Open in New Window",
                                 .shortcut = enterText(ShortcutStyle::MacGlyphs),
                                 .availability = RowAvailability::Enabled });
}

TEST_CASE("a window without a daemon is offered no Open in New Window", "[switcher][sidebarmenu]")
{
    auto const menu =
        buildSwitcherRowMenu(SwitcherRowMenuState { .name = "docs",
                                                    .owner = SwitcherMenuOwner::OtherDaemon,
                                                    .workspace = {},
                                                    .newWindow = SwitcherMenuNewWindow::Withheld },
                             ShortcutStyle::MacGlyphs);
    CHECK(titlesOf(menu) == std::vector<std::string> { "Switch to docs" });
}
