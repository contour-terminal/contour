// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Times.hpp>
#include <core/Utils.hpp>
#include <core/cli/App.hpp>
#include <core/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace std;
using namespace std::chrono_literals;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
using vtbackend::Modifier;
using vtbackend::PageSize;
using namespace vtbackend::test;

// NOLINTBEGIN(misc-const-correctness)

TEST_CASE("Terminal.selection_does_not_pad_wide_characters", "[terminal]")
{
    // A wide character occupies two cells: a head carrying the text, and a continuation whose
    // codepoint is 0. Copying must yield the character ONCE. Treating the continuation as an empty
    // cell turns it into a space, so every CJK selection comes back padded -- and a multi-column
    // text-sizing block would trail one space per extra column.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(1), ColumnCount(6) } };
    mock.writeToScreen("\u4e2dab");

    mock.terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    mock.terminal.selector()->complete();

    // Not "\u4e2d ab" -- the space between the wide char and 'a' is the bug.
    CHECK(mock.terminal.extractSelectionText() == "\u4e2dab");
}

TEST_CASE("Terminal.selection_keeps_leading_blank_lines", "[terminal]")
{
    // A blank line inside a selection is a line the user selected, so it must copy as a newline. The
    // line break is emitted as a SEPARATOR, guarded on whether anything has been written yet -- and
    // using the accumulated text for that test loses LEADING blank lines, because such a line trims
    // to nothing and never makes the text non-empty. Interior blank lines survive, so the loss is
    // silent and depends on where in the selection the blank line falls.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(6) } };
    mock.writeToScreen("\033[3;1H"
                       "abc"sv); // rows 0 and 1 stay blank

    mock.terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "\n\nabc");
}

TEST_CASE("Terminal.TextSelection", "[terminal]")
{
    // Create empty TE
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    // Fill main page with text
    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    // Perform selection
    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    mock.terminal.tick(1s);
    auto const appHandledMouse = mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    // We want to ensure that this call is returning false if the app has not explicitly requested
    // to listen on mouse events (without passive mode being on).
    REQUIRE(appHandledMouse == Handled { false });

    CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

    // Mouse is pressed, but we did not start selecting (by moving the mouse) yet,
    // so any text extraction shall be empty.
    CHECK(mock.terminal.extractSelectionText().empty());

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

    mock.terminal.tick(1s);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

    // Clear selection by simply left-clicking.
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 2_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText().empty());
}

TEST_CASE("Terminal.plain_drag_selection_does_not_overwrite_the_search_pattern", "[terminal]")
{
    // Regression: updateSelectionMatches() serializes the selection into Terminal::search().pattern
    // so double/triple-click word-selection can highlight the word's other occurrences elsewhere on
    // screen (SearchOrigin::DoubleClick). It used to run unconditionally on EVERY selection-extending
    // mouse-move -- including an ordinary single-click drag, which is not a word at all -- so dragging
    // out any plain text selection silently overwrote whatever search pattern the user had typed (and,
    // wherever a status line or find-bar echoes it, made the selected text visibly appear as a live
    // search).
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Seed a search pattern the way a user typing into the find bar would.
    (void) mock.terminal.setNewSearchTerm(U"needle", SearchOrigin::Typed);
    REQUIRE(mock.terminal.search().pattern == U"needle");

    // An ordinary single-click drag across two lines -- not a double/triple-click word selection.
    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    REQUIRE(mock.terminal.extractSelectionText() == "7890\nABC");

    // The drag must not have touched the search pattern.
    CHECK(mock.terminal.search().pattern == U"needle");

    mock.terminal.tick(1s);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.search().pattern == U"needle");
}

TEST_CASE("Terminal.word_selection_across_a_soft_wrap_still_updates_the_search_pattern", "[terminal]")
{
    // Regression: updateSelectionMatches() used to reject a word-wise selection whose from() and to()
    // sat on different PHYSICAL rows, on the theory that a multi-line pattern cannot match the flat,
    // single-line stream RenderBufferBuilder's search-match scanner walks. But a word straddling a soft
    // wrap spans two rows while extracting as ONE line -- SelectionRenderer emits a break only where
    // `!isLineWrapped` -- so the guard refused a perfectly good pattern and left the previous search
    // term standing, costing the wrapped word its matching-word highlights.
    auto mock = MockTerm { ColumnCount(5), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");

    // One 10-character word: fills row 0 and soft-wraps into row 1.
    mock.writeToScreen(std::string(10, 'a'));

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    (void) mock.terminal.setNewSearchTerm(U"needle", SearchOrigin::Typed);
    REQUIRE(mock.terminal.search().pattern == U"needle");

    // Double-click inside the word: two presses at the same cell, inside the one-second window.
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    // The selection crossed the wrap, yet extracts as a single line...
    auto const selected = mock.terminal.extractSelectionText();
    REQUIRE(selected == std::string(10, 'a'));
    REQUIRE(!selected.contains('\n'));

    // ...so it IS a word, and must have become the pattern.
    CHECK(mock.terminal.search().pattern == std::u32string(10, U'a'));
    CHECK(mock.terminal.search().origin == SearchOrigin::DoubleClick);
}

namespace
{
/// Double-clicks the cell at @p at, a second after any click before it, and returns the text selected.
std::string doubleClickAt(MockTerm<vtpty::MockPty>& mock, CellLocation at)
{
    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    return mock.terminal.extractSelectionText();
}
} // namespace

TEST_CASE("Terminal.word_selection_steps_over_wide_characters_and_wrap_padding", "[terminal]")
{
    // A word holds a wide character as one character: neither its second half nor the padding a narrowing
    // leaves where it moved on to the next row ends the word, as foot's word walk steps over its spacers
    // (selection.c:404).
    auto mock = MockTerm { ColumnCount(10), LineCount(3) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("ABCD\xE4\xB8\xAD"
                       "E F"); // U+4E2D, two columns

    using namespace vtbackend;
    auto const doubleClick = [&](CellLocation at) {
        return doubleClickAt(mock, at);
    };

    SECTION("on one row")
    {
        CHECK(doubleClick(0_lineOffset + 0_columnOffset)
              == "ABCD\xE4\xB8\xAD"
                 "E");
        CHECK(doubleClick(0_lineOffset + 6_columnOffset)
              == "ABCD\xE4\xB8\xAD"
                 "E");
    }

    SECTION("across the padding a narrowing leaves")
    {
        // "ABCD" and padding, then "中E F" on the row that continues it.
        mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(5) });
        REQUIRE(mock.terminal.primaryScreen()
                    .grid()
                    .lineAt(LineOffset(0))
                    .storage()
                    .sgr[4]
                    .flags.contains(CellFlag::WrapPadding));

        CHECK(doubleClick(0_lineOffset + 0_columnOffset)
              == "ABCD\xE4\xB8\xAD"
                 "E");
        CHECK(doubleClick(1_lineOffset + 2_columnOffset)
              == "ABCD\xE4\xB8\xAD"
                 "E");

        // The word under the cursor, which vi's * and # search for, is the same word: one line, as
        // it was printed.
        CHECK(std::get<0>(mock.terminal.extractWordUnderCursor(0_lineOffset + 1_columnOffset)) == U"ABCD中E");
    }
}

TEST_CASE("Terminal.word_selection_follows_a_word_only_across_soft_wraps", "[terminal]")
{
    // A word runs on into the next row exactly where that row continues this one, and a row after a hard
    // line break begins words of its own, as foot's word walk stops at a hard linebreak (selection.c:397).
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
    mock.terminal.setWordDelimiters(" ");

    SECTION("a word that ends just past a wrap, on a row that continues one")
    {
        // "ab cd", "efghi", "j k": the second and third rows each continue the one above.
        mock.writeToScreen("ab cdefghij k");
        REQUIRE(mock.terminal.isLineWrapped(LineOffset(2)));
        CHECK(doubleClickAt(mock, 0_lineOffset + 3_columnOffset) == "cdefghij");
        CHECK(doubleClickAt(mock, 2_lineOffset + 0_columnOffset) == "cdefghij");
        // The word under the cursor, which vi's * and # search for, walks the same way.
        CHECK(std::get<0>(mock.terminal.extractWordUnderCursor(0_lineOffset + 3_columnOffset))
              == U"cdefghij");
    }

    SECTION("not across a hard line break after a wrapped row")
    {
        // "ab cd", "efghi" continuing it, then "XY" after a line break.
        mock.writeToScreen("ab cdefghi\r\nXY");
        REQUIRE_FALSE(mock.terminal.isLineWrapped(LineOffset(2)));
        CHECK(doubleClickAt(mock, 0_lineOffset + 3_columnOffset) == "cdefghi");
        CHECK(doubleClickAt(mock, 2_lineOffset + 0_columnOffset) == "XY");
        CHECK(std::get<0>(mock.terminal.extractWordUnderCursor(2_lineOffset + 0_columnOffset)) == U"XY");
    }

    SECTION("not back across a hard line break")
    {
        mock.writeToScreen("XYZWV\r\nabc");
        CHECK(doubleClickAt(mock, 1_lineOffset + 1_columnOffset) == "abc");
    }

    SECTION("back into the history")
    {
        // "ab cd" scrolls into the history; "efghi", "jkl" and "mn" continue it on the page.
        mock.writeToScreen("ab cdefghijklmnopqrs");
        REQUIRE(mock.terminal.primaryScreen().historyLineCount() == LineCount(1));
        REQUIRE(mock.terminal.isLineWrapped(LineOffset(0)));
        CHECK(doubleClickAt(mock, 0_lineOffset + 0_columnOffset) == "cdefghijklmnopqrs");
    }
}

TEST_CASE("Terminal.both_word_walks_end_a_word_on_the_same_cell", "[terminal]")
{
    // The double-click selection and the word under the cursor (vi's * and #) walk a word the same way:
    // a word ending on a wide character ends on its second half, so the whole character is in it.
    using namespace vtbackend;
    auto mock = MockTerm { ColumnCount(8), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("ab\xE4\xB8\xAD x"); // U+4E2D on columns 2 and 3

    auto const [word, range] = mock.terminal.extractWordUnderCursor(0_lineOffset + 0_columnOffset);
    CHECK(word == U"ab中");
    CHECK(range.first == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(range.second == CellLocation { LineOffset(0), ColumnOffset(3) });

    CHECK(doubleClickAt(mock, 0_lineOffset + 0_columnOffset) == "ab\xE4\xB8\xAD");
    REQUIRE(mock.terminal.selector() != nullptr);
    CHECK(mock.terminal.selector()->to() == range.second);

    // A word that is nothing but the wide character, taken from its first column.
    mock.writeToScreen("\r\n\xE4\xB8\xAD x");
    auto const [alone, aloneRange] = mock.terminal.extractWordUnderCursor(1_lineOffset + 0_columnOffset);
    CHECK(alone == U"中");
    CHECK(aloneRange.second == CellLocation { LineOffset(1), ColumnOffset(1) });
    CHECK(doubleClickAt(mock, 1_lineOffset + 0_columnOffset) == "\xE4\xB8\xAD");
    CHECK(mock.terminal.selector()->to() == aloneRange.second);
}

TEST_CASE("Terminal.a_press_at_a_new_cell_starts_a_fresh_click_sequence", "[terminal]")
{
    // Regression: sendMousePressEvent() anchors a bare press at the cell it landed on (a fast click can
    // arrive with no preceding MouseMove), but it did so WITHOUT resetting the multi-click counter that
    // sendMouseMoveEvent resets for exactly the same cell change. A second fast click at a DIFFERENT
    // cell, still inside the one-second window, therefore inherited the first one's count and opened as
    // a double-click word selection instead of a fresh drag.
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // First click, on "hello".
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    // A second bare press at a DIFFERENT cell, arriving well inside the one-second double-click window
    // and with no MouseMove in between -- the fast-click path.
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 7_columnOffset, PixelCoordinate, UiHandledHint);

    // It must open a fresh, empty drag -- not select the word under it.
    CHECK(mock.terminal.selector() != nullptr);
    CHECK(dynamic_cast<WordWiseSelection const*>(mock.terminal.selector()) == nullptr);
    CHECK(mock.terminal.extractSelectionText().empty());
}

TEST_CASE("Terminal.shift_click_extending_a_word_retires_its_search_pattern", "[terminal]")
{
    // Regression: a double click makes the word the search pattern (SearchOrigin::DoubleClick) so its
    // other occurrences highlight. Shift+Click then re-creates the selection as a LinearSelection --
    // which updateSelectionMatches() rightly refuses to serialize -- so the call it used to make there
    // became a no-op, and the double click's pattern stayed live, painting matches for a word the
    // selection no longer is. The extension must retire it, as an ordinary single click already does.
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Double-click "hello": two presses at the same cell inside the one-second window.
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(mock.terminal.extractSelectionText() == "hello");
    REQUIRE(mock.terminal.search().pattern == U"hello");
    REQUIRE(mock.terminal.search().origin == SearchOrigin::DoubleClick);

    // Shift+Click out to "world": the selection stops being that word.
    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::Shift, MouseButton::Left, 0_lineOffset + 9_columnOffset, PixelCoordinate, UiHandledHint);

    CHECK(mock.terminal.extractSelectionText() != "hello");

    // The word's pattern must not have outlived it.
    CHECK(mock.terminal.search().pattern.empty());
}

TEST_CASE("Terminal.TextSelection_wrapped_line", "[terminal]")
{
    // Create empty TE
    auto mock = MockTerm { ColumnCount(5), LineCount(2) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    // write one line with 10 a
    mock.writeToScreen(std::string(10, 'a'));

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("aaaaa\naaaaa" == trimmedTextScreenshot(mock));

    // Perform selection
    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    mock.terminal.tick(1s);
    auto const appHandledMouse = mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    REQUIRE(appHandledMouse == Handled { false });

    CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

    CHECK(mock.terminal.extractSelectionText().empty());

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "aaaaaa");

    mock.terminal.tick(1s);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText() == "aaaaaa");

    mock.terminal.tick(1s);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    CHECK(mock.terminal.extractSelectionText().empty());
}

TEST_CASE("Terminal.selection_does_not_join_rows_whose_wrap_was_erased", "[terminal]")
{
    // EL 0 erased the end of the first row, so it no longer wraps into the second: a copy across both is
    // two lines, not one.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(10) } };
    mock.writeToScreen("ABCDEFGHIJKLMNO\033[1;6H\033[K");

    mock.terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "ABCDE\nKLMNO");
}

TEST_CASE("Terminal.ShiftClickExtendSelection", "[terminal]")
{
    // Create TE with some content.
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("extends completed selection forward")
    {
        // Select "7890\nABC" (row 1 col 1 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "7890\nABC");
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Shift+Click at row 3, col 3 to extend selection.
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);

        // Selection should now span from original start to new click position.
        CHECK(mock.terminal.extractSelectionText() == "7890\nABCDE\nabcd");
    }

    SECTION("extends completed selection backward")
    {
        // Select "BCDE\nabcd" (row 2 col 1 → row 3 col 3)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 2_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "BCDE\nabcd");
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Shift+Click at row 0 col 0 to extend backward.
        // Anchor moves to selEnd (3,3), extend to (0,0).
        // Selection covers (0,0) to (3,3): "12345\n67890\nABCDE\nabcd"
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcd");
    }

    SECTION("no selection starts new selection on Shift+Click")
    {
        // No prior selection exists. Shift+Click should start a new selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);

        // Should create a new selection (Waiting state), not crash.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);
    }

    SECTION("click to deselect after Shift+Click extend")
    {
        // Regression test: a normal click shortly after Shift+Click extend
        // must deselect rather than trigger a word-wise selection.

        // 1. Create and complete a selection: "7890\nABC" (row 1 col 1 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 1_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "7890\nABC");

        // 2. Shift+Click to extend the selection.
        mock.terminal.tick(4s + 300ms);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s + 500ms);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK_FALSE(mock.terminal.extractSelectionText().empty());

        // 3. Normal click shortly after (within 1s) to deselect — must clear selection.
        mock.terminal.tick(4s + 800ms);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 3_lineOffset + 3_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText().empty());
    }

    SECTION("extends upward then downward re-anchors correctly")
    {
        // Select "890\nABC" (row 1 col 2 → row 2 col 2)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "890\nABC");

        // Shift+Click upward at (0,0): anchor moves to selEnd (2,2), extend to (0,0).
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::Shift, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABC");

        // Shift+Click downward at (4,4): anchor moves to selStart (0,0), extend to (4,4).
        mock.terminal.tick(9s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 4_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(10s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 4_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcde\nfghij");
    }

    SECTION("extends into selection interior shrinks to nearer anchor")
    {
        // Select full content (0,0)→(4,4)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 4_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABCDE\nabcde\nfghij");

        // Shift+Click inside at (2,2): click >= selStart, so anchor = selStart (0,0), extend to (2,2).
        mock.terminal.tick(6s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(7s);
        mock.terminal.sendMousePressEvent(
            Modifier::Shift, MouseButton::Left, 2_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "12345\n67890\nABC");
    }
}

TEST_CASE("Terminal.YankHighlight.trivialLineIsHighlighted", "[terminal][vi]")
{
    // Regression: a vi yank/motion highlight over a plain-text (trivial) line must recolor it.
    // Tall page so content lines stay clear of the bottom indicator status line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB\r\n");
    mc.writeToScreen("plainC");

    // Highlight grid line 1 (a plain-text trivial line) while still in insert mode so no
    // status-line resize shifts coordinates; the highlight alone must recolor the trivial line.
    terminal.setHighlightRange(vtbackend::LinearHighlight {
        .from = vtbackend::CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) },
        .to = vtbackend::CellLocation { .line = LineOffset(1), .column = ColumnOffset(5) } });
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    CHECK(screenLineBackground(buf, LineOffset(1)) != defaultBg);
    // A non-highlighted plain-text line keeps the default background.
    CHECK(screenLineBackground(buf, LineOffset(2)) == defaultBg);
}

TEST_CASE("Terminal.selectAll", "[terminal]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(5) };

    // Push two lines into the scrollback, leaving three on the page.
    for (auto const* text: { "hist1", "hist2", "page1", "page2" })
        mock.writeToScreen(std::format("{}\r\n", text));
    mock.writeToScreen("page3");
    REQUIRE(mock.terminal.currentScreen().historyLineCount() == LineCount(2));

    REQUIRE_FALSE(mock.terminal.selectionAvailable());
    mock.terminal.selectAll();
    REQUIRE(mock.terminal.selectionAvailable());
    CHECK(mock.terminal.isSelectionComplete());

    // "All" means the scrollback too, not merely the visible page.
    auto const text = mock.terminal.extractSelectionText();
    CHECK(text.contains("hist1"));
    CHECK(text.contains("hist2"));
    CHECK(text.contains("page1"));
    CHECK(text.contains("page3"));
}

TEST_CASE("Terminal.selectAll.completesInInsertMode", "[terminal]")
{
    // Insert mode is the only mode that may complete a selection — see sendMouseReleaseEvent(), and the Vi
    // invariants pinned in ViCommands_test's "vi.selectAll" case.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(5) };
    mock.writeToScreen("one\r\n"
                       "two\r\n"
                       "three");
    REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Insert);

    mock.terminal.selectAll();

    REQUIRE(mock.terminal.selectionAvailable());
    CHECK(mock.terminal.isSelectionComplete());
}

TEST_CASE("Terminal.TextSelection_drag_into_blank_stops_at_the_pointer", "[terminal]")
{
    // Dragging past the end of a short line used to snap the selection to the right margin, so the
    // whole line lit up -- and copied -- the moment the pointer crossed the last character. The
    // guard meant to spare real spaces only ever spared TYPED ones: compareCellTextAt returns
    // `character == 0` for a cell holding no codepoints, so every never-written cell snapped.
    auto mock = MockTerm { ColumnCount(20), LineCount(4) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.writeToScreen("abc");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.tick(1s);
    (void) mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoordinate, UiHandledHint);

    // Drag well past "abc", into cells that were never written.
    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 6_columnOffset, PixelCoordinate, UiHandledHint);

    CHECK(mock.terminal.isSelected(mock.terminal.primaryScreen(),
                                   CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) }));
    CHECK_FALSE(mock.terminal.isSelected(mock.terminal.primaryScreen(),
                                         CellLocation { .line = LineOffset(0), .column = ColumnOffset(7) }));
    CHECK_FALSE(mock.terminal.isSelected(mock.terminal.primaryScreen(),
                                         CellLocation { .line = LineOffset(0), .column = ColumnOffset(19) }));
}

TEST_CASE("Terminal.TextSelection_multiline_drag_still_takes_the_first_line_whole", "[terminal]")
{
    // Removing that snap must not cost the standard behaviour: a selection running onto a later
    // line takes the first line to its right margin. Selection::ranges() and the lexicographic
    // Selection::contains already provide that, which is what made the snap redundant.
    auto mock = MockTerm { ColumnCount(20), LineCount(4) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.writeToScreen("abc\r\ndef");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.tick(1s);
    (void) mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoordinate, UiHandledHint);
    mock.terminal.tick(1s);
    mock.terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);

    // Column 15 of the FIRST line is past its text but inside a multi-line selection.
    CHECK(mock.terminal.isSelected(mock.terminal.primaryScreen(),
                                   CellLocation { .line = LineOffset(0), .column = ColumnOffset(15) }));
    // Split so the escape does not run into the text that follows it, which a spell checker reading
    // this file would otherwise take for a single misspelled token.
    CHECK(mock.terminal.extractSelectionText()
          == "abc\n"
             "de");
}

TEST_CASE("Terminal.selection_of_a_trivial_line_survives_a_scrolled_viewport", "[terminal]")
{
    // The renderer asks two questions about selection at two granularities: isSelected(CellLocation)
    // colours a cell, and isSelected(LineOffset) decides whether a line may take the trivial fast
    // path -- which draws it uniformly and never consults the per-cell test at all.
    //
    // Both overloads take GRID coordinates, but renderTrivialLine used to hand the coarse one a
    // SCREEN offset. Unscrolled the two coincide, so this only appears once the viewport moves: a
    // selected line asked about the wrong line, answered "not selected", and rendered unhighlighted.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(30) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);

    for (auto const i: std::views::iota(0, 20))
        mock.writeToScreen("line" + std::to_string(i) + "\r\n");

    // Scroll back further than text_sizing::MaxScale. The coarse test looks back that many lines
    // for a tall block reaching down into this one, and that look-back would otherwise mask a small
    // coordinate error by accident -- a scroll of 2 still lands inside it.
    mock.terminal.viewport().scrollUp(LineCount(9));
    auto const selectedGridLine = LineOffset(-9);

    auto const anchor = CellLocation { .line = selectedGridLine, .column = ColumnOffset(0) };
    mock.terminal.setSelector(
        std::make_unique<vtbackend::LinearSelection>(mock.terminal.selectionHelper(), anchor, []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = selectedGridLine, .column = ColumnOffset(3) });
    mock.terminal.selector()->complete();

    mock.terminal.tick(ClockBase + 100ms);
    mock.terminal.ensureFreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    // A selected line must NOT have been emitted as a uniform trivial line...
    auto const trivialAtScreenLine0 =
        std::ranges::any_of(buffer.get().lines, [](vtbackend::RenderLine const& line) {
            return line.lineOffset == LineOffset(0);
        });
    CHECK_FALSE(trivialAtScreenLine0);

    // ...it must have dropped to the per-cell path, where the selection colour is applied.
    auto const cellsAtScreenLine0 =
        std::ranges::count_if(buffer.get().cells, [](vtbackend::RenderCell const& cell) {
            return cell.position.line == LineOffset(0);
        });
    CHECK(cellsAtScreenLine0 > 0);
}

TEST_CASE("Terminal.a_binding_is_offered_the_click_count_before_the_selection", "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto const at = 0_lineOffset + 1_columnOffset;

    auto offered = std::vector<int> {};
    auto const claimThird = [&](uint8_t clickCount) {
        offered.push_back(clickCount);
        return clickCount == 3 ? MousePressClaim::Claimed : MousePressClaim::Unclaimed;
    };

    for ([[maybe_unused]] auto const _: std::views::iota(0, 2))
    {
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint, claimThird);
        mock.terminal.sendMouseReleaseEvent(
            Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    }
    REQUIRE(mock.terminal.extractSelectionText() == "hello");

    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint, claimThird);

    CHECK(offered == std::vector<int> { 1, 2, 3 });
    // The claimed press neither extended the word selection nor started a drag.
    CHECK(mock.terminal.extractSelectionText() == "hello");
    CHECK_FALSE(mock.terminal.leftMouseButtonPressed());
}

TEST_CASE("Terminal.a_claimed_press_whose_release_never_arrives_does_not_select_on_moves",
          "[terminal][selection]")
{
    // A claimed press may hand focus elsewhere (following a link opens the browser), so its release can be
    // lost. The pointer moving afterwards must neither start nor extend a selection, even with the
    // block-selection modifier held.
    auto mock = MockTerm { ColumnCount(11), LineCount(3) };
    mock.writeToScreen("hello world\r\nhello world");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto const claimAll = [](uint8_t) {
        return MousePressClaim::Claimed;
    };
    mock.terminal.sendMousePressEvent(Modifier::Control,
                                      MouseButton::Left,
                                      0_lineOffset + 1_columnOffset,
                                      vtbackend::PixelCoordinate {},
                                      UiHandledHint,
                                      claimAll);
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     1_lineOffset + 8_columnOffset,
                                     vtbackend::PixelCoordinate { .x = { 80 }, .y = { 20 } },
                                     UiHandledHint);
    mock.terminal.sendMouseMoveEvent(Modifier::Control,
                                     1_lineOffset + 9_columnOffset,
                                     vtbackend::PixelCoordinate { .x = { 90 }, .y = { 20 } },
                                     UiHandledHint);

    CHECK_FALSE(mock.terminal.selectionAvailable());
    CHECK_FALSE(mock.terminal.leftMouseButtonPressed());
}

TEST_CASE("Terminal.a_press_an_application_asked_for_is_never_offered_to_a_binding",
          "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.writeToScreen("\033[?1000h"); // what vim and tmux turn on

    using namespace vtbackend;
    auto offered = 0;
    mock.terminal.sendMousePressEvent(Modifier::None,
                                      MouseButton::Left,
                                      0_lineOffset + 1_columnOffset,
                                      vtbackend::PixelCoordinate {},
                                      false,
                                      [&](uint8_t) {
                                          ++offered;
                                          return MousePressClaim::Claimed;
                                      });
    CHECK(offered == 0);
}

TEST_CASE("Terminal.a_pointer_move_between_presses_starts_a_new_count", "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto offered = std::vector<int> {};
    auto const record = [&](uint8_t clickCount) {
        offered.push_back(clickCount);
        return MousePressClaim::Unclaimed;
    };

    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, false, record);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, false);
    mock.terminal.sendMouseMoveEvent(Modifier::None, 0_lineOffset + 2_columnOffset, PixelCoordinate, false);
    mock.terminal.sendMouseMoveEvent(Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, false);
    mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, false, record);

    CHECK(offered == std::vector<int> { 1, 1 });
}

TEST_CASE("Terminal.a_claimed_press_reports_the_selection_its_action_made_on_release",
          "[terminal][semanticblocks]")
{
    // A binding that claims a press and selects (Ctrl+triple-click on a block) completes the selection
    // itself. Its release must still report it, or it never reaches PRIMARY as a plain triple-click does.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    runCommand(mock, "ls", { "a-file" });
    mock.writeToScreen(PromptStart);

    using namespace vtbackend;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto const claimAll = [](uint8_t) {
        return MousePressClaim::Claimed;
    };
    auto const claimedClick = [&](auto const& action) {
        mock.terminal.sendMousePressEvent(Modifier::None,
                                          MouseButton::Left,
                                          0_lineOffset + 1_columnOffset,
                                          PixelCoordinate,
                                          false,
                                          claimAll);
        action();
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, false);
    };

    SECTION("an action that selects")
    {
        claimedClick([&] {
            REQUIRE(
                mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput)
                    .has_value());
        });
        CHECK(mock.selectionCompletions == 1);
        CHECK_FALSE(mock.terminal.leftMouseButtonPressed());
    }

    SECTION("an action that selects nothing")
    {
        // A selection made before the press is not the action's to report.
        mock.terminal.selectAll();
        claimedClick([] {});
        CHECK(mock.selectionCompletions == 0);
    }
}

TEST_CASE("Terminal.a_word_walk_works_before_anyone_sets_the_delimiters", "[terminal]")
{
    // A terminal nobody configured walks words by the delimiters its settings carry -- none, so blanks
    // alone -- rather than calling a walk it was never given.
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto const at = 0_lineOffset + 1_columnOffset;

    for (auto const clicks: std::views::iota(1, 4))
    {
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, at, PixelCoordinate, false);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, false);
        if (clicks > 1) // the word, then the extended word
            CHECK(mock.terminal.extractSelectionText() == "hello");
    }
}

// NOLINTEND(misc-const-correctness)
