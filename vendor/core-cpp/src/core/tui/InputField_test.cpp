// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/CellTestSupport.hpp>
#include <core/tui/InputField.hpp>
#include <core/tui/TextDecorator.hpp>
#include <core/tui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <type_traits>

using core::ManualClock;
using core::tui::test::requireCell;

using namespace core::tui;

namespace
{

// Helper to create a KeyEvent for a printable character
KeyEvent charKey(char c, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = keyCodeFromCodepoint(static_cast<char32_t>(c)),
                      .modifiers = mod,
                      .codepoint = static_cast<char32_t>(c) };
}

// Helper to create a KeyEvent for a special key
KeyEvent specialKey(KeyCode key, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = key, .modifiers = mod, .codepoint = 0 };
}

} // namespace

// ============================================================================
// Basic editing tests
// ============================================================================

TEST_CASE("InputField.empty_initial_state")
{
    InputField const field;
    CHECK(field.text().empty());
    CHECK(field.cursor() == 0);
    CHECK(field.lineCount() == 1);
    CHECK(field.cursorLine() == 0);
    CHECK(field.cursorColumn() == 0);
}

TEST_CASE("InputField.insert_characters")
{
    InputField field;

    (void) field.processEvent(charKey('h'));
    (void) field.processEvent(charKey('e'));
    (void) field.processEvent(charKey('l'));
    (void) field.processEvent(charKey('l'));
    (void) field.processEvent(charKey('o'));

    CHECK(field.text() == "hello");
    CHECK(field.cursor() == 5);
}

TEST_CASE("InputField.backspace")
{
    InputField field;
    field.setText("hello");

    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "hell");
    CHECK(field.cursor() == 4);

    (void) field.processEvent(specialKey(KeyCode::Backspace));
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "he");
    CHECK(field.cursor() == 2);
}

TEST_CASE("InputField.delete_key")
{
    InputField field;
    field.setText("hello");

    // Move cursor to start
    (void) field.processEvent(specialKey(KeyCode::Home));
    CHECK(field.cursor() == 0);

    (void) field.processEvent(specialKey(KeyCode::Delete));
    CHECK(field.text() == "ello");

    (void) field.processEvent(specialKey(KeyCode::Delete));
    CHECK(field.text() == "llo");
}

TEST_CASE("InputField.clear")
{
    InputField field;
    field.setText("hello world");

    field.clear();
    CHECK(field.text().empty());
    CHECK(field.cursor() == 0);
}

TEST_CASE("InputField.setText")
{
    InputField field;
    field.setText("hello");
    CHECK(field.text() == "hello");
    CHECK(field.cursor() == 5); // Cursor at end

    field.setText("world");
    CHECK(field.text() == "world");
    CHECK(field.cursor() == 5);
}

// ============================================================================
// Cursor movement tests
// ============================================================================

TEST_CASE("InputField.cursor_movement_left_right")
{
    InputField field;
    field.setText("hello");

    // Cursor starts at end
    CHECK(field.cursor() == 5);

    // Move left
    (void) field.processEvent(specialKey(KeyCode::Left));
    CHECK(field.cursor() == 4);

    (void) field.processEvent(specialKey(KeyCode::Left));
    (void) field.processEvent(specialKey(KeyCode::Left));
    CHECK(field.cursor() == 2);

    // Move right
    (void) field.processEvent(specialKey(KeyCode::Right));
    CHECK(field.cursor() == 3);

    // Can't move past end
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right)); // Should stop at end
    CHECK(field.cursor() == 5);
}

TEST_CASE("InputField.cursor_home_end")
{
    InputField field;
    field.setText("hello world");

    (void) field.processEvent(specialKey(KeyCode::Home));
    CHECK(field.cursor() == 0);

    (void) field.processEvent(specialKey(KeyCode::End));
    CHECK(field.cursor() == 11);
}

TEST_CASE("InputField.word_movement")
{
    InputField field;
    field.setText("hello world test");
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Move forward by word (Ctrl+Right)
    (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Ctrl));
    CHECK(field.cursor() == 5); // After "hello"

    (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Ctrl));
    CHECK(field.cursor() == 11); // After "world"

    // Move backward by word (Ctrl+Left)
    (void) field.processEvent(specialKey(KeyCode::Left, Modifier::Ctrl));
    CHECK(field.cursor() == 6); // Before "world"
}

// ============================================================================
// Selection tests
// ============================================================================

TEST_CASE("InputField.selection_with_shift_arrows")
{
    InputField field;
    field.setText("hello");
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Select forward with Shift+Right
    (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Shift));
    CHECK(field.hasSelection());
    CHECK(field.selectionStart() == 0);
    CHECK(field.selectionEnd() == 1);
    CHECK(field.selectedText() == "h");

    (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Shift));
    (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Shift));
    CHECK(field.selectedText() == "hel");
}

TEST_CASE("InputField.selection_shift_home_end")
{
    InputField field;
    field.setText("hello world");

    // Start in middle
    (void) field.processEvent(specialKey(KeyCode::Home));
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));
    CHECK(field.cursor() == 3);

    // Shift+Home selects to start
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Shift));
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hel");

    // Clear and try Shift+End
    field.clearSelection();
    (void) field.processEvent(specialKey(KeyCode::Home));
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));

    (void) field.processEvent(specialKey(KeyCode::End, Modifier::Shift));
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "lo world");
}

TEST_CASE("InputField.select_all")
{
    InputField field;
    field.setText("hello world");

    // Ctrl+Shift+A selects all
    (void) field.processEvent(charKey('a', Modifier::Ctrl | Modifier::Shift));
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hello world");
}

TEST_CASE("InputField.selection_cleared_on_unshifted_movement")
{
    InputField field;
    field.setText("hello");
    (void) field.processEvent(charKey('a', Modifier::Ctrl | Modifier::Shift)); // Select all

    CHECK(field.hasSelection());

    // Unshifted movement clears selection
    (void) field.processEvent(specialKey(KeyCode::Left));
    CHECK_FALSE(field.hasSelection());
}

TEST_CASE("InputField.typing_replaces_selection")
{
    InputField field;
    field.setText("hello");
    (void) field.processEvent(charKey('a', Modifier::Ctrl | Modifier::Shift)); // Select all

    // Typing replaces selection
    (void) field.processEvent(charKey('x'));
    CHECK(field.text() == "x");
    CHECK_FALSE(field.hasSelection());
}

TEST_CASE("InputField.backspace_deletes_selection")
{
    InputField field;
    field.setText("hello world");
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Select "hello"
    for ([[maybe_unused]] auto const i: std::views::iota(0, 5))
        (void) field.processEvent(specialKey(KeyCode::Right, Modifier::Shift));

    CHECK(field.selectedText() == "hello");

    // Backspace deletes selection
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == " world");
    CHECK_FALSE(field.hasSelection());
}

// ============================================================================
// Undo/Redo tests
// ============================================================================

TEST_CASE("InputField.undo_single_operation")
{
    InputField field;

    CHECK_FALSE(field.canUndo());

    (void) field.processEvent(charKey('h'));
    (void) field.processEvent(charKey('i'));

    CHECK(field.text() == "hi");
    CHECK(field.canUndo());

    // Undo last character
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    CHECK(field.text() == "h");

    // Undo first character
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    CHECK(field.text().empty());
}

TEST_CASE("InputField.redo")
{
    InputField field;

    (void) field.processEvent(charKey('a'));
    (void) field.processEvent(charKey('b'));
    (void) field.processEvent(charKey('c'));
    CHECK(field.text() == "abc");

    // Undo twice
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    CHECK(field.text() == "a");
    CHECK(field.canRedo());

    // Redo once
    (void) field.processEvent(charKey('z', Modifier::Ctrl | Modifier::Shift));
    CHECK(field.text() == "ab");

    // Redo again
    (void) field.processEvent(charKey('z', Modifier::Ctrl | Modifier::Shift));
    CHECK(field.text() == "abc");
    CHECK_FALSE(field.canRedo());
}

TEST_CASE("InputField.redo_ctrl_y")
{
    InputField field;

    (void) field.processEvent(charKey('a'));
    (void) field.processEvent(charKey('b'));
    (void) field.processEvent(charKey('c'));
    CHECK(field.text() == "abc");

    // Undo twice
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    CHECK(field.text() == "a");

    // Redo with Ctrl+Y (modern keybinding)
    (void) field.processEvent(charKey('y', Modifier::Ctrl));
    CHECK(field.text() == "ab");

    // Redo again with Ctrl+Y
    (void) field.processEvent(charKey('y', Modifier::Ctrl));
    CHECK(field.text() == "abc");
}

TEST_CASE("InputField.new_edit_clears_redo_stack")
{
    InputField field;

    (void) field.processEvent(charKey('a'));
    (void) field.processEvent(charKey('b'));

    // Undo
    (void) field.processEvent(charKey('z', Modifier::Ctrl));
    CHECK(field.text() == "a");
    CHECK(field.canRedo());

    // New edit clears redo
    (void) field.processEvent(charKey('x'));
    CHECK(field.text() == "ax");
    CHECK_FALSE(field.canRedo());
}

TEST_CASE("InputField.clear_undo_history")
{
    InputField field;

    (void) field.processEvent(charKey('a'));
    (void) field.processEvent(charKey('b'));
    CHECK(field.canUndo());

    field.clearUndoHistory();
    CHECK_FALSE(field.canUndo());
    CHECK_FALSE(field.canRedo());
}

// ============================================================================
// Multiline tests
// ============================================================================

TEST_CASE("InputField.multiline_disabled_by_default")
{
    InputField const field;
    CHECK_FALSE(field.isMultiline());
}

TEST_CASE("InputField.multiline_line_count")
{
    InputField field;
    field.setMultiline(true);
    field.setText("line1\nline2\nline3");

    CHECK(field.lineCount() == 3);
}

TEST_CASE("InputField.multiline_cursor_line_and_column")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Cursor at end
    CHECK(field.cursorLine() == 1);
    CHECK(field.cursorColumn() == 5);

    // Move to start of buffer
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Ctrl));
    CHECK(field.cursorLine() == 0);
    CHECK(field.cursorColumn() == 0);

    // Move right to 'e'
    (void) field.processEvent(specialKey(KeyCode::Right));
    CHECK(field.cursorLine() == 0);
    CHECK(field.cursorColumn() == 1);
}

TEST_CASE("InputField.multiline_lineAt")
{
    InputField field;
    field.setMultiline(true);
    field.setText("first\nsecond\nthird");

    CHECK(field.lineAt(0) == "first");
    CHECK(field.lineAt(1) == "second");
    CHECK(field.lineAt(2) == "third");
    CHECK(field.lineAt(3).empty());  // Out of range
    CHECK(field.lineAt(-1).empty()); // Negative index
}

TEST_CASE("InputField.multiline_up_down_movement")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld\ntest");

    // Start at end of last line
    CHECK(field.cursorLine() == 2);

    // Move up
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.cursorLine() == 1);

    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.cursorLine() == 0);
}

TEST_CASE("InputField.multiline_insert_newline_with_alt_enter")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello");
    (void) field.processEvent(specialKey(KeyCode::End));

    // Alt+Enter inserts newline
    (void) field.processEvent(specialKey(KeyCode::Enter, Modifier::Alt));
    CHECK(field.text() == "hello\n");
    CHECK(field.lineCount() == 2);
}

TEST_CASE("InputField.multiline_insert_newline_with_shift_enter")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello");
    (void) field.processEvent(specialKey(KeyCode::End));

    // Shift+Enter inserts newline
    (void) field.processEvent(specialKey(KeyCode::Enter, Modifier::Shift));
    CHECK(field.text() == "hello\n");
    CHECK(field.lineCount() == 2);
}

TEST_CASE("InputField.multiline_multiple_lines_then_submit")
{
    InputField field;
    field.setMultiline(true);

    // Type "echo a"
    for (char const c: std::string_view("echo a"))
        (void) field.processEvent(charKey(c));

    // Shift+Enter
    auto action = field.processEvent(specialKey(KeyCode::Enter, Modifier::Shift));
    CHECK(action == InputFieldAction::Changed);

    // Type "echo b"
    for (char const c: std::string_view("echo b"))
        (void) field.processEvent(charKey(c));

    // Shift+Enter
    action = field.processEvent(specialKey(KeyCode::Enter, Modifier::Shift));
    CHECK(action == InputFieldAction::Changed);

    // Type "echo c"
    for (char const c: std::string_view("echo c"))
        (void) field.processEvent(charKey(c));

    // Verify full content before submit
    CHECK(field.text() == "echo a\necho b\necho c");
    CHECK(field.lineCount() == 3);

    // Enter (submit)
    action = field.processEvent(specialKey(KeyCode::Enter));
    CHECK(action == InputFieldAction::Submit);

    // After submit, text should still be intact
    CHECK(field.text() == "echo a\necho b\necho c");
}

TEST_CASE("InputField.multiline_max_lines_limit")
{
    InputField field;
    field.setMultiline(true);
    field.setMaxLines(2);
    field.setText("line1");

    // First newline should work
    (void) field.processEvent(specialKey(KeyCode::Enter, Modifier::Alt));
    CHECK(field.lineCount() == 2);

    // Add some text
    (void) field.processEvent(charKey('x'));

    // Second newline should be blocked
    (void) field.processEvent(specialKey(KeyCode::Enter, Modifier::Alt));
    CHECK(field.lineCount() == 2); // Still 2
}

TEST_CASE("InputField.multiline_home_goes_to_line_start")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Cursor at end of "world"
    CHECK(field.cursorLine() == 1);
    CHECK(field.cursorColumn() == 5);

    // Home goes to start of line, not buffer
    (void) field.processEvent(specialKey(KeyCode::Home));
    CHECK(field.cursorLine() == 1);
    CHECK(field.cursorColumn() == 0);
}

TEST_CASE("InputField.multiline_ctrl_home_goes_to_buffer_start")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Ctrl+Home goes to buffer start
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Ctrl));
    CHECK(field.cursorLine() == 0);
    CHECK(field.cursorColumn() == 0);
    CHECK(field.cursor() == 0);
}

// ============================================================================
// History tests
// ============================================================================

TEST_CASE("InputField.history_navigation")
{
    InputField field;
    field.addHistory("first");
    field.addHistory("second");
    field.addHistory("third");

    // Up arrow goes back in history
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "third");

    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "second");

    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "first");

    // Down arrow goes forward
    (void) field.processEvent(specialKey(KeyCode::Down));
    CHECK(field.text() == "second");
}

TEST_CASE("InputField.history_saves_current_line")
{
    InputField field;
    field.addHistory("history1");

    // Type something
    (void) field.processEvent(charKey('t'));
    (void) field.processEvent(charKey('e'));
    (void) field.processEvent(charKey('s'));
    (void) field.processEvent(charKey('t'));
    CHECK(field.text() == "test");

    // Go up to history
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "history1");

    // Go back down - should restore what was typed
    (void) field.processEvent(specialKey(KeyCode::Down));
    CHECK(field.text() == "test");
}

TEST_CASE("InputField.history_no_duplicates")
{
    InputField field;
    field.addHistory("same");
    field.addHistory("same"); // Should be ignored

    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "same");

    // Should not have another "same" before this
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "same"); // Still the same entry
}

// ============================================================================
// Kill ring tests (Emacs-style)
// ============================================================================

TEST_CASE("InputField.kill_to_end_of_line")
{
    InputField field;
    field.setText("hello world");
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Move to middle
    for ([[maybe_unused]] auto const i: std::views::iota(0, 5))
        (void) field.processEvent(specialKey(KeyCode::Right));

    // Ctrl+K kills to end
    (void) field.processEvent(charKey('k', Modifier::Ctrl));
    CHECK(field.text() == "hello");
}

TEST_CASE("InputField.kill_to_start_of_line")
{
    InputField field;
    field.setText("hello world");

    // Move to middle
    (void) field.processEvent(specialKey(KeyCode::Home));
    for ([[maybe_unused]] auto const i: std::views::iota(0, 6))
        (void) field.processEvent(specialKey(KeyCode::Right));

    // Ctrl+U kills to start
    (void) field.processEvent(charKey('u', Modifier::Ctrl));
    CHECK(field.text() == "world");
}

TEST_CASE("InputField.yank")
{
    InputField field;
    // Bind Ctrl+V to Paste explicitly (no default binding)
    field.keyBindings().bind(KeyChord::fromChar('v', Modifier::Ctrl), EditAction::Paste);
    field.setText("hello");

    // Kill the text
    (void) field.processEvent(charKey('u', Modifier::Ctrl));
    CHECK(field.text().empty());

    // Yank it back with Ctrl+V (Paste, which uses kill ring)
    (void) field.processEvent(charKey('v', Modifier::Ctrl));
    CHECK(field.text() == "hello");
}

// ============================================================================
// Submit and abort tests
// ============================================================================

TEST_CASE("InputField.enter_submits")
{
    InputField field;
    field.setText("command");

    auto action = field.processEvent(specialKey(KeyCode::Enter));
    CHECK(action == InputFieldAction::Submit);
}

TEST_CASE("InputField.ctrl_c_copies_without_selection")
{
    InputField field;
    field.setText("something");

    // Ctrl+C without selection does nothing (no abort)
    auto action = field.processEvent(charKey('c', Modifier::Ctrl));
    CHECK(action == InputFieldAction::None);
}

TEST_CASE("InputField.ctrl_d_aborts_on_empty")
{
    InputField field;
    // Empty buffer

    // Ctrl+D on empty buffer returns Eof (abort)
    auto action = field.processEvent(charKey('d', Modifier::Ctrl));
    CHECK(action == InputFieldAction::Eof);
}

TEST_CASE("InputField.ctrl_d_deletes_char_when_not_empty")
{
    InputField field;
    field.setText("hello");

    // Move cursor to beginning
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Ctrl+D on non-empty buffer deletes character at cursor
    auto action = field.processEvent(charKey('d', Modifier::Ctrl));
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.text() == "ello");
}

TEST_CASE("InputField.ctrl_c_copies_when_selection")
{
    InputField field;
    // Bind Ctrl+V to Paste explicitly (no default binding)
    field.keyBindings().bind(KeyChord::fromChar('v', Modifier::Ctrl), EditAction::Paste);
    field.setText("hello");
    (void) field.processEvent(charKey('a', Modifier::Ctrl | Modifier::Shift)); // Select all

    // Ctrl+C with selection should copy, not abort
    auto action = field.processEvent(charKey('c', Modifier::Ctrl));
    CHECK(action == InputFieldAction::Changed);

    // Clear selection and buffer, then paste to verify copy worked
    field.clear();
    (void) field.processEvent(charKey('v', Modifier::Ctrl)); // Paste
    CHECK(field.text() == "hello");
}

TEST_CASE("InputField.ctrl_d_eof_on_empty")
{
    InputField field;
    // Empty buffer

    auto action = field.processEvent(charKey('d', Modifier::Ctrl));
    CHECK(action == InputFieldAction::Eof);
}

TEST_CASE("InputField.ctrl_d_deletes_when_not_empty")
{
    InputField field;
    field.setText("hello");
    (void) field.processEvent(specialKey(KeyCode::Home));

    auto action = field.processEvent(charKey('d', Modifier::Ctrl));
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.text() == "ello");
}

// ============================================================================
// Paste event tests
// ============================================================================

TEST_CASE("InputField.paste_event_inserts_text")
{
    InputField field;
    field.setText("hello ");

    PasteEvent paste { .text = "world" };
    auto action = field.processEvent(paste);

    CHECK(action == InputFieldAction::Changed);
    CHECK(field.text() == "hello world");
}

TEST_CASE("InputField.paste_replaces_selection")
{
    InputField field;
    field.setText("hello");
    (void) field.processEvent(charKey('a', Modifier::Ctrl | Modifier::Shift)); // Select all

    PasteEvent paste { .text = "goodbye" };
    (void) field.processEvent(paste);

    CHECK(field.text() == "goodbye");
}

// ============================================================================
// Click-to-position tests
// ============================================================================

TEST_CASE("InputField.setCursorFromClick_single_line")
{
    InputField field;
    field.setText("hello world");

    field.setCursorFromClick(0, 5);
    CHECK(field.cursor() == 5);
    CHECK_FALSE(field.hasSelection());
}

TEST_CASE("InputField.setCursorFromClick_multiline")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    field.setCursorFromClick(1, 2);
    CHECK(field.cursorLine() == 1);
    CHECK(field.cursorColumn() == 2);
}

TEST_CASE("InputField.setCursorFromClick_extends_selection")
{
    InputField field;
    field.setText("hello world");
    (void) field.processEvent(specialKey(KeyCode::Home));

    // Click with extend selection
    field.setCursorFromClick(0, 5, true);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hello");
}

// ============================================================================
// Transpose test
// ============================================================================

TEST_CASE("InputField.transpose_characters")
{
    InputField field;
    // Ctrl+T defaults to AgentMode; bind it to Transpose for this test
    field.keyBindings().bind(KeyChord::fromChar('t', Modifier::Ctrl), EditAction::Transpose);
    field.setText("ab");

    // Ctrl+T transposes characters before cursor
    (void) field.processEvent(charKey('t', Modifier::Ctrl));
    CHECK(field.text() == "ba");
}

// ============================================================================
// UTF-8 tests
// ============================================================================

TEST_CASE("InputField.utf8_characters")
{
    InputField field;

    // Insert UTF-8 characters - e with acute (U+00E9)
    (void) field.processEvent(KeyEvent {
        .key = keyCodeFromCodepoint(U'\u00E9'), .modifiers = Modifier::None, .codepoint = U'\u00E9' });
    // e with grave (U+00E8)
    (void) field.processEvent(KeyEvent {
        .key = keyCodeFromCodepoint(U'\u00E8'), .modifiers = Modifier::None, .codepoint = U'\u00E8' });

    // Should have two grapheme clusters
    CHECK(field.text() == "\xC3\xA9\xC3\xA8"); // UTF-8 encoding of e with acute + e with grave
}

// ============================================================================
// Mouse handling tests
// ============================================================================

TEST_CASE("InputField.mouse_click_positions_cursor")
{
    InputField field;
    field.setText("hello world");

    // Single click at column 5 should position cursor there
    auto action = field.handleMouse(MouseEvent::Type::Press, 0, 5, Modifier::None);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.cursor() == 5);
    CHECK(!field.hasSelection());
}

TEST_CASE("InputField.mouse_click_with_shift_extends_selection")
{
    InputField field;
    field.setText("hello world");
    (void) field.processEvent(specialKey(KeyCode::Home)); // Cursor at start

    // Shift+click at column 5 should create selection
    auto action = field.handleMouse(MouseEvent::Type::Press, 0, 5, Modifier::Shift);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hello");
}

TEST_CASE("InputField.mouse_drag_creates_selection")
{
    InputField field;
    field.setText("hello world");

    // Press at column 0
    (void) field.handleMouse(MouseEvent::Type::Press, 0, 0, Modifier::None);
    CHECK(!field.hasSelection());

    // Drag to column 5
    auto action = field.handleMouse(MouseEvent::Type::Move, 0, 5, Modifier::None);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hello");

    // Release
    action = field.handleMouse(MouseEvent::Type::Release, 0, 5, Modifier::None);
    CHECK(action == InputFieldAction::None);
    CHECK(field.hasSelection()); // Selection remains
}

TEST_CASE("InputField.double_click_selects_word")
{
    auto const clock = ManualClock {}; // Frozen: every click lands inside the multi-click window.
    auto field = InputField { clock };
    field.setText("hello world");

    // First click
    (void) field.handleMouse(MouseEvent::Type::Press, 0, 7, Modifier::None);
    (void) field.handleMouse(MouseEvent::Type::Release, 0, 7, Modifier::None);

    // Second click (double-click) - should select "world"
    auto action = field.handleMouse(MouseEvent::Type::Press, 0, 7, Modifier::None);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "world");
}

TEST_CASE("InputField.double_click_on_path_selects_component")
{
    auto const clock = ManualClock {}; // Frozen: every click lands inside the multi-click window.
    auto field = InputField { clock };
    field.setText("/home/user/file.txt");

    // First click on "user"
    (void) field.handleMouse(MouseEvent::Type::Press, 0, 7, Modifier::None);
    (void) field.handleMouse(MouseEvent::Type::Release, 0, 7, Modifier::None);

    // Second click (double-click) - should select "user" not whole path
    auto action = field.handleMouse(MouseEvent::Type::Press, 0, 7, Modifier::None);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "user");
}

TEST_CASE("InputField.triple_click_selects_line")
{
    auto const clock = ManualClock {}; // Frozen: every click lands inside the multi-click window.
    auto field = InputField { clock };
    field.setMultiline(true);
    field.setText("first line\nsecond line\nthird line");

    // First click on second line
    (void) field.handleMouse(MouseEvent::Type::Press, 1, 3, Modifier::None);
    (void) field.handleMouse(MouseEvent::Type::Release, 1, 3, Modifier::None);

    // Second click
    (void) field.handleMouse(MouseEvent::Type::Press, 1, 3, Modifier::None);
    (void) field.handleMouse(MouseEvent::Type::Release, 1, 3, Modifier::None);

    // Third click (triple-click) - should select entire second line
    auto action = field.handleMouse(MouseEvent::Type::Press, 1, 3, Modifier::None);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "second line\n");
}

TEST_CASE("InputField.click_count_resets_after_moving_away")
{
    auto const clock = ManualClock {};
    auto field = InputField { clock };
    field.setText("hello world");

    // First click
    (void) field.handleMouse(MouseEvent::Type::Press, 0, 5, Modifier::None);
    (void) field.handleMouse(MouseEvent::Type::Release, 0, 5, Modifier::None);
    CHECK(!field.hasSelection());

    // A click at a different position, no time later, starts a new count.
    (void) field.handleMouse(MouseEvent::Type::Press, 0, 0, Modifier::None);
    CHECK(!field.hasSelection()); // Single click, no selection
}

TEST_CASE("InputField.double_click_is_timed_on_the_injected_clock")
{
    auto clock = ManualClock {};
    auto field = InputField { clock };
    field.setText("hello world");
    auto const click = [&field] {
        auto const action = field.handleMouse(MouseEvent::Type::Press, 0, 7, Modifier::None);
        (void) field.handleMouse(MouseEvent::Type::Release, 0, 7, Modifier::None);
        return action;
    };

    SECTION("a second click 399 ms later is a double-click")
    {
        (void) click();
        clock.advance(std::chrono::milliseconds(399));
        CHECK(click() == InputFieldAction::Changed);
        CHECK(field.selectedText() == "world");
    }

    SECTION("a second click 400 ms later is a new single click")
    {
        (void) click();
        clock.advance(std::chrono::milliseconds(400));
        (void) click();
        CHECK_FALSE(field.hasSelection());
    }

    SECTION("a third click inside the window of the second is a triple-click")
    {
        field.setMultiline(true);
        field.setText("first line\nsecond line");
        auto const press = [&field] {
            auto const action = field.handleMouse(MouseEvent::Type::Press, 1, 3, Modifier::None);
            (void) field.handleMouse(MouseEvent::Type::Release, 1, 3, Modifier::None);
            return action;
        };
        (void) press();
        clock.advance(std::chrono::milliseconds(300));
        (void) press();
        clock.advance(std::chrono::milliseconds(300)); // 600 ms after the first, 300 after the second
        CHECK(press() == InputFieldAction::Changed);
        CHECK(field.selectedText() == "second line");
    }
}

TEST_CASE("InputField.select_word_fish_style")
{
    InputField field;
    field.setText("hello-world.txt");

    // Select word at position 0 (should select "hello")
    field.selectWord(0);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "hello");

    // Clear and select word at position 6 (on "-", should select just "-")
    field.clearSelection();
    field.selectWord(5); // The hyphen position
    CHECK(field.hasSelection());
    // Since hyphen is not a word char, we select just the character
    CHECK(field.selectedText() == "-");
}

TEST_CASE("InputField.select_line")
{
    InputField field;
    field.setMultiline(true);
    field.setText("line one\nline two\nline three");

    // Select line 1 (second line)
    field.selectLine(1);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "line two\n");

    // Select line 0 (first line)
    field.selectLine(0);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "line one\n");

    // Select last line (no trailing newline)
    field.selectLine(2);
    CHECK(field.hasSelection());
    CHECK(field.selectedText() == "line three");
}

TEST_CASE("InputField.scroll_by")
{
    InputField field;
    field.setMultiline(true);
    field.setText("line1\nline2\nline3\nline4\nline5");

    CHECK(field.scrollOffset() == 0);

    // Scroll down
    field.scrollBy(2);
    CHECK(field.scrollOffset() == 2);

    // Scroll up
    field.scrollBy(-1);
    CHECK(field.scrollOffset() == 1);

    // Don't scroll past beginning
    field.scrollBy(-10);
    CHECK(field.scrollOffset() == 0);

    // Don't scroll past end (4 lines means max offset of 4)
    field.scrollBy(100);
    CHECK(field.scrollOffset() == 4); // 5 lines, max offset is 4
}

// ============================================================================
// CapsLock and Shift capitalization tests (Kitty keyboard protocol)
// ============================================================================

TEST_CASE("InputField.shift_capitalizes_letters")
{
    InputField field;

    // Shift+a should produce 'A' (Kitty sends lowercase codepoint + Shift modifier)
    (void) field.processEvent(charKey('a', Modifier::Shift));
    CHECK(field.text() == "A");

    field.clear();

    // Shift+z should produce 'Z'
    (void) field.processEvent(charKey('z', Modifier::Shift));
    CHECK(field.text() == "Z");
}

TEST_CASE("InputField.capslock_capitalizes_letters")
{
    InputField field;

    // CapsLock active + 'a' should produce 'A'
    (void) field.processEvent(charKey('a', Modifier::CapsLock));
    CHECK(field.text() == "A");

    field.clear();

    // CapsLock active + 'z' should produce 'Z'
    (void) field.processEvent(charKey('z', Modifier::CapsLock));
    CHECK(field.text() == "Z");
}

TEST_CASE("InputField.capslock_plus_shift_produces_lowercase")
{
    InputField field;

    // CapsLock + Shift should cancel out (XOR behavior) - produces lowercase
    (void) field.processEvent(charKey('a', Modifier::CapsLock | Modifier::Shift));
    CHECK(field.text() == "a");

    field.clear();

    (void) field.processEvent(charKey('z', Modifier::CapsLock | Modifier::Shift));
    CHECK(field.text() == "z");
}

TEST_CASE("InputField.capslock_does_not_affect_numbers")
{
    InputField field;

    // CapsLock should not affect non-letter characters
    (void) field.processEvent(charKey('1', Modifier::CapsLock));
    CHECK(field.text() == "1");

    (void) field.processEvent(charKey('!', Modifier::CapsLock));
    CHECK(field.text() == "1!");

    (void) field.processEvent(charKey('@', Modifier::CapsLock));
    CHECK(field.text() == "1!@");
}

TEST_CASE("InputField.capslock_does_not_affect_symbols")
{
    InputField field;

    // CapsLock should not affect symbols
    (void) field.processEvent(charKey('-', Modifier::CapsLock));
    (void) field.processEvent(charKey('=', Modifier::CapsLock));
    (void) field.processEvent(charKey('[', Modifier::CapsLock));
    CHECK(field.text() == "-=[");
}

TEST_CASE("InputField.mixed_capslock_typing")
{
    InputField field;

    // Simulate typing "Hello" with CapsLock on, using Shift for lowercase 'e' and 'o'
    (void) field.processEvent(charKey('h', Modifier::CapsLock));                   // H
    (void) field.processEvent(charKey('e', Modifier::CapsLock | Modifier::Shift)); // e (shift cancels caps)
    (void) field.processEvent(charKey('l', Modifier::CapsLock));                   // L
    (void) field.processEvent(charKey('l', Modifier::CapsLock));                   // L
    (void) field.processEvent(charKey('o', Modifier::CapsLock | Modifier::Shift)); // o (shift cancels caps)

    CHECK(field.text() == "HeLLo");
}

// ============================================================================
// Smart cursor movement tests (Ctrl+A / Ctrl+E)
// ============================================================================

TEST_CASE("InputField.smart_move_to_line_start_single_line")
{
    InputField field;
    field.setText("hello");

    // Ctrl+A goes to position 0 in single-line mode
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 0);
}

TEST_CASE("InputField.smart_move_to_line_start_multiline_from_middle")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Cursor at end of "world" (position 11)
    CHECK(field.cursor() == 11);

    // First Ctrl+A goes to start of current line ("world" starts at 6)
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 6);
}

TEST_CASE("InputField.smart_move_to_line_start_multiline_already_at_start")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Move to start of second line
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 6);

    // Second Ctrl+A jumps to start of previous line
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 0);
}

TEST_CASE("InputField.smart_move_to_line_start_already_at_buffer_start")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Move to buffer start
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Ctrl));
    CHECK(field.cursor() == 0);

    // Ctrl+A stays at 0 (nowhere to go)
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 0);
}

TEST_CASE("InputField.smart_move_to_line_end_single_line")
{
    InputField field;
    field.setText("hello");

    // Move to start first
    (void) field.processEvent(specialKey(KeyCode::Home));
    CHECK(field.cursor() == 0);

    // Ctrl+E goes to buffer end in single-line mode
    (void) field.processEvent(charKey('e', Modifier::Ctrl));
    CHECK(field.cursor() == 5);
}

TEST_CASE("InputField.smart_move_to_line_end_multiline_from_middle")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Move to buffer start
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Ctrl));
    CHECK(field.cursor() == 0);

    // Move to middle of first line
    (void) field.processEvent(specialKey(KeyCode::Right));
    (void) field.processEvent(specialKey(KeyCode::Right));
    CHECK(field.cursor() == 2);

    // First Ctrl+E goes to end of current line ("hello" ends at 5)
    (void) field.processEvent(charKey('e', Modifier::Ctrl));
    CHECK(field.cursor() == 5);
}

TEST_CASE("InputField.smart_move_to_line_end_multiline_already_at_end")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Move to buffer start, then to end of first line
    (void) field.processEvent(specialKey(KeyCode::Home, Modifier::Ctrl));
    (void) field.processEvent(charKey('e', Modifier::Ctrl));
    CHECK(field.cursor() == 5);

    // Second Ctrl+E jumps to end of next line
    (void) field.processEvent(charKey('e', Modifier::Ctrl));
    CHECK(field.cursor() == 11);
}

TEST_CASE("InputField.smart_move_to_line_end_already_at_buffer_end")
{
    InputField field;
    field.setMultiline(true);
    field.setText("hello\nworld");

    // Cursor already at buffer end (11)
    CHECK(field.cursor() == 11);

    // Ctrl+E stays at end (nowhere to go)
    (void) field.processEvent(charKey('e', Modifier::Ctrl));
    CHECK(field.cursor() == 11);
}

TEST_CASE("InputField.smart_move_three_lines")
{
    InputField field;
    field.setMultiline(true);
    field.setText("aaa\nbbb\nccc");

    // Cursor at end of "ccc" (position 11)
    CHECK(field.cursor() == 11);

    // Ctrl+A from end of third line -> start of third line (8)
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 8);

    // Ctrl+A again -> start of second line (4)
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 4);

    // Ctrl+A again -> start of first line (0)
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 0);

    // Ctrl+A again -> stays at 0
    (void) field.processEvent(charKey('a', Modifier::Ctrl));
    CHECK(field.cursor() == 0);
}

// ============================================================================
// lineStartOffset tests
// ============================================================================

TEST_CASE("InputField.lineStartOffset_single_line")
{
    InputField field;
    field.setText("hello");
    CHECK(field.lineStartOffset(0) == 0);
}

TEST_CASE("InputField.lineStartOffset_multiline")
{
    InputField field;
    field.setMultiline(true);
    field.setText("aaa\nbbb\nccc");

    CHECK(field.lineStartOffset(0) == 0); // "aaa"
    CHECK(field.lineStartOffset(1) == 4); // "bbb" (after "aaa\n")
    CHECK(field.lineStartOffset(2) == 8); // "ccc" (after "aaa\nbbb\n")
}

TEST_CASE("InputField.lineStartOffset_empty_lines")
{
    InputField field;
    field.setMultiline(true);
    field.setText("a\n\nc");

    CHECK(field.lineStartOffset(0) == 0); // "a"
    CHECK(field.lineStartOffset(1) == 2); // "" (empty line)
    CHECK(field.lineStartOffset(2) == 3); // "c"
}

// ============================================================================
// Multiline rendering tests
// ============================================================================

TEST_CASE("InputField.multiline_render_basic")
{
    // Verify that multiline render puts content on the correct rows
    InputField field;
    field.setMultiline(true);
    field.setText("aaa\nbbb");

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Row 0 should contain "aaa", row 1 should contain "bbb"
    CHECK(requireCell(buf, { .x = 0, .y = 0 }).grapheme == "a");
    CHECK(requireCell(buf, { .x = 1, .y = 0 }).grapheme == "a");
    CHECK(requireCell(buf, { .x = 2, .y = 0 }).grapheme == "a");
    CHECK(requireCell(buf, { .x = 0, .y = 1 }).grapheme == "b");
    CHECK(requireCell(buf, { .x = 1, .y = 1 }).grapheme == "b");
    CHECK(requireCell(buf, { .x = 2, .y = 1 }).grapheme == "b");
}

TEST_CASE("InputField.multiline_render_with_prompt")
{
    InputField field;
    field.setMultiline(true);
    field.setPrompt("> ");
    field.setContinuationPrompt("  ");
    field.setText("aaa\nbbb");

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Row 0: "> " prompt then "aaa"
    CHECK(requireCell(buf, { .x = 0, .y = 0 }).grapheme == ">");
    CHECK(requireCell(buf, { .x = 1, .y = 0 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 2, .y = 0 }).grapheme == "a");
    // Row 1: "  " continuation then "bbb"
    CHECK(requireCell(buf, { .x = 0, .y = 1 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 1, .y = 1 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 2, .y = 1 }).grapheme == "b");
}

TEST_CASE("InputField.multiline_render_ghost_text_on_last_line")
{
    InputField field;
    field.setMultiline(true);
    field.setText("aaa\nbbb");
    field.setGhostText("ccc");

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Ghost text appears after "bbb" on row 1 (last line)
    CHECK(requireCell(buf, { .x = 3, .y = 1 }).grapheme == "c");
    CHECK(requireCell(buf, { .x = 4, .y = 1 }).grapheme == "c");
    CHECK(requireCell(buf, { .x = 5, .y = 1 }).grapheme == "c");
}

TEST_CASE("InputField.multiline_continuation_prompt")
{
    InputField field;
    field.setMultiline(true);
    field.setContinuationPrompt(".. ");
    CHECK(field.continuationPrompt() == ".. ");
}

// ============================================================================
// TextDecorator tests
// ============================================================================

TEST_CASE("InputField.multiline_auto_continuation_alignment")
{
    // When no continuation prompt is set, continuation lines should auto-align
    // to the same column as the first line's text (spaces matching prompt width).
    InputField field;
    field.setMultiline(true);
    field.setPrompt("> "); // 2-column prompt, NO setContinuationPrompt call
    field.setText("aaa\nbbb\nccc");

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Row 0: "> aaa" — text starts at col 2
    CHECK(requireCell(buf, { .x = 0, .y = 0 }).grapheme == ">");
    CHECK(requireCell(buf, { .x = 2, .y = 0 }).grapheme == "a");
    // Row 1: "  bbb" — auto-continuation: 2 spaces, text at col 2
    CHECK(requireCell(buf, { .x = 0, .y = 1 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 1, .y = 1 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 2, .y = 1 }).grapheme == "b");
    // Row 2: "  ccc" — same alignment
    CHECK(requireCell(buf, { .x = 0, .y = 2 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 1, .y = 2 }).grapheme == " ");
    CHECK(requireCell(buf, { .x = 2, .y = 2 }).grapheme == "c");
}

TEST_CASE("InputField.decorator_foreground_applied")
{
    // Simple decorator that colors the first grapheme red
    struct TestDecorator: TextDecorator
    {
        mutable std::vector<std::size_t> queriedIndices;

        [[nodiscard]] auto foreground(TextPosition pos) const -> std::optional<RgbColor> override
        {
            queriedIndices.push_back(pos.graphemeIndex);
            if (pos.graphemeIndex == 0)
                return RgbColor { .r = 255, .g = 0, .b = 0 };
            return {};
        }
    };

    InputField field;
    field.setText("abc");

    TestDecorator decorator;
    field.setTextDecorator(&decorator);

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 1 });
    field.render(canvas);

    // Decorator should be queried for each grapheme (a, b, c)
    REQUIRE(decorator.queriedIndices.size() == 3);
    CHECK(decorator.queriedIndices[0] == 0);
    CHECK(decorator.queriedIndices[1] == 1);
    CHECK(decorator.queriedIndices[2] == 2);

    // First grapheme should have red foreground
    CHECK(std::holds_alternative<RgbColor>(requireCell(buf, { .x = 0, .y = 0 }).style.fg));
    auto const& fg = std::get<RgbColor>(requireCell(buf, { .x = 0, .y = 0 }).style.fg);
    CHECK(fg.r == 255);
    CHECK(fg.g == 0);
    CHECK(fg.b == 0);
}

TEST_CASE("InputField.decorator_background_applied")
{
    struct TestDecorator: TextDecorator
    {
        mutable int callCount = 0;

        [[nodiscard]] auto background(int /*displayCol*/) const -> std::optional<RgbColor> override
        {
            ++callCount;
            return RgbColor { .r = 42, .g = 42, .b = 42 };
        }
    };

    InputField field;
    field.setText("ab");

    TestDecorator decorator;
    field.setTextDecorator(&decorator);

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 1 });
    field.render(canvas);

    // Background decorator should be called during rendering
    CHECK(decorator.callCount > 0);

    // Text cells should have the decorator background
    CHECK(std::holds_alternative<RgbColor>(requireCell(buf, { .x = 0, .y = 0 }).style.bg));
    auto const& bg = std::get<RgbColor>(requireCell(buf, { .x = 0, .y = 0 }).style.bg);
    CHECK(bg.r == 42);
}

TEST_CASE("InputField.decorator_cleared_with_nullptr")
{
    struct TestDecorator: TextDecorator
    {
        mutable int callCount = 0;

        [[nodiscard]] auto foreground(TextPosition /*pos*/) const -> std::optional<RgbColor> override
        {
            ++callCount;
            return RgbColor { .r = 255, .g = 0, .b = 0 };
        }
    };

    InputField field;
    field.setText("a");

    TestDecorator decorator;
    field.setTextDecorator(&decorator);
    field.setTextDecorator(nullptr); // Clear decorator

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 1 });
    field.render(canvas);

    // Decorator should NOT be called after being cleared
    CHECK(decorator.callCount == 0);
}

TEST_CASE("InputField.decorator_multiline_global_grapheme_index")
{
    // Verify that the decorator receives global (buffer-wide) grapheme indices in multiline mode
    struct TestDecorator: TextDecorator
    {
        mutable std::vector<std::size_t> queriedIndices;

        [[nodiscard]] auto foreground(TextPosition pos) const -> std::optional<RgbColor> override
        {
            queriedIndices.push_back(pos.graphemeIndex);
            return {};
        }
    };

    InputField field;
    field.setMultiline(true);
    field.setText("ab\ncd"); // 2 graphemes + newline + 2 graphemes = indices 0,1,(2=newline),3,4

    TestDecorator decorator;
    field.setTextDecorator(&decorator);

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Should have queried indices for a(0), b(1), c(3), d(4) — index 2 is the newline (skipped)
    REQUIRE(decorator.queriedIndices.size() == 4);
    CHECK(decorator.queriedIndices[0] == 0); // 'a'
    CHECK(decorator.queriedIndices[1] == 1); // 'b'
    CHECK(decorator.queriedIndices[2] == 3); // 'c' (index 2 = newline)
    CHECK(decorator.queriedIndices[3] == 4); // 'd'
}

TEST_CASE("InputField.ghost_text_decorator_background_per_column")
{
    // A gradient decorator that returns a different background per column.
    struct GradientDecorator: TextDecorator
    {
        mutable std::vector<int> queriedColumns;

        [[nodiscard]] auto background(int displayCol) const -> std::optional<RgbColor> override
        {
            queriedColumns.push_back(displayCol);
            return RgbColor { .r = static_cast<uint8_t>(displayCol * 10),
                              .g = static_cast<uint8_t>(displayCol * 20),
                              .b = static_cast<uint8_t>(displayCol * 30) };
        }
    };

    InputField field;
    field.setText("ab");
    field.setGhostText("xyz");

    GradientDecorator decorator;
    field.setTextDecorator(&decorator);

    Buffer buf(1, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 1 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 1 });
    field.render(canvas);

    // Ghost text starts at column 2 (after "ab").
    // Each ghost character should have its own per-column background.
    auto const& bgX = std::get<RgbColor>(requireCell(buf, { .x = 2, .y = 0 }).style.bg);
    auto const& bgY = std::get<RgbColor>(requireCell(buf, { .x = 3, .y = 0 }).style.bg);
    auto const& bgZ = std::get<RgbColor>(requireCell(buf, { .x = 4, .y = 0 }).style.bg);

    // Verify each ghost char got a distinct background matching its column
    CHECK(bgX.r == 20); // col 2 * 10
    CHECK(bgY.r == 30); // col 3 * 10
    CHECK(bgZ.r == 40); // col 4 * 10

    CHECK(bgX.g == 40); // col 2 * 20
    CHECK(bgY.g == 60); // col 3 * 20
    CHECK(bgZ.g == 80); // col 4 * 20

    CHECK(bgX.b == 60);  // col 2 * 30
    CHECK(bgY.b == 90);  // col 3 * 30
    CHECK(bgZ.b == 120); // col 4 * 30
}

TEST_CASE("InputField.multiline_ghost_text_decorator_background_per_column")
{
    struct GradientDecorator: TextDecorator
    {
        [[nodiscard]] auto background(int displayCol) const -> std::optional<RgbColor> override
        {
            return RgbColor { .r = static_cast<uint8_t>(displayCol * 10),
                              .g = static_cast<uint8_t>(displayCol * 20),
                              .b = static_cast<uint8_t>(displayCol * 30) };
        }
    };

    InputField field;
    field.setMultiline(true);
    field.setText("aaa\nbbb");
    field.setGhostText("xy");

    GradientDecorator decorator;
    field.setTextDecorator(&decorator);

    Buffer buf(10, 40);
    auto theme = darkTheme();
    Canvas canvas(buf, Rect { .x = 0, .y = 0, .width = 40, .height = 10 }, theme);
    field.setArea(Rect { .x = 0, .y = 0, .width = 40, .height = 5 });
    field.render(canvas);

    // Ghost text on row 1 (last line), starting at column 3 (after "bbb").
    auto const& bgX = std::get<RgbColor>(requireCell(buf, { .x = 3, .y = 1 }).style.bg);
    auto const& bgY = std::get<RgbColor>(requireCell(buf, { .x = 4, .y = 1 }).style.bg);

    // Each ghost char should get its own column's gradient background
    CHECK(bgX.r == 30); // col 3 * 10
    CHECK(bgY.r == 40); // col 4 * 10

    CHECK(bgX.g == 60); // col 3 * 20
    CHECK(bgY.g == 80); // col 4 * 20

    CHECK(bgX.b == 90);  // col 3 * 30
    CHECK(bgY.b == 120); // col 4 * 30
}

// ============================================================================
// Newline sanitization tests (single-line mode)
// ============================================================================

TEST_CASE("InputField.setText_strips_newlines_in_single_line_mode")
{
    InputField field;
    // Single-line mode is the default
    CHECK_FALSE(field.isMultiline());

    field.setText("hello\nworld\nfoo");
    CHECK(field.text() == "hello world foo");
    CHECK(field.cursor() == 15);
}

TEST_CASE("InputField.setText_preserves_newlines_in_multiline_mode")
{
    InputField field;
    field.setMultiline(true);

    field.setText("hello\nworld\nfoo");
    CHECK(field.text() == "hello\nworld\nfoo");
    CHECK(field.lineCount() == 3);
}

TEST_CASE("InputField.setGhostText_truncates_at_newline")
{
    InputField field;
    field.setGhostText("first line\nsecond line\nthird");
    CHECK(field.ghostText() == "first line");
}

TEST_CASE("InputField.paste_strips_newlines_in_single_line_mode")
{
    InputField field;
    CHECK_FALSE(field.isMultiline());
    field.setText("prefix ");

    PasteEvent paste { .text = "line1\nline2\rline3" };
    auto action = field.processEvent(paste);
    CHECK(action == InputFieldAction::Changed);
    CHECK(field.text() == "prefix line1 line2line3");
}

TEST_CASE("InputField.history_cycling_strips_newlines_in_single_line_mode")
{
    InputField field;
    CHECK_FALSE(field.isMultiline());

    field.addHistory("single line");
    field.addHistory("multi\nline\nentry");

    // Up arrow: most recent history entry (multi-line)
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "multi line entry");

    // Up arrow: older entry (single-line, unchanged)
    (void) field.processEvent(specialKey(KeyCode::Up));
    CHECK(field.text() == "single line");

    // Down arrow: back to multi-line entry (sanitized)
    (void) field.processEvent(specialKey(KeyCode::Down));
    CHECK(field.text() == "multi line entry");
}

// ============================================================================
// Ghost text trimming tests
// ============================================================================

TEST_CASE("InputField.ghost_text_trimmed_on_matching_char")
{
    InputField field;
    field.setText("foo");
    field.setCursor(3);
    field.setGhostText("bar");
    (void) field.processEvent(charKey('b'));
    CHECK(field.text() == "foob");
    CHECK(field.ghostText() == "ar");
}

TEST_CASE("InputField.ghost_text_cleared_on_non_matching_char")
{
    InputField field;
    field.setText("foo");
    field.setCursor(3);
    field.setGhostText("bar");
    (void) field.processEvent(charKey('x'));
    CHECK(field.text() == "foox");
    CHECK(field.ghostText().empty());
}

TEST_CASE("InputField.ghost_text_cleared_when_cursor_not_at_end")
{
    InputField field;
    field.setText("foo");
    field.setCursor(3);
    field.setGhostText("bar");
    // Move cursor to start
    (void) field.processEvent(specialKey(KeyCode::Home));
    (void) field.processEvent(charKey('x'));
    CHECK(field.ghostText().empty());
}

TEST_CASE("InputField.ghost_text_trim_respects_capitalization")
{
    InputField field;
    field.setText("H");
    field.setCursor(1);
    field.setGhostText("ello");
    // Type 'e' with Shift → becomes 'E', does not match ghost text starting with 'e'
    (void) field.processEvent(charKey('e', Modifier::Shift));
    CHECK(field.text() == "HE");
    CHECK(field.ghostText().empty());
}

// ============================================================================
// Ghost text deletion tests (re-prepend to avoid flicker on backspace/kill)
// ============================================================================

TEST_CASE("InputField.ghost_text_reprepended_on_backspace")
{
    InputField field;
    field.setText("git ch");
    field.setCursor(6);
    field.setGhostText("eckout");
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "git c");
    CHECK(field.ghostText() == "heckout"); // deleted 'h' re-prepended onto ghost
}

TEST_CASE("InputField.ghost_text_reprepended_on_word_backward")
{
    InputField field;
    field.setText("git checkout");
    field.setCursor(12);
    field.setGhostText(" main");
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // DeleteWordBackward
    CHECK(field.text() == "git ");
    CHECK(field.ghostText() == "checkout main"); // whole killed word re-prepended (text remains)
}

TEST_CASE("InputField.ghost_text_cleared_on_word_backward_empties_buffer")
{
    InputField field;
    field.setText("hello");
    field.setCursor(5);
    field.setGhostText(" world");
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // DeleteWordBackward removes the only word
    CHECK(field.text().empty());
    CHECK(field.ghostText().empty()); // emptied buffer: ghost cleared, not resurrected
}

TEST_CASE("InputField.ghost_text_cleared_on_kill_to_start_empties_buffer")
{
    InputField field;
    field.setText("hello");
    field.setCursor(5);
    field.setGhostText(" world");
    (void) field.processEvent(charKey('u', Modifier::Ctrl)); // KillToStart empties the buffer
    CHECK(field.text().empty());
    CHECK(field.ghostText().empty()); // Ctrl-U clears the line — do not resurrect it as a ghost
}

TEST_CASE("InputField.ghost_text_cleared_on_kill_to_end_at_end")
{
    InputField field;
    field.setText("hello");
    field.setCursor(5);
    field.setGhostText(" world");
    (void) field.processEvent(charKey('k', Modifier::Ctrl)); // KillToEnd at end: nothing deleted
    CHECK(field.text() == "hello");
    CHECK(field.ghostText().empty()); // forward kill clears the now-stale ghost
}

TEST_CASE("InputField.ghost_text_cleared_on_delete_word_forward_at_end")
{
    InputField field;
    field.setText("git");
    field.setCursor(3);
    field.setGhostText(" status");
    (void) field.processEvent(charKey('d', Modifier::Alt)); // DeleteWord (forward): no word ahead
    CHECK(field.text() == "git");
    CHECK(field.ghostText().empty()); // forward word kill clears the now-stale ghost
}

TEST_CASE("InputField.ghost_text_cleared_on_undo_after_backspace")
{
    InputField field;
    field.setText("git ch");
    field.setCursor(6);
    field.setGhostText("eckout");
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // -> "git c", ghost "heckout"
    (void) field.processEvent(charKey('z', Modifier::Ctrl));   // Undo
    CHECK(field.text() == "git ch");
    CHECK(field.ghostText().empty()); // undo drops the stale ghost (no "git chheckout")
}

TEST_CASE("InputField.ghost_text_cleared_on_redo")
{
    InputField field;
    field.setText("git ch");
    field.setCursor(6);
    field.setGhostText("eckout");
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // -> "git c"
    (void) field.processEvent(charKey('z', Modifier::Ctrl));   // Undo -> "git ch"
    (void) field.processEvent(charKey('y', Modifier::Ctrl));   // Redo -> "git c"
    CHECK(field.text() == "git c");
    CHECK(field.ghostText().empty()); // redo also drops any stale ghost
}

TEST_CASE("InputField.ghost_text_no_newline_after_backspace_multiline")
{
    InputField field;
    field.setMultiline(true);
    field.setText("a\n");
    field.setCursor(2); // at end, on the (empty) second line
    field.setGhostText("b");
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // deletes the trailing '\n'
    CHECK(field.text() == "a");
    CHECK(field.ghostText() == "b");               // newline not prepended
    CHECK_FALSE(field.ghostText().contains('\n')); // ghost stays single-line
}

TEST_CASE("InputField.ghost_text_prepend_noop_on_empty_ghost")
{
    InputField field;
    field.setText("abc");
    field.setCursor(3); // at end, but no ghost set
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "ab");
    CHECK(field.ghostText().empty()); // never invent a suggestion that wasn't showing
}

TEST_CASE("InputField.ghost_text_restored_on_backspace_after_typing_last_suggestion_char")
{
    // First-deletion flicker: typing the final char of a suggestion trims the ghost to empty,
    // so an immediate backspace has no live ghost to re-prepend onto. The consumed-prefix memory
    // restores the suggestion the debounce would otherwise recompute 100ms later.
    InputField field;
    field.setText("git checkou");
    field.setCursor(11);
    field.setGhostText("t"); // suggestion "git checkout", only the final 't' remains as ghost
    (void) field.processEvent(charKey('t'));
    CHECK(field.text() == "git checkout");
    CHECK(field.ghostText().empty()); // typed the last char: ghost trimmed to empty
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "git checkou");
    CHECK(field.ghostText() == "t"); // suggestion restored without waiting for the debounce
}

TEST_CASE("InputField.ghost_text_restored_on_backspace_after_partial_trim")
{
    // Typing a matching char that only partially trims the ghost, then backspacing it, restores
    // the original suggestion exactly (consumed memory popped back onto the live ghost).
    InputField field;
    field.setText("git che");
    field.setCursor(7);
    field.setGhostText("ckout");
    (void) field.processEvent(charKey('c'));
    CHECK(field.ghostText() == "kout");
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "git che");
    CHECK(field.ghostText() == "ckout");
}

TEST_CASE("InputField.ghost_text_consumed_memory_not_resurrected_after_nonmatching_char")
{
    // A non-matching keystroke clears both the ghost and the consumed memory, so a later backspace
    // must not resurrect a stale suggestion the completer never offered for this text.
    InputField field;
    field.setText("git checkou");
    field.setCursor(11);
    field.setGhostText("t");
    (void) field.processEvent(charKey('t')); // consumes 't', ghost empty
    (void) field.processEvent(charKey('x')); // non-matching: clears consumed memory
    CHECK(field.text() == "git checkoutx");
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // deletes 'x'
    CHECK(field.text() == "git checkout");
    CHECK(field.ghostText().empty()); // no stale resurrection
}

TEST_CASE("InputField.ghost_text_consumed_memory_reset_by_new_suggestion")
{
    // A fresh suggestion (debounce recompute) supersedes the consumed memory: backspacing a char
    // re-prepends onto the new ghost via the normal path, never the stale consumed run.
    InputField field;
    field.setText("git checkou");
    field.setCursor(11);
    field.setGhostText("t");
    (void) field.processEvent(charKey('t')); // consumes 't', ghost empty, buffer "git checkout"
    field.setGhostText(" main");             // debounce recompute for "git checkout"
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "git checkou");
    CHECK(field.ghostText() == "t main"); // deleted 't' re-prepended onto the fresh suggestion
}

TEST_CASE("InputField.ghost_text_restored_on_word_backspace_after_accept")
{
    // Accept the whole suggestion (Ctrl+E etc.), then word-backward delete the last word: the
    // word must reappear as ghost synchronously, no debounce gap. acceptGhostText seeds the
    // consumed-prefix memory with the accepted suffix so adjustGhostAfterBackwardDelete restores it.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" --build --target install");
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake --build --target install");
    REQUIRE(field.ghostText().empty());
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // DeleteWordBackward
    CHECK(field.text() == "cmake --build --target ");
    CHECK(field.ghostText() == "install"); // restored synchronously — no flash
}

TEST_CASE("InputField.ghost_text_restored_on_char_backspace_after_accept")
{
    // After accept, a single Backspace restores the last char onto the ghost. Verifies that
    // deleteCharBackward's materialization guard sees the non-empty consumed memory.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" build");
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake build");
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "cmake buil");
    CHECK(field.ghostText() == "d"); // last accepted char restored
}

TEST_CASE("InputField.ghost_text_chains_repeated_word_backspace_after_accept")
{
    // Two consecutive word-backward deletes each restore their suffix: the consumed memory pops
    // the matched tail so the next delete still matches.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" --build install");
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake --build install");
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // delete "install"
    CHECK(field.text() == "cmake --build ");
    CHECK(field.ghostText() == "install");
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // delete "build" (then "--")
    CHECK(field.text() == "cmake --");
    CHECK(field.ghostText() == "build install");
}

TEST_CASE("InputField.ghost_text_not_resurrected_on_nontail_edit_after_accept")
{
    // After accept, deleting somewhere that is NOT a tail of the accepted suffix must not invent a
    // wrong ghost. Here the cursor is moved into the middle, so wasAtEnd is false → ghost cleared.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" build");
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake build");
    (void) field.processEvent(specialKey(KeyCode::Left));      // cursor mid-buffer (not at end)
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // deletes 'l' of "build"
    CHECK(field.text() == "cmake buid");
    CHECK(field.ghostText().empty()); // no wrong ghost invented
}

TEST_CASE("InputField.ghost_text_accept_empty_ghost_is_noop_no_consumed_seed")
{
    // Accept with no ghost is a no-op and must not seed a stale consumed run that a later backspace
    // could resurrect.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.acceptGhostText(); // no ghost — no-op
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "cmak");
    CHECK(field.ghostText().empty());
}

TEST_CASE("InputField.ghost_text_accept_seed_is_single_line")
{
    // The accepted suffix is single-line by construction (setGhostText strips at the newline), so
    // the seeded consumed memory and any restored ghost stay single-line.
    InputField field;
    field.setMultiline(true);
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" build\nmore"); // truncated to " build" by setGhostText
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake build");
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // delete "build"
    CHECK(field.ghostText() == "build");
    CHECK_FALSE(field.ghostText().contains('\n'));
}

TEST_CASE("InputField.bigword_backward_deletes_whole_space_delimited_token")
{
    // Alt+Backspace (DeleteBigWordBackward) uses whitespace-only boundaries, so it removes a whole
    // space-delimited token at once — including any '-' or '/' inside it.
    InputField field;
    field.setText("git checkout -b");
    field.setCursor(15);
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt)); // Alt+Backspace
    CHECK(field.text() == "git checkout ");
}

TEST_CASE("InputField.bigword_backward_diverges_from_ctrl_w_small_word")
{
    // The two granularities differ on a token containing '-': bigword (Alt+Backspace) takes the
    // whole "-b", while small-word (Ctrl+W) stops at the '-' boundary.
    InputField big;
    big.setText("git checkout -b");
    big.setCursor(15);
    (void) big.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(big.text() == "git checkout ");

    InputField small;
    small.setText("git checkout -b");
    small.setCursor(15);
    (void) small.processEvent(charKey('w', Modifier::Ctrl));
    CHECK(small.text() == "git checkout -"); // small word stops at the '-' boundary
}

TEST_CASE("InputField.bigword_backward_deletes_whole_path")
{
    // A path like "foo/bar/baz" is a single bigword: Alt+Backspace clears it entirely, whereas
    // Ctrl+W would only remove the trailing "baz" path component.
    InputField big;
    big.setText("foo/bar/baz");
    big.setCursor(11);
    (void) big.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(big.text().empty());

    InputField small;
    small.setText("foo/bar/baz");
    small.setCursor(11);
    (void) small.processEvent(charKey('w', Modifier::Ctrl));
    CHECK(small.text() == "foo/bar/");
}

TEST_CASE("InputField.bigword_backward_at_start_is_noop")
{
    InputField field;
    field.setText("foo");
    field.setCursor(0);
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(field.text() == "foo");
}

TEST_CASE("InputField.bigword_backward_restores_ghost_text_after_accept")
{
    // Bigword backward delete must adjust ghost text just like the small-word path: deleting the
    // accepted tail restores it as the ghost (no flash). Mirrors the Ctrl+W ghost test.
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" install");
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake install");
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt)); // delete "install"
    CHECK(field.text() == "cmake ");
    CHECK(field.ghostText() == "install"); // restored synchronously — no flash
}

TEST_CASE("InputField.bigword_backward_mid_buffer_deletes_and_clears_ghost")
{
    // Cursor NOT at end (wasAtEnd == false): the bigword kill removes the token before the cursor
    // and the trailing text is preserved. A mid-buffer delete is never rendered with a ghost, so
    // any live ghost is dropped.
    InputField field;
    field.setText("foo bar baz");
    field.setCursor(7); // just after "bar"
    field.setGhostText("ghost");
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(field.text() == "foo  baz"); // "bar" removed, surrounding spaces and "baz" intact
    CHECK(field.cursor() == 4);
    CHECK(field.ghostText().empty()); // mid-buffer delete clears the ghost
}

TEST_CASE("InputField.bigword_backward_reprepends_live_ghost")
{
    // Live-ghost re-prepend path: no consumed-prefix memory, a ghost is showing, and the delete is
    // at end-of-buffer. The killed run is prepended onto the live ghost (mirrors the small-word
    // path) so the suggestion still renders without a debounce gap.
    InputField field;
    field.setText("git remote");
    field.setCursor(10);
    field.setGhostText(" add");
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt)); // delete "remote"
    CHECK(field.text() == "git ");
    CHECK(field.ghostText() == "remote add"); // killed token prepended onto the live ghost
}

TEST_CASE("InputField.bigword_backward_past_suggestion_clears_ghost")
{
    // When the bigword kill removes MORE than the accepted suggestion tail — i.e. user-typed text
    // that the suggestion depended on — the suggestion is no longer valid for the new buffer
    // prefix, so the ghost is cleared rather than showing a stale suggestion. (The small-word path
    // would delete only the accepted "b" and could restore it; bigword deletes "-b" and cannot.)
    InputField field;
    field.setText("git checkout -");
    field.setCursor(14);
    field.setGhostText("b");
    field.acceptGhostText(); // _ghostConsumed = "b", buffer = "git checkout -b"
    REQUIRE(field.text() == "git checkout -b");
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt)); // deletes "-b"
    CHECK(field.text() == "git checkout ");
    CHECK(field.ghostText().empty()); // "b" alone is not a valid suggestion after '-' is gone
}

TEST_CASE("InputField.bigword_backward_stops_at_unicode_whitespace")
{
    // isBigWordChar classifies a whole codepoint, so multibyte Unicode separators delimit words.
    // U+3000 IDEOGRAPHIC SPACE (\xE3\x80\x80) must stop the bigword kill at the space boundary
    // rather than being treated as a word character and swallowed.
    InputField ideographic;
    ideographic.setText("foo\xE3\x80\x80"
                        "bar"); // "foo" + U+3000 + "bar"
    ideographic.setCursor(9);   // after "bar" (3 + 3 + 3 bytes)
    (void) ideographic.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(ideographic.text() == "foo\xE3\x80\x80"); // only "bar" removed; the space remains

    InputField nbsp;
    nbsp.setText("foo\xC2\xA0"
                 "bar"); // "foo" + U+00A0 NBSP + "bar"
    nbsp.setCursor(8);   // after "bar" (3 + 2 + 3 bytes)
    (void) nbsp.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(nbsp.text() == "foo\xC2\xA0"); // only "bar" removed
}

TEST_CASE("InputField.bigword_backward_skips_multiple_interior_spaces")
{
    // Several ASCII spaces before the token are skipped (boundary run) and the whole word plus its
    // leading whitespace run back to the previous token is removed, matching emacs backward-kill.
    InputField field;
    field.setText("echo    hello");
    field.setCursor(13);
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    CHECK(field.text() == "echo    "); // "hello" removed, the space run before it kept
}

TEST_CASE("InputField.bigword_backward_pushes_killed_token_to_kill_ring")
{
    // The bigword kill shares the kill ring: the removed token is yankable. Verifies the kill-ring
    // round-trip rather than only the resulting buffer text.
    InputField field;
    field.keyBindings().bind(KeyChord::fromChar('v', Modifier::Ctrl), EditAction::Paste);
    field.setText("make foo/bar");
    field.setCursor(12);
    (void) field.processEvent(specialKey(KeyCode::Backspace, Modifier::Alt));
    REQUIRE(field.text() == "make ");
    (void) field.processEvent(charKey('v', Modifier::Ctrl)); // yank
    CHECK(field.text() == "make foo/bar");                   // whole bigword "foo/bar" was on the kill ring
}

TEST_CASE("InputField.ghost_text_cleared_on_backspace_when_cursor_not_at_end")
{
    InputField field;
    field.setText("foobar");
    field.setCursor(6);
    field.setGhostText("baz");
    (void) field.processEvent(specialKey(KeyCode::Left));      // cursor 5, ghost no longer shown
    (void) field.processEvent(specialKey(KeyCode::Backspace)); // deletes 'a' before cursor
    CHECK(field.text() == "foobr");
    CHECK(field.ghostText().empty()); // mid-buffer delete clears ghost
}

TEST_CASE("InputField.ghost_text_untouched_on_forward_delete_at_end")
{
    InputField field;
    field.setText("foo");
    field.setCursor(3);
    field.setGhostText("bar");
    (void) field.processEvent(specialKey(KeyCode::Delete)); // deletes nothing at end
    CHECK(field.text() == "foo");
    CHECK(field.ghostText() == "bar"); // visible ghost preserved
}

TEST_CASE("InputField.ghost_text_cleared_on_forward_delete_midbuffer")
{
    InputField field;
    field.setText("foobar");
    field.setCursor(3);
    field.setGhostText("baz");
    (void) field.processEvent(specialKey(KeyCode::Delete)); // deletes 'b' mid-buffer
    CHECK(field.text() == "fooar");
    CHECK(field.ghostText().empty());
}

TEST_CASE("InputField.ghost_text_cleared_on_delete_selection")
{
    InputField field;
    field.setText("foobar");
    field.setCursor(6);
    field.setGhostText("baz");
    // Select 'bar' (three chars back), then delete the selection.
    (void) field.processEvent(specialKey(KeyCode::Left, Modifier::Shift));
    (void) field.processEvent(specialKey(KeyCode::Left, Modifier::Shift));
    (void) field.processEvent(specialKey(KeyCode::Left, Modifier::Shift));
    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "foo");
    CHECK(field.ghostText().empty()); // selection delete clears, does not re-prepend
}

// ============================================================================
// Ghost state must not survive buffer-mutating editor commands
// (yank / yank-pop / transpose / history recall / setText / clear)
// ============================================================================

TEST_CASE("InputField.ghost_text_cleared_on_yank_pop")
{
    // Yank-pop (Alt+Y, default-bound) inserts a kill-ring entry, rewriting the buffer. With a live
    // ghost showing, the mutation must drop it rather than leave it dangling on the new text.
    // (Yank/Transpose share the same fix but have no default keybinding to drive from a unit test.)
    InputField field;
    field.setText("foo");
    field.setCursor(3);
    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // kill "foo" → kill ring ["foo"]
    REQUIRE(field.text().empty());

    field.setGhostText("ghost"); // a live ghost the upcoming yank-pop must clear
    REQUIRE(field.ghostText() == "ghost");
    (void) field.processEvent(charKey('y', Modifier::Alt)); // YankPop inserts from the kill ring
    CHECK(field.ghostText().empty());                       // yank-pop dropped the stale ghost
}

TEST_CASE("InputField.ghost_text_cleared_on_history_recall")
{
    InputField field;
    field.addHistory("ls -la");
    field.setText("git ");
    field.setCursor(4);
    field.setGhostText("checkout");
    field.acceptGhostText(); // _ghostConsumed="checkout"
    REQUIRE(field.text() == "git checkout");

    (void) field.processEvent(specialKey(KeyCode::Up)); // recall "ls -la"
    CHECK(field.text() == "ls -la");
    CHECK(field.ghostText().empty());

    (void) field.processEvent(specialKey(KeyCode::Backspace)); // delete 'a' at end
    CHECK(field.text() == "ls -l");
    CHECK(field.ghostText().empty()); // previous buffer's ghost not resurrected
}

TEST_CASE("InputField.ghost_text_cleared_on_setText")
{
    // setText replaces the buffer wholesale: it must drop BOTH the displayed ghost and the
    // consumed-prefix memory, else the previous buffer's suggestion renders / re-prepends.
    InputField field;
    field.setText("git ch");
    field.setCursor(6);
    field.setGhostText("eckout");
    field.setText("foo");
    CHECK(field.ghostText().empty()); // stale _ghostText dropped

    (void) field.processEvent(specialKey(KeyCode::Backspace));
    CHECK(field.text() == "fo");
    CHECK(field.ghostText().empty()); // not re-prepended onto a previous-buffer ghost
}

TEST_CASE("InputField.ghost_text_cleared_on_clear")
{
    InputField field;
    field.setText("hello");
    field.setCursor(5);
    field.setGhostText(" world");
    field.clear();
    CHECK(field.text().empty());
    CHECK(field.ghostText().empty()); // clear() drops the displayed ghost, not just consumed memory
}

TEST_CASE("InputField.ghost_text_accept_seed_strips_carriage_return")
{
    // A suggestion carrying a '\r' is stripped from the buffer by sanitizeForSingleLine on accept;
    // the consumed-prefix seed must match the SANITIZED buffer so a later word-backspace restores it
    // synchronously (rather than mismatching on the '\r' and skipping the restore).
    InputField field;
    field.setText("cmake");
    field.setCursor(5);
    field.setGhostText(" build"); // setGhostText strips at the first line break anyway
    field.acceptGhostText();
    REQUIRE(field.text() == "cmake build");
    REQUIRE(field.ghostText().empty());

    (void) field.processEvent(charKey('w', Modifier::Ctrl)); // delete "build"
    CHECK(field.text() == "cmake ");
    CHECK(field.ghostText() == "build"); // restored synchronously, seed matched the buffer
    CHECK_FALSE(field.ghostText().contains('\r'));
}

TEST_CASE("InputField.ghost_text_set_strips_carriage_return")
{
    // setGhostText / prependGhostText share the single-line rule: a '\r' must never enter the ghost
    // (it would render as a control glyph and be committed verbatim on accept).
    InputField field;
    field.setText("a");
    field.setCursor(1);
    field.setGhostText("b\rc"); // truncated at '\r'
    CHECK(field.ghostText() == "b");
    CHECK_FALSE(field.ghostText().contains('\r'));
}

TEST_CASE("InputField.ghost_text_backspace_does_not_splice_grapheme_base_char")
{
    // Per-codepoint consumption vs per-grapheme-cluster deletion: when the ghost head is a combining
    // mark and the user types it (forming a composed grapheme with the preceding base char), a
    // backspace deletes the WHOLE cluster (base + mark). The deleted run is longer than the consumed
    // memory, so the run must NOT be spliced back into the ghost (which would re-commit the base
    // char on accept). The ghost is cleared and the debounce will recompute.
    InputField field;
    field.setText("xe");
    field.setCursor(2); // after base 'e'
    // Ghost: combining acute U+0301 (\xCC\x81) then '!'. Typing the combining mark trims it.
    field.setGhostText("\xCC\x81!");
    // Type the combining mark codepoint U+0301 directly via a key event with that codepoint.
    (void) field.processEvent(
        KeyEvent { .key = static_cast<KeyCode>(0x0301), .modifiers = Modifier::None, .codepoint = 0x0301 });
    // Buffer is now "x" + composed 'é'; ghost trimmed to "!", _ghostConsumed holds the mark bytes.
    REQUIRE(field.ghostText() == "!");

    (void) field.processEvent(specialKey(KeyCode::Backspace)); // deletes the whole 'é' cluster
    CHECK(field.text() == "x");
    CHECK(field.ghostText().empty()); // base char NOT spliced into the ghost
    CHECK_FALSE(field.ghostText().contains('e'));
}

// ============================================================================
// Horizontal scroll (single-line overflow)
// ============================================================================

namespace
{

// Render the field into a fresh buffer of (1 x cols) and return it.
Buffer renderSingleLine(InputField& field, int cols)
{
    Buffer buffer(1, cols);
    auto const theme = darkTheme();
    Canvas canvas(buffer, Rect { .x = 0, .y = 0, .width = cols, .height = 1 }, theme);
    field.render(canvas);
    return buffer;
}

std::string rowText(Buffer const& buffer)
{
    std::string out;
    for (auto const c: std::views::iota(0, std::max(0, buffer.cols())))
    {
        auto const& cell = requireCell(buffer, { .x = c, .y = 0 });
        out += cell.grapheme.empty() ? std::string(" ") : cell.grapheme;
    }
    return out;
}

} // namespace

TEST_CASE("InputField.hscroll_long_line_follows_cursor_to_right_edge")
{
    InputField field;
    field.setText("0123456789ABCDEF"); // 16 graphemes, cursor at end (16)

    auto buffer = renderSingleLine(field, 10);

    // Tail of the input plus a trailing blank for the end-of-line cursor.
    // The leftmost cell is overlaid with the left continuation indicator
    // because content continues off-screen on the left.
    CHECK(rowText(buffer)
          == "\xE2\x80\xB9"
             "89ABCDEF ");
    CHECK(buffer.cursor().x == 9);
    CHECK(buffer.cursor().y == 0);
}

TEST_CASE("InputField.hscroll_cursor_at_start_shows_prefix")
{
    InputField field;
    field.setText("0123456789ABCDEF");
    field.setCursor(0);

    auto buffer = renderSingleLine(field, 10);

    // Right-most cell shows the right continuation indicator.
    CHECK(rowText(buffer) == "012345678\xE2\x80\xBA");
    CHECK(buffer.cursor().x == 0);
}

TEST_CASE("InputField.hscroll_cursor_in_middle_scrolls_into_view")
{
    InputField field;
    field.setText("0123456789ABCDEF");
    field.setCursor(12); // between 'B' and 'C'

    auto buffer = renderSingleLine(field, 10);

    // Offset places cursor at the right edge; leftmost cell gets the left indicator.
    CHECK(rowText(buffer)
          == "\xE2\x80\xB9"
             "456789ABC");
    CHECK(buffer.cursor().x == 9);
}

TEST_CASE("InputField.hscroll_short_line_does_not_scroll")
{
    InputField field;
    field.setText("hi");
    field.setCursor(2);

    auto buffer = renderSingleLine(field, 10);

    CHECK(rowText(buffer) == "hi        ");
    CHECK(buffer.cursor().x == 2);
}

TEST_CASE("InputField.hscroll_prompt_area_is_preserved")
{
    InputField field;
    field.setPrompt("$ ");
    field.setText("0123456789ABCDEF");

    auto buffer = renderSingleLine(field, 10);

    // Prompt occupies cols 0-1; text area is 8 cols. Cursor at end ⇒ tail visible
    // with a trailing blank for the cursor: "$ BCDEF  ".
    auto const row = rowText(buffer);
    CHECK(row.substr(0, 2) == "$ ");
    CHECK(buffer.cursor().x == 9);
}

TEST_CASE("InputField.hscroll_right_indicator_when_tail_clipped")
{
    InputField field;
    field.setText("0123456789ABCDEF"); // 16 chars
    field.setCursor(0);                // viewport pinned to the left

    auto buffer = renderSingleLine(field, 10);

    // First 9 chars of the buffer visible; last cell replaced by the right indicator.
    auto const row = rowText(buffer);
    CHECK(row.substr(0, 9) == "012345678");
    CHECK(row.substr(9, 3) == "\xE2\x80\xBA"); // U+203A ›
    CHECK(buffer.cursor().x == 0);
}

TEST_CASE("InputField.hscroll_left_indicator_when_head_clipped")
{
    InputField field;
    field.setText("0123456789ABCDEF");
    field.setCursor(16); // end — viewport scrolled right

    auto buffer = renderSingleLine(field, 10);

    // Leftmost cell shows the left indicator on top of the first visible char.
    auto const row = rowText(buffer);
    CHECK(row.substr(0, 3) == "\xE2\x80\xB9"); // U+2039 ‹
    CHECK(buffer.cursor().x == 9);
}

TEST_CASE("InputField.hscroll_both_indicators_when_clipped_on_both_sides")
{
    InputField field;
    field.setText("0123456789ABCDEFGHIJ"); // 20 chars

    // First render: cursor mid-buffer, viewport scrolls right (offset > 0).
    field.setCursor(15);
    (void) renderSingleLine(field, 6);

    // Second render: cursor moves back into the visible range, so the viewport
    // stays put — text is clipped on BOTH sides and the cursor is in the middle.
    field.setCursor(12);
    auto buffer = renderSingleLine(field, 6);

    auto const row = rowText(buffer);
    CHECK(row.substr(0, 3) == "\xE2\x80\xB9");
    CHECK(row.substr(row.size() - 3) == "\xE2\x80\xBA");
    CHECK(buffer.cursor().x != 0);
    CHECK(buffer.cursor().x != buffer.cols() - 1);
}

TEST_CASE("InputField.hscroll_no_indicators_when_content_fits")
{
    InputField field;
    field.setText("hello");

    auto buffer = renderSingleLine(field, 10);
    auto const row = rowText(buffer);
    // Plain ASCII "hello" followed by spaces — no angle-quote bytes present.
    CHECK_FALSE(row.contains("\xE2\x80\xB9"));
    CHECK_FALSE(row.contains("\xE2\x80\xBA"));
}

TEST_CASE("InputField.hscroll_resets_on_clear_and_setText")
{
    InputField field;
    field.setText("0123456789ABCDEF");
    (void) renderSingleLine(field, 10); // establishes non-zero scroll offset

    field.clear();
    auto buffer = renderSingleLine(field, 10);
    CHECK(buffer.cursor().x == 0);

    field.setText("abc");
    buffer = renderSingleLine(field, 10);
    CHECK(rowText(buffer).substr(0, 3) == "abc");
    CHECK(buffer.cursor().x == 3);
}

TEST_CASE("InputField.a_temporary_clock_is_refused")
{
    STATIC_CHECK(std::is_constructible_v<InputField, ManualClock&>);
    STATIC_CHECK_FALSE(std::is_constructible_v<InputField, ManualClock>);
}
