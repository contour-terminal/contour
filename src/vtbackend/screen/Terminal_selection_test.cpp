// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/App.hpp>
#include <crispy/Times.hpp>
#include <crispy/Utils.hpp>
#include <crispy/testing/Environment.hpp>

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

// NOLINTEND(misc-const-correctness)
