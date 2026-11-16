// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/FuzzyPickerPopup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace core::tui;
using namespace core::tui::completer;

namespace
{

/// Helper to build test items.
std::vector<std::string> makeTestItems()
{
    return {
        "src/tui/FuzzyMatch.hpp",
        "src/tui/FuzzyPickerPopup.hpp",
        "src/tui/FuzzyPickerPopup.cpp",
        "src/tui/CommandPalettePopup.hpp",
        "src/shell/Shell.cpp",
        "src/shell/completion/FileCompleter.cpp",
        "README.md",
        "CMakeLists.txt",
    };
}

/// Helper to send a character key event.
InputEvent charKey(char ch)
{
    return KeyEvent {
        .key = static_cast<KeyCode>(ch),
        .modifiers = Modifier::None,
        .codepoint = static_cast<char32_t>(ch),
    };
}

/// Helper to send a special key event.
InputEvent specialKey(KeyCode code, Modifier mod = Modifier::None)
{
    return KeyEvent {
        .key = code,
        .modifiers = mod,
        .codepoint = 0,
    };
}

} // namespace

// ============================================================================
// Visibility tests
// ============================================================================

TEST_CASE("FuzzyPickerPopup.initial_state")
{
    FuzzyPickerPopup const popup;
    CHECK_FALSE(popup.visible());
    CHECK(popup.selectedItem() == nullptr);
}

TEST_CASE("FuzzyPickerPopup.show_makes_visible")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());
    CHECK(popup.visible());
    CHECK(popup.selectedItem() != nullptr);
}

TEST_CASE("FuzzyPickerPopup.show_with_title")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems(), "File> ");
    CHECK(popup.visible());
}

TEST_CASE("FuzzyPickerPopup.show_empty_stays_visible")
{
    FuzzyPickerPopup popup;
    popup.show({});
    // Even with empty items, popup is visible (user can see "no results")
    CHECK(popup.visible());
}

TEST_CASE("FuzzyPickerPopup.hide")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());
    CHECK(popup.visible());

    popup.hide();
    CHECK_FALSE(popup.visible());
    CHECK(popup.selectedItem() == nullptr);
}

// ============================================================================
// Navigation tests
// ============================================================================

TEST_CASE("FuzzyPickerPopup.escape_dismisses")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    auto const action = popup.processEvent(specialKey(KeyCode::Escape));
    CHECK(action == FuzzyPickerAction::Dismissed);
    CHECK_FALSE(popup.visible());
}

TEST_CASE("FuzzyPickerPopup.enter_accepts")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // First item should be selected initially
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "src/tui/FuzzyMatch.hpp");

    auto const action = popup.processEvent(specialKey(KeyCode::Enter));
    CHECK(action == FuzzyPickerAction::Accepted);
}

TEST_CASE("FuzzyPickerPopup.down_arrow_navigates")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // Move down once
    auto const action = popup.processEvent(specialKey(KeyCode::Down));
    CHECK(action == FuzzyPickerAction::Changed);

    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "src/tui/FuzzyPickerPopup.hpp");
}

TEST_CASE("FuzzyPickerPopup.up_arrow_wraps")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // From first item, Up should wrap to last
    auto const action = popup.processEvent(specialKey(KeyCode::Up));
    CHECK(action == FuzzyPickerAction::Changed);

    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "CMakeLists.txt");
}

// ============================================================================
// Fuzzy filtering tests
// ============================================================================

TEST_CASE("FuzzyPickerPopup.typing_filters_items")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // Type "shell" to filter
    for (auto ch: std::string_view("shell"))
    {
        auto const action = popup.processEvent(charKey(ch));
        CHECK(action == FuzzyPickerAction::Changed);
    }

    // Should still be visible and first match should contain "shell"
    CHECK(popup.visible());
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(selected->contains("shell"));
}

TEST_CASE("FuzzyPickerPopup.fuzzy_matching")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // Type "fzm" which should fuzzy-match "FuzzyMatch"
    for (auto ch: std::string_view("fzm"))
    {
        (void) popup.processEvent(charKey(ch));
    }

    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(selected->contains("FuzzyMatch"));
}

TEST_CASE("FuzzyPickerPopup.substring_matches_prioritized")
{
    FuzzyPickerPopup popup;
    // Both "shell.cpp" and "src/shell/completion/FileCompleter.cpp" contain "shell"
    // as an exact-case contiguous substring, so they land in the same match tier.
    // (Both lowercase so case-sensitivity does not confound the comparison — that
    // dimension is covered by case_sensitive_substring_ranked_higher.) The shorter
    // path with higher coverage should then rank higher.
    popup.show({
        "src/shell/completion/FileCompleter.cpp",
        "shell.cpp",
        "src/tui/FuzzyMatch.hpp",
    });

    for (auto ch: std::string_view("shell"))
    {
        (void) popup.processEvent(charKey(ch));
    }

    // "shell.cpp" should rank first (same substring tier, higher coverage percentage)
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "shell.cpp");
}

TEST_CASE("FuzzyPickerPopup.coverage_affects_ranking")
{
    FuzzyPickerPopup popup;
    popup.show({
        "very/long/deeply/nested/path/to/README.md",
        "README.md",
    });

    for (auto ch: std::string_view("readme"))
    {
        (void) popup.processEvent(charKey(ch));
    }

    // Shorter path with higher coverage should rank first
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "README.md");
}

TEST_CASE("FuzzyPickerPopup.case_sensitive_substring_ranked_higher")
{
    FuzzyPickerPopup popup;
    popup.show({
        "TODO.md",
        "todo.md",
        "src/to/do/file.md",
    });

    for (auto ch: std::string_view("todo"))
        (void) popup.processEvent(charKey(ch));

    // Tier 1 (exact case) ranks above Tier 2 (case-insensitive) ranks above fuzzy
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "todo.md");
}

TEST_CASE("FuzzyPickerPopup.tier_ordering_same_coverage")
{
    FuzzyPickerPopup popup;
    popup.show({
        "TODO.txt",
        "todo.txt",
    });

    for (auto ch: std::string_view("todo"))
        (void) popup.processEvent(charKey(ch));

    // Same coverage, but case-sensitive match wins
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "todo.txt");
}

TEST_CASE("FuzzyPickerPopup.uppercase_pattern_matches_both_cases")
{
    FuzzyPickerPopup popup;
    popup.show({
        "todo.md",
        "TODO.md",
    });

    for (auto ch: std::string_view("TODO"))
        (void) popup.processEvent(charKey(ch));

    // Uppercase pattern: TODO.md is Tier 1 (exact case), todo.md is Tier 2
    auto const* selected = popup.selectedItem();
    REQUIRE(selected != nullptr);
    CHECK(*selected == "TODO.md");
}

TEST_CASE("FuzzyPickerPopup.filter_no_matches")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    // Type something that matches nothing
    for (auto ch: std::string_view("zzzzzzzzz"))
    {
        (void) popup.processEvent(charKey(ch));
    }

    CHECK(popup.visible());
    CHECK(popup.selectedItem() == nullptr);
}

// ============================================================================
// Size tests
// ============================================================================

TEST_CASE("FuzzyPickerPopup.preferredSize_reasonable")
{
    FuzzyPickerPopup popup;
    popup.show(makeTestItems());

    auto const size = popup.preferredSize();
    CHECK(size.width >= 50);
    CHECK(size.width <= 100);
    CHECK(size.height >= 4); // Minimum: borders + filter + separator
}

TEST_CASE("FuzzyPickerPopup.preferredSize_when_hidden")
{
    FuzzyPickerPopup const popup;
    auto const size = popup.preferredSize();

    CHECK(size.width == 0);
    CHECK(size.height == 0);
}

TEST_CASE("FuzzyPickerPopup.injectedClockReachesTheFilterField")
{
    auto const clock = core::ManualClock {};
    auto const picker = FuzzyPickerPopup { clock };
    CHECK(&picker.clock() == &clock);
    CHECK_FALSE(picker.visible()); // Hidden from the start, as with the default constructor.
    CHECK(&FuzzyPickerPopup {}.clock() == &core::defaultSteadyClock());

    STATIC_CHECK_FALSE(std::is_constructible_v<FuzzyPickerPopup, core::ManualClock>);
}
