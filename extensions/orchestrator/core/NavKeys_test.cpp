// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/DashboardKeys.hpp>
#include <orchestrator/core/NavKeys.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

namespace
{

/// One action's configured keys.
struct Configured
{
    NavAction action;
    std::vector<std::string> keys;
};

/// @return navKeysOf() for @p rows, every other action keeping its defaults.
[[nodiscard]] std::expected<NavKeys, KeyBindingProblem> configure(std::vector<Configured> const& rows,
                                                                  VimKeys vim = VimKeys::On)
{
    auto configured = std::array<std::optional<std::vector<std::string>>, NavActionCount> {};
    for (auto const& row: rows)
        configured[static_cast<std::size_t>(row.action)] = row.keys;
    return navKeysOf(vim, configured);
}

/// @return Whether @p actual names exactly @p expected, in order.
[[nodiscard]] bool keysAre(std::span<std::string const> actual, std::vector<std::string_view> const& expected)
{
    return std::ranges::equal(actual, expected);
}

} // namespace

TEST_CASE("NavKeys: the rows are in NavAction order, with config names", "[orchestrator][navkeys]")
{
    REQUIRE(navActionRows().size() == NavActionCount);
    for (auto const [index, row]: std::views::enumerate(navActionRows()))
    {
        CAPTURE(row.configName);
        CHECK(static_cast<std::size_t>(row.action) == static_cast<std::size_t>(index));
        CHECK_FALSE(row.configName.empty());
        for (auto const key: row.plainKeys)
            CHECK(keyShapeOf(key) != KeyShape::Invalid);
        for (auto const key: row.vimKeys)
            CHECK(keyShapeOf(key) != KeyShape::Invalid);
    }
    // Every default set is itself accepted.
    auto const none = std::array<std::optional<std::vector<std::string>>, NavActionCount> {};
    CHECK(navKeysOf(VimKeys::On, none) == NavKeys::defaults(VimKeys::On));
    CHECK(navKeysOf(VimKeys::Off, none) == NavKeys::defaults(VimKeys::Off));
}

TEST_CASE("NavKeys: the defaults, with and without vim", "[orchestrator][navkeys]")
{
    auto const vim = NavKeys::defaults(VimKeys::On);
    CHECK(std::ranges::equal(vim.keysOf(NavAction::MoveDown), std::array { "down"sv, "j"sv }));
    CHECK(std::ranges::equal(vim.keysOf(NavAction::First), std::array { "home"sv, "gg"sv }));
    CHECK(std::ranges::equal(vim.keysOf(NavAction::HalfPageDown), std::array { "^d"sv }));
    CHECK(vim.actionFor("j", NavContext::Navigating) == NavAction::MoveDown);
    CHECK_FALSE(vim.actionFor("j", NavContext::Typing).has_value());
    CHECK(vim.actionFor("esc", NavContext::Typing) == NavAction::LeaveField);
    CHECK(vim.actionFor("esc", NavContext::Navigating) == NavAction::Close);
    CHECK(vim.actionFor("gg", NavContext::Navigating) == NavAction::First);
    CHECK(vim.startsSequence("g", NavContext::Navigating));
    CHECK_FALSE(vim.startsSequence("j", NavContext::Navigating));
    CHECK_FALSE(vim.startsSequence("g", NavContext::Typing));
    auto const plain = NavKeys::defaults(VimKeys::Off);
    CHECK(std::ranges::equal(plain.keysOf(NavAction::MoveDown), std::array { "down"sv }));
    CHECK(plain.keysOf(NavAction::HalfPageDown).empty());
    CHECK_FALSE(plain.actionFor("q", NavContext::Navigating).has_value());
    CHECK(plain.actionFor("esc", NavContext::Navigating) == NavAction::Close);
    CHECK_FALSE(plain.startsSequence("g", NavContext::Navigating));
}

TEST_CASE("keyShapeOf", "[orchestrator][navkeys]")
{
    for (auto const key: { "j"sv, "G"sv, "^"sv, "0"sv, "/"sv })
    {
        CAPTURE(key);
        CHECK(keyShapeOf(key) == KeyShape::Printable);
    }
    for (auto const key: { "^d"sv, "^u"sv, "^c"sv })
    {
        CAPTURE(key);
        CHECK(keyShapeOf(key) == KeyShape::Ctrl);
    }
    for (auto const key: { "gg"sv, "zz"sv })
    {
        CAPTURE(key);
        CHECK(keyShapeOf(key) == KeyShape::Sequence);
    }
    for (auto const key: SpecialKeyNames)
    {
        CAPTURE(key);
        CHECK(keyShapeOf(key) == KeyShape::Special);
    }
    for (auto const key: { ""sv,
                           " "sv,
                           "^h"sv,
                           "^i"sv,
                           "^j"sv,
                           "^m"sv,
                           "^D"sv,
                           "^1"sv,
                           "ggg"sv,
                           "PgDn"sv,
                           "\t"sv,
                           "\xc3\xa9"sv })
    {
        CAPTURE(key);
        CHECK(keyShapeOf(key) == KeyShape::Invalid);
    }
}

TEST_CASE("navKeysOf: a configured list replaces the action's defaults", "[orchestrator][navkeys]")
{
    auto const replaced = configure({ { NavAction::MoveDown, { "J", "down" } } });
    REQUIRE(replaced.has_value());
    CHECK(keysAre(replaced->keysOf(NavAction::MoveDown), { "J", "down" }));
    CHECK(replaced->actionFor("J", NavContext::Navigating) == NavAction::MoveDown);
    CHECK_FALSE(replaced->actionFor("j", NavContext::Navigating).has_value());

    auto const none = configure({ { NavAction::HalfPageDown, {} } });
    REQUIRE(none.has_value());
    CHECK(none->keysOf(NavAction::HalfPageDown).empty());

    auto const plainJ = configure({ { NavAction::MoveDown, { "j" } } }, VimKeys::Off);
    REQUIRE(plainJ.has_value());
    CHECK(plainJ->actionFor("j", NavContext::Navigating) == NavAction::MoveDown);

    auto const own =
        configure({ { NavAction::EnterField, { "enter" } }, { NavAction::Close, { "esc", "q" } } });
    REQUIRE(own.has_value());
    CHECK(keysAre(own->keysOf(NavAction::Close), { "esc", "q" }));

    auto const leave = configure({ { NavAction::LeaveField, { "^c" } } });
    REQUIRE(leave.has_value());
    CHECK(leave->actionFor("^c", NavContext::Typing) == NavAction::LeaveField);
    CHECK_FALSE(leave->actionFor("esc", NavContext::Typing).has_value());

    auto const zero = configure({ { NavAction::First, { "0" } } });
    REQUIRE(zero.has_value());
    CHECK(keysAre(zero->keysOf(NavAction::First), { "0" }));

    // A key listed twice in one action is kept once.
    auto const twice = configure({ { NavAction::MoveUp, { "K", "up", "K" } } });
    REQUIRE(twice.has_value());
    CHECK(keysAre(twice->keysOf(NavAction::MoveUp), { "K", "up" }));

    // A key a dropped default freed may be taken by another action.
    auto const freed = configure({ { NavAction::MoveDown, { "down" } }, { NavAction::Close, { "j" } } });
    REQUIRE(freed.has_value());
    CHECK(freed->actionFor("j", NavContext::Navigating) == NavAction::Close);
}

TEST_CASE("navKeysOf: what is refused, and the subject that names the other holder",
          "[orchestrator][navkeys]")
{
    struct Row
    {
        std::vector<Configured> configured;
        KeyBindingProblem problem;
    };
    auto const taken = [](NavAction action, std::string subject) {
        return KeyBindingProblem {
            .action = action, .index = 0, .refusal = KeyRefusal::Taken, .subject = std::move(subject)
        };
    };
    auto const rows = std::vector<Row> {
        { { { NavAction::MoveUp, { "^h" } } },
          { .action = NavAction::MoveUp, .index = 0, .refusal = KeyRefusal::NotAKey } },
        { { { NavAction::MoveUp, { "up", "PgDn" } } },
          { .action = NavAction::MoveUp, .index = 1, .refusal = KeyRefusal::NotAKey } },
        { { { NavAction::LeaveField, { "x" } } },
          { .action = NavAction::LeaveField, .index = 0, .refusal = KeyRefusal::TypesText } },
        { { { NavAction::LeaveField, { "gg" } } },
          { .action = NavAction::LeaveField, .index = 0, .refusal = KeyRefusal::TypesText } },
        { { { NavAction::Close, {} } }, { .action = NavAction::Close, .refusal = KeyRefusal::Unbound } },
        { { { NavAction::LeaveField, {} } },
          { .action = NavAction::LeaveField, .refusal = KeyRefusal::Unbound } },
        { { { NavAction::MoveDown, { "e" } } },
          taken(NavAction::MoveDown, "e, the dashboard's key to end the run") },
        { { { NavAction::MoveDown, { "enter" } } },
          taken(NavAction::MoveDown, "enter, the dashboard's key to start or open the selected row") },
        { { { NavAction::LeaveField, { "^u" } } },
          taken(NavAction::LeaveField, "^u, the dashboard's key to clear the text") },
        { { { NavAction::Close, { "j" } } }, taken(NavAction::Close, "j, also keys.move_down") },
        { { { NavAction::Last, { "g" } } }, taken(NavAction::Last, "g, which starts keys.first's gg") },
        { { { NavAction::MoveDown, { "ee" } } },
          taken(NavAction::MoveDown, "ee, which starts with the dashboard's key to end the run") },
        { { { NavAction::MoveUp, { "jx" } } },
          taken(NavAction::MoveUp, "jx, which starts with keys.move_down's j") },
        { { { NavAction::MoveDown, { "J" } }, { NavAction::Last, { "J" } } },
          taken(NavAction::Last, "J, also keys.move_down") },
        { { { NavAction::First, { "y" } }, { NavAction::Last, { "yy" } } },
          taken(NavAction::Last, "yy, which starts with keys.first's y") },
        // A sequence and its first key in one action: the later entry is refused.
        { { { NavAction::First, { "y", "yy" } } },
          { .action = NavAction::First,
            .index = 1,
            .refusal = KeyRefusal::Taken,
            .subject = "yy, which starts with keys.first's y" } },
        { { { NavAction::LeaveField, { "^p" } } },
          taken(NavAction::LeaveField,
                "^p, the dashboard's key to show the prompt or choose the previous palette entry") },
        // The palette, typed, chooses its entry with up and down.
        { { { NavAction::LeaveField, { "down" } } },
          taken(NavAction::LeaveField, "down, the dashboard's key to choose the next palette entry") },
        { { { NavAction::LeaveField, { "up" } } },
          taken(NavAction::LeaveField, "up, the dashboard's key to choose the previous palette entry") },
        { { { NavAction::LeaveField, { "^n" } } },
          taken(NavAction::LeaveField, "^n, the dashboard's key to choose the next palette entry") },
        { { { NavAction::LeaveField, { "left" } } },
          taken(NavAction::LeaveField, "left, the dashboard's key to turn to the previous transcript page") },
        // enter_field holds enter too; the command is reported first (Decision 8's order).
        { { { NavAction::Close, { "enter" } } },
          taken(NavAction::Close, "enter, the dashboard's key to start or open the selected row") },
        { { { NavAction::MoveDown, { "3" } } },
          taken(NavAction::MoveDown, "3, the dashboard's key to answer with quick reply 3") },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.problem.subject, row.problem.action, row.problem.refusal);
        auto const keys = configure(row.configured);
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error() == row.problem);
    }

    // ^u clears only while typing: half_page_up may have it.
    CHECK(configure({ { NavAction::HalfPageUp, { "^u" } } }).has_value());
}

TEST_CASE("commandKeys: the message box's pass-through keys are Typing command keys",
          "[orchestrator][navkeys]")
{
    for (auto const& pass: messagePassKeys())
    {
        CAPTURE(pass.name);
        auto const typing = std::ranges::find_if(commandKeys(), [&](CommandKey const& key) {
            return key.name == pass.name && key.context == NavContext::Typing;
        });
        REQUIRE(typing != commandKeys().end());
        CHECK(typing->does == pass.does);
    }
    CHECK(std::ranges::equal(messagePassKeys() | std::views::transform(&MessagePassKey::name),
                             std::array { "pgup"sv, "pgdn"sv, "end"sv, "left"sv, "right"sv }));
    for (auto const& key: commandKeys())
    {
        CAPTURE(key.name);
        CHECK(keyShapeOf(key.name) != KeyShape::Invalid);
        CHECK_FALSE(key.does.empty());
    }
}
