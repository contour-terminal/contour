// SPDX-License-Identifier: Apache-2.0
//
// The keys each chrome area answers, the clamped movement of focus, and when closing a tab asks first.

#include <contour/window/ChromeKeys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

using namespace contour::window;
using enum ChromeKeyModifier;

TEST_CASE("every chrome key row is unique", "[chromekeys]")
{
    for (auto const& row: ChromeKeyRows)
        CHECK(std::ranges::count_if(ChromeKeyRows,
                                    [&](auto const& other) {
                                        return other.area == row.area && other.key == row.key
                                               && other.modifier == row.modifier;
                                    })
              == 1);
}

TEST_CASE("the tab well answers its keys", "[chromekeys]")
{
    constexpr auto Tabs = ChromeArea::Tabs;
    CHECK(chromeCommandFor(Tabs, ChromeKey::Left, None) == ChromeCommand::FocusPrevious);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Right, None) == ChromeCommand::FocusNext);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Right, Alt) == ChromeCommand::MoveRight);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Left, Alt) == ChromeCommand::MoveLeft);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Backspace, None) == ChromeCommand::Close);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Delete, None) == ChromeCommand::Close);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Return, None) == ChromeCommand::Activate);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Space, None) == ChromeCommand::Activate);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Escape, None) == ChromeCommand::Leave);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Return, Alt) == ChromeCommand::None);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Left, Other) == ChromeCommand::None);
    CHECK(chromeCommandFor(Tabs, ChromeKey::Other, None) == ChromeCommand::None);
}

TEST_CASE("the toolbar cannot close or reorder", "[chromekeys]")
{
    constexpr auto Toolbar = ChromeArea::Toolbar;
    CHECK(chromeCommandFor(Toolbar, ChromeKey::Backspace, None) == ChromeCommand::None);
    CHECK(chromeCommandFor(Toolbar, ChromeKey::Right, Alt) == ChromeCommand::None);
    CHECK(chromeCommandFor(Toolbar, ChromeKey::Return, None) == ChromeCommand::Activate);
    CHECK(chromeCommandFor(Toolbar, ChromeKey::Escape, None) == ChromeCommand::Leave);
}

TEST_CASE("the terminal answers no chrome key", "[chromekeys]")
{
    for (auto const key: { ChromeKey::Left, ChromeKey::Return, ChromeKey::Backspace, ChromeKey::Escape })
        for (auto const modifier: { None, Alt, Other })
            CHECK(chromeCommandFor(ChromeArea::Terminal, key, modifier) == ChromeCommand::None);
}

TEST_CASE("focusedIndexAfter clamps and never wraps", "[chromekeys]")
{
    CHECK(focusedIndexAfter(ChromeCommand::FocusPrevious, 0, 3) == 0);
    CHECK(focusedIndexAfter(ChromeCommand::FocusPrevious, 2, 3) == 1);
    CHECK(focusedIndexAfter(ChromeCommand::FocusNext, 2, 3) == 2);
    CHECK(focusedIndexAfter(ChromeCommand::FocusNext, 0, 3) == 1);
    CHECK(focusedIndexAfter(ChromeCommand::FocusLast, 0, 3) == 2);
    CHECK(focusedIndexAfter(ChromeCommand::FocusFirst, 2, 3) == 0);
    CHECK(focusedIndexAfter(ChromeCommand::Activate, 1, 3) == 1);
    CHECK(focusedIndexAfter(ChromeCommand::FocusNext, 0, 0) == 0);
}

TEST_CASE("paneActivityOf compares the foreground to the shell", "[chromekeys]")
{
    CHECK(paneActivityOf("/bin/zsh", std::string { "zsh" }) == PaneActivity::Idle);
    CHECK(paneActivityOf("/bin/zsh", std::string { "-zsh" }) == PaneActivity::Idle);
    CHECK(paneActivityOf("/bin/zsh", std::string { "make" }) == PaneActivity::Running);
    CHECK(paneActivityOf("/bin/zsh", std::nullopt) == PaneActivity::Unknown);
}

TEST_CASE("closeConfirmationFor asks only about a known running program", "[chromekeys]")
{
    using enum PaneActivity;
    CHECK(closeConfirmationFor(std::array { Idle, Running }) == CloseConfirmation::Ask);
    CHECK(closeConfirmationFor(std::array { Idle, Unknown }) == CloseConfirmation::None);
    CHECK(closeConfirmationFor(std::array<PaneActivity, 0> {}) == CloseConfirmation::None);
}
