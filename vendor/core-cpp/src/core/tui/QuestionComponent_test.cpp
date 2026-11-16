// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/CellTestSupport.hpp>
#include <core/tui/QuestionComponent.hpp>
#include <core/tui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <type_traits>

using core::tui::test::requireCell;

using namespace core::tui;

namespace
{

/// Helper to create a KeyEvent for a printable character.
KeyEvent charKey(char c, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = keyCodeFromCodepoint(static_cast<char32_t>(c)),
                      .modifiers = mod,
                      .codepoint = static_cast<char32_t>(c) };
}

/// Helper to create a KeyEvent for a special key.
KeyEvent specialKey(KeyCode key, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = key, .modifiers = mod, .codepoint = 0 };
}

/// Helper to render a component and return the buffer text at a given row.
auto renderRow(QuestionComponent& comp, int row, int width = 60, int height = 15) -> std::string
{
    auto const& theme = currentTheme();
    auto buffer = Buffer(height, width);
    auto canvas = Canvas(buffer, Rect { .x = 0, .y = 0, .width = width, .height = height }, theme);
    comp.setArea(Rect { .x = 0, .y = 0, .width = width, .height = height });
    comp.setScreenBounds(Rect { .x = 0, .y = 0, .width = width, .height = height });
    comp.render(canvas);

    auto result = std::string {};
    for (auto const col: std::views::iota(0, std::max(0, width)))
    {
        auto const& cell = requireCell(buffer, { .x = col, .y = row });
        if (!cell.grapheme.empty() && cell.width > 0)
            result += cell.grapheme;
    }
    return result;
}

} // namespace

// ============================================================================
// Free-text mode tests
// ============================================================================

TEST_CASE("QuestionComponent.free_text_render", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "What is the path?",
    });

    auto const prefSize = comp.preferredSize();
    CHECK(prefSize.height > 0);
    CHECK(prefSize.width > 0);

    // Header row should contain "question"
    auto const headerRow = renderRow(comp, 0);
    CHECK(headerRow.contains("question"));

    // Second row should contain the question text
    auto const questionRow = renderRow(comp, 1);
    CHECK(questionRow.contains("What is the path?"));
}

TEST_CASE("QuestionComponent.free_text_confirm", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Enter name:",
    });

    // Type "hello"
    CHECK(comp.processInput(charKey('h')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('e')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('l')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('l')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('o')) == QuestionAction::Changed);

    // Press Enter to confirm
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "hello");
}

TEST_CASE("QuestionComponent.free_text_cancel", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Enter name:",
    });

    // Press Escape to cancel
    CHECK(comp.processInput(specialKey(KeyCode::Escape)) == QuestionAction::Cancelled);
}

// ============================================================================
// Single-select mode tests
// ============================================================================

TEST_CASE("QuestionComponent.single_select_render", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick an approach:",
        .options = { "Option A", "Option B", "Option C" },
        .allowOther = false,
    });

    auto const prefSize = comp.preferredSize();
    // Header(1) + question(1) + blank(1) + 3 options + hint(1) = 7
    CHECK(prefSize.height == 7);

    // Check that header contains "question"
    auto const headerRow = renderRow(comp, 0, 60, 10);
    CHECK(headerRow.contains("question"));
}

TEST_CASE("QuestionComponent.single_select_confirm", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick one:",
        .options = { "Alpha", "Beta", "Gamma" },
        .allowOther = false,
    });

    // First item is selected by default — press Enter
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "Alpha");
}

TEST_CASE("QuestionComponent.single_select_navigate_and_confirm", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick one:",
        .options = { "Alpha", "Beta", "Gamma" },
        .allowOther = false,
    });

    // Navigate down to "Beta"
    CHECK(comp.processInput(specialKey(KeyCode::Down)) == QuestionAction::Changed);
    // Confirm
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "Beta");
}

TEST_CASE("QuestionComponent.single_select_cancel", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick one:",
        .options = { "Alpha", "Beta" },
        .allowOther = false,
    });

    CHECK(comp.processInput(specialKey(KeyCode::Escape)) == QuestionAction::Cancelled);
}

TEST_CASE("QuestionComponent.single_select_other_transition", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick one:",
        .options = { "Alpha", "Beta" },
        .allowOther = true,
    });

    // Navigate to "Other..." (index 2)
    CHECK(comp.processInput(specialKey(KeyCode::Down)) == QuestionAction::Changed);
    CHECK(comp.processInput(specialKey(KeyCode::Down)) == QuestionAction::Changed);

    // Press Enter on "Other..." — should transition to InputField
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Changed);

    // Now type custom text
    CHECK(comp.processInput(charKey('c')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('u')) == QuestionAction::Changed);
    CHECK(comp.processInput(charKey('s')) == QuestionAction::Changed);

    // Confirm with Enter
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "cus");
}

TEST_CASE("QuestionComponent.single_select_other_back", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick:",
        .options = { "A", "B" },
        .allowOther = true,
    });

    // Navigate to "Other..." and select it
    (void) comp.processInput(specialKey(KeyCode::Down));
    (void) comp.processInput(specialKey(KeyCode::Down));
    (void) comp.processInput(specialKey(KeyCode::Enter));

    // Press Escape in "Other..." input — should go back to list
    CHECK(comp.processInput(specialKey(KeyCode::Escape)) == QuestionAction::Changed);

    // Cursor is still on "Other...", navigate back to "A" and confirm
    (void) comp.processInput(specialKey(KeyCode::Up));
    (void) comp.processInput(specialKey(KeyCode::Up));
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "A");
}

// ============================================================================
// Multi-select mode tests
// ============================================================================

TEST_CASE("QuestionComponent.multi_select_toggle_and_confirm", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Select features:",
        .options = { "Feature A", "Feature B", "Feature C" },
        .multiSelect = true,
        .allowOther = false,
    });

    // Toggle "Feature A" (already selected by cursor)
    CHECK(comp.processInput(charKey(' ')) == QuestionAction::Changed);

    // Navigate to "Feature C" and toggle
    (void) comp.processInput(specialKey(KeyCode::Down));
    (void) comp.processInput(specialKey(KeyCode::Down));
    CHECK(comp.processInput(charKey(' ')) == QuestionAction::Changed);

    // Confirm
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "Feature A, Feature C");
}

TEST_CASE("QuestionComponent.multi_select_cancel", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Select:",
        .options = { "A", "B" },
        .multiSelect = true,
        .allowOther = false,
    });

    CHECK(comp.processInput(specialKey(KeyCode::Escape)) == QuestionAction::Cancelled);
}

TEST_CASE("QuestionComponent.allow_other_false_no_other_item", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick:",
        .options = { "X", "Y" },
        .allowOther = false,
    });

    // Should only have 2 items, not 3
    auto const prefSize = comp.preferredSize();
    // Header(1) + question(1) + blank(1) + 2 options + hint(1) = 6
    CHECK(prefSize.height == 6);

    // Navigate past last item — should still select "Y"
    (void) comp.processInput(specialKey(KeyCode::Down));
    (void) comp.processInput(specialKey(KeyCode::Down)); // Can't go past end
    CHECK(comp.processInput(specialKey(KeyCode::Enter)) == QuestionAction::Confirmed);
    CHECK(comp.answer() == "Y");
}

TEST_CASE("QuestionComponent.cursor_shape_free_text", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Type:",
    });

    CHECK(comp.cursorShape() == CursorShape::SteadyBar);
}

TEST_CASE("QuestionComponent.cursor_shape_list", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Pick:",
        .options = { "A", "B" },
    });

    // List mode — no InputField active
    CHECK(comp.cursorShape() == CursorShape::Default);
}

TEST_CASE("QuestionComponent.multiline_question_text", "[tui][question]")
{
    auto comp = QuestionComponent(QuestionConfig {
        .questionText = "Allow shell_execute (Execute command)?\ncmake --build --preset clang-debug",
        .options = { "Yes", "No" },
        .allowOther = false,
    });

    // Two question lines + header(1) + blank(1) + 2 options + hint(1) = 7
    CHECK(comp.preferredSize().height == 7);

    // Render and verify both lines appear on separate rows.
    auto const size = comp.preferredSize();
    auto buffer = Buffer(size.height, 80);
    auto canvas = Canvas(buffer, Rect { .x = 0, .y = 0, .width = 80, .height = size.height }, currentTheme());
    comp.setArea(Rect { .x = 0, .y = 0, .width = 80, .height = size.height });
    comp.render(canvas);

    // Row 0: header (╭─ question ─...)
    // Row 1: first question line (│  Allow shell_execute...)
    // Row 2: second question line (│  cmake --build...)
    auto row1Text = std::string {};
    for (auto const col: std::views::iota(0, 80))
        row1Text += requireCell(buffer, { .x = col, .y = 1 }).grapheme;
    CHECK(row1Text.contains("Allow shell_execute"));

    auto row2Text = std::string {};
    for (auto const col: std::views::iota(0, 80))
        row2Text += requireCell(buffer, { .x = col, .y = 2 }).grapheme;
    CHECK(row2Text.contains("cmake --build --preset clang-debug"));
}

// ============================================================================
// List multi-select tests
// ============================================================================

TEST_CASE("List.multi_select_toggle", "[tui][list]")
{
    auto list = List(std::vector<ListItem> {
        { .label = "Item A" },
        { .label = "Item B" },
        { .label = "Item C" },
    });
    list.setMultiSelect(true);

    CHECK_FALSE(list.isChecked(0));
    CHECK_FALSE(list.isChecked(1));

    // Toggle first item via Space
    auto const action = list.processEvent(charKey(' '));
    CHECK(action == ListAction::Toggled);
    CHECK(list.isChecked(0));
    CHECK_FALSE(list.isChecked(1));

    // Toggle again
    (void) list.processEvent(charKey(' '));
    CHECK_FALSE(list.isChecked(0));
}

TEST_CASE("List.checked_indices", "[tui][list]")
{
    auto list = List(std::vector<ListItem> {
        { .label = "A" },
        { .label = "B" },
        { .label = "C" },
    });
    list.setMultiSelect(true);

    list.setChecked(0, true);
    list.setChecked(2, true);

    auto const indices = list.checkedIndices();
    REQUIRE(indices.size() == 2);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 2);
}

TEST_CASE("List.multi_select_render_checked_single_item", "[tui][list]")
{
    // Test with a single-item list to avoid the pre-existing fill Rect argument-order issue
    // when multiple rows are rendered. Verified via style_check and putString_basic tests above.
    auto list = List(std::vector<ListItem> {
        { .label = "On" },
    });
    list.setMultiSelect(true);
    list.setChecked(0, true);

    auto const& theme = currentTheme();
    auto buffer = Buffer(1, 40);
    auto canvas = Canvas(buffer, Rect { .x = 0, .y = 0, .width = 40, .height = 1 }, theme);
    list.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 1 });
    list.render(canvas);

    // Single row should contain checked prefix "[x]" followed by item label
    auto firstRow = std::string {};
    for (auto const col: std::views::iota(0, 20))
    {
        auto const& cell = requireCell(buffer, { .x = col, .y = 0 });
        if (!cell.grapheme.empty() && cell.width > 0)
            firstRow += cell.grapheme;
    }
    INFO("firstRow = '" << firstRow << "'");
    CHECK(firstRow.contains("[x]"));
    CHECK(firstRow.contains("On"));
}

TEST_CASE("List.space_no_toggle_in_single_select", "[tui][list]")
{
    auto list = List(std::vector<ListItem> {
        { .label = "A" },
        { .label = "B" },
    });
    // multiSelect is false by default

    auto const action = list.processEvent(charKey(' '));
    CHECK(action == ListAction::None);
}

TEST_CASE("QuestionComponent.injectedClockReachesTheInputField")
{
    auto const clock = core::ManualClock {};
    auto const question = QuestionComponent(QuestionConfig { .questionText = "Name?" }, clock);
    CHECK(&question.clock() == &clock);
    CHECK(&QuestionComponent(QuestionConfig { .questionText = "Name?" }).clock()
          == &core::defaultSteadyClock());

    STATIC_CHECK_FALSE(std::is_constructible_v<QuestionComponent, QuestionConfig, core::ManualClock>);
}
