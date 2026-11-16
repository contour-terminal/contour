// SPDX-License-Identifier: Apache-2.0
#include <core/tui/PopupKeyDispatch.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

namespace
{

KeyEvent makeKey(KeyCode code, Modifier mod = Modifier::None, char32_t cp = 0)
{
    return KeyEvent { .key = code, .modifiers = mod, .codepoint = cp };
}

KeyEvent makeCharKey(char ch, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = {}, .modifiers = mod, .codepoint = static_cast<char32_t>(ch) };
}

} // namespace

TEST_CASE("classifyNavigationKey.down_arrow")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::Down)) == PopupNavAction::SelectNext);
}

TEST_CASE("classifyNavigationKey.up_arrow")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::Up)) == PopupNavAction::SelectPrev);
}

TEST_CASE("classifyNavigationKey.ctrl_j")
{
    CHECK(classifyNavigationKey(makeCharKey('j', Modifier::Ctrl)) == PopupNavAction::SelectNext);
}

TEST_CASE("classifyNavigationKey.ctrl_k")
{
    CHECK(classifyNavigationKey(makeCharKey('k', Modifier::Ctrl)) == PopupNavAction::SelectPrev);
}

TEST_CASE("classifyNavigationKey.page_down")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::PageDown)) == PopupNavAction::PageDown);
}

TEST_CASE("classifyNavigationKey.page_up")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::PageUp)) == PopupNavAction::PageUp);
}

TEST_CASE("classifyNavigationKey.home")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::Home)) == PopupNavAction::SelectFirst);
}

TEST_CASE("classifyNavigationKey.end")
{
    CHECK(classifyNavigationKey(makeKey(KeyCode::End)) == PopupNavAction::SelectLast);
}

TEST_CASE("classifyNavigationKey.unrecognized_key")
{
    CHECK(classifyNavigationKey(makeCharKey('a')) == PopupNavAction::None);
    CHECK(classifyNavigationKey(makeKey(KeyCode::Enter)) == PopupNavAction::None);
    CHECK(classifyNavigationKey(makeKey(KeyCode::Escape)) == PopupNavAction::None);
    CHECK(classifyNavigationKey(makeKey(KeyCode::Tab)) == PopupNavAction::None);
}
