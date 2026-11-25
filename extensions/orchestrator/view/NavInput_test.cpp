// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/DashboardKeys.hpp>
#include <orchestrator/core/NavKeys.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/NavInput.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using core::tui::KeyCode;
using core::tui::KeyEvent;
using core::tui::Modifier;

namespace
{

[[nodiscard]] KeyEvent character(char32_t codepoint, Modifier modifiers = Modifier::None)
{
    return KeyEvent { .key = static_cast<KeyCode>(codepoint),
                      .modifiers = modifiers,
                      .codepoint = codepoint };
}

[[nodiscard]] KeyEvent special(KeyCode code)
{
    return KeyEvent { .key = code, .modifiers = Modifier::None, .codepoint = 0 };
}

} // namespace

TEST_CASE("keyNameFor names a key as a binding spells it", "[orchestrator][view][navinput]")
{
    CHECK(keyNameFor(character(U'j')) == "j");
    CHECK(keyNameFor(character(U'G', Modifier::Shift)) == "G");
    CHECK(keyNameFor(character(U'd', Modifier::Ctrl)) == "^d");
    CHECK(keyNameFor(special(KeyCode::Down)) == "down");
    CHECK(keyNameFor(character(U'j', Modifier::Alt)).empty());
    // Ctrl+H is Backspace: no binding names it.
    CHECK(keyNameFor(character(U'h', Modifier::Ctrl)).empty());
}

TEST_CASE("navActionOf resolves the defaults while navigating", "[orchestrator][view][navinput]")
{
    auto const keys = NavKeys::defaults(VimKeys::On);
    auto pending = std::string {};
    constexpr auto Navigating = NavContext::Navigating;

    CHECK(navActionOf(character(U'j'), Navigating, keys, pending) == NavAction::MoveDown);
    CHECK(pending.empty());

    SECTION("g waits for the second g")
    {
        CHECK(navActionOf(character(U'g'), Navigating, keys, pending) == std::unexpected(NavMiss::Pending));
        CHECK(pending == "g");
        CHECK(navActionOf(character(U'g'), Navigating, keys, pending) == NavAction::First);
        CHECK(pending.empty());
    }
    SECTION("g then another key: the other key acts alone, and nothing jumps (Review Focus 4)")
    {
        CHECK(navActionOf(character(U'g'), Navigating, keys, pending) == std::unexpected(NavMiss::Pending));
        CHECK(navActionOf(character(U'j'), Navigating, keys, pending) == NavAction::MoveDown);
        CHECK(pending.empty());
    }
    SECTION("a key no action holds")
    {
        CHECK(navActionOf(character(U'x'), Navigating, keys, pending) == std::unexpected(NavMiss::NotBound));
    }
}

TEST_CASE("navActionOf while typing: letters type, esc leaves the field", "[orchestrator][view][navinput]")
{
    auto const keys = NavKeys::defaults(VimKeys::On);
    auto pending = std::string {};
    CHECK(navActionOf(character(U'j'), NavContext::Typing, keys, pending)
          == std::unexpected(NavMiss::NotBound));
    CHECK(navActionOf(special(KeyCode::Escape), NavContext::Typing, keys, pending) == NavAction::LeaveField);
}

TEST_CASE("listCommandOf: what a movement action is in each list", "[orchestrator][view][navinput]")
{
    CHECK(listCommandOf(NavAction::NextValue, View::Work) == KeyCommand::RepoNext);
    CHECK(listCommandOf(NavAction::NextValue, View::Agents) == KeyCommand::NextPage);
    CHECK(listCommandOf(NavAction::Close, View::Work) == std::nullopt);
    CHECK(listCommandOf(NavAction::EnterField, View::Agents) == std::nullopt);
    CHECK(listCommandOf(NavAction::LeaveField, View::Agents) == std::nullopt);
}

TEST_CASE("navKeyLabel names an action's first key", "[orchestrator][view][navinput]")
{
    CHECK(navKeyLabel(NavKeys::defaults(VimKeys::On), NavAction::MoveDown) == "↓");
    CHECK(navKeyLabel(NavKeys::defaults(VimKeys::On), NavAction::EnterField) == "i");
    CHECK(navKeyLabel(NavKeys::defaults(VimKeys::Off), NavAction::EnterField).empty());
    CHECK(navKeyLabel(NavKeys::defaults(VimKeys::On), NavAction::HalfPageDown) == "^d");
}

TEST_CASE("the key tables bind command keys only, as commandKeys() lists them",
          "[orchestrator][view][navinput]")
{
    // A command key shares a key with an action's own defaults only here (Decision 8).
    constexpr auto SharedWithActions = std::array { "enter"sv, "esc"sv };
    auto const defaults = NavKeys::defaults(VimKeys::On);
    for (auto const table: { keyRows(), workKeyRows() })
        for (auto const& row: table)
        {
            CAPTURE(row.key);
            CHECK(std::ranges::any_of(commandKeys(), [&](CommandKey const& command) {
                return command.name == row.key && command.context == NavContext::Navigating;
            }));
            if (!std::ranges::contains(SharedWithActions, row.key))
                CHECK(defaults.actionFor(row.key, NavContext::Navigating) == std::nullopt);
        }
}
