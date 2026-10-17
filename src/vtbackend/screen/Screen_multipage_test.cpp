// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <core/Escape.hpp>
#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <charconv>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

TEST_CASE("MultiPage.NP_PP_navigation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Initially on page 1 (index 0).
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());

    // NP: CSI 1 U — move to next page (page 2), cursor to home.
    mock.writeToScreen("Hello");
    mock.writeToScreen("\033[1U"); // NP 1
    CHECK(terminal.cursorPageIndex() == PageIndex(1));
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(0));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(0));

    // Write on page 2.
    mock.writeToScreen("World");

    // PP: CSI 1 V — move back to page 1, cursor to home.
    mock.writeToScreen("\033[1V"); // PP 1
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(0));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(0));
    // Page 1 content should still have "Hello".
    CHECK(terminal.primaryScreen().renderMainPageText() == "Hello\n     \n     \n");

    // Move forward by 3 pages.
    mock.writeToScreen("\033[3U"); // NP 3
    CHECK(terminal.cursorPageIndex() == PageIndex(3));

    // Default param (NP without arg = 1).
    mock.writeToScreen("\033[U"); // NP (default 1)
    CHECK(terminal.cursorPageIndex() == PageIndex(4));
}

TEST_CASE("MultiPage.NP_PP_clamping", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // NP with very large count should clamp to page 15 (index 14, MaxPageCount-2).
    mock.writeToScreen("\033[100U"); // NP 100
    CHECK(terminal.cursorPageIndex() == PageIndex(MaxPageCount - 2));

    // PP with very large count should clamp to page 1 (index 0).
    mock.writeToScreen("\033[100V"); // PP 100
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
}

TEST_CASE("MultiPage.NP_PP_never_reach_alternate", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // NP should never reach the alternate screen page (index 15).
    mock.writeToScreen("\033[100U");                                  // NP beyond limit
    CHECK(terminal.cursorPageIndex() == PageIndex(MaxPageCount - 2)); // page 15, NOT 16
    CHECK(terminal.cursorPageIndex() != AlternateScreenPageIndex);
}

TEST_CASE("MultiPage.PPA_PPR_PPB_navigation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Move cursor to a specific position on page 1.
    mock.writeToScreen("\033[2;3H"); // CUP to row 2, col 3
    CHECK(terminal.currentScreen().cursor().position.line == LineOffset(1));
    CHECK(terminal.currentScreen().cursor().position.column == ColumnOffset(2));

    // PPA: CSI 5 SP P — move to page 5, to the corresponding line and column (DEC STD 070 §4.7).
    mock.writeToScreen("\033[5 P");                    // PPA to page 5
    CHECK(terminal.cursorPageIndex() == PageIndex(4)); // 0-based index
    CHECK(terminal.currentScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(2) });

    // PPR: CSI 2 SP Q — move forward 2 pages from page 5 to page 7.
    mock.writeToScreen("\033[2 Q");                    // PPR 2
    CHECK(terminal.cursorPageIndex() == PageIndex(6)); // page 7

    // PPB: CSI 3 SP R — move backward 3 pages from page 7 to page 4.
    mock.writeToScreen("\033[3 R");                    // PPB 3
    CHECK(terminal.cursorPageIndex() == PageIndex(3)); // page 4

    // PPA with page 1 (go back to primary).
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());
}

TEST_CASE("MultiPage.PPA_PPR_PPB.moveTheCursorToTheSameLineAndColumn", "[screen]")
{
    // DEC STD 070 §4.7: PPA, PPR and PPB move "the Active Position to the corresponding line and column" on
    // the other page, and affect nothing but the active position: the rendition and the character sets are
    // the terminal's. Windows Terminal's PageManager::MoveTo carries the attributes and the cursor to the new
    // page.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    auto const [movement, target, number] = GENERATE(table<std::string_view, PageIndex, int>({
        { "\033[3 P", PageIndex(2), 3 }, // PPA 3
        { "\033[2 Q", PageIndex(6), 7 }, // PPR 2
        { "\033[2 R", PageIndex(2), 3 }, // PPB 2
    }));
    INFO(e(movement));
    mock.writeToScreen("\033[5 P\033[3;5H\033[1m\033(0"); // page 5, at 3;5, bold, G0 the DEC line drawing set

    mock.writeToScreen(movement);
    REQUIRE(terminal.cursorPageIndex() == target);
    mock.discardPendingReplies();
    mock.writeToScreen("\033[?6n"); // DECXCPR
    CHECK(e(terminal.peekInput()) == e(std::format("\033[?3;5;{}R", number)));
    mock.writeToScreen("q");
    CHECK(terminal.pageAt(target).grid().lineText(LineOffset(2)) == "    ─     ");
    CHECK(terminal.currentScreen().cursor().graphicsRendition.flags.contains(CellFlag::Bold));
}

TEST_CASE("MultiPage.NP_PP.moveTheCursorHomeObeyingOriginMode", "[screen]")
{
    // DEC STD 070 §4.7: "NP moves the Active Position to the home position in the new page obeying Origin
    // Mode", PP likewise, and the rendition goes along. Windows Terminal's NextPage and PrecedingPage are a
    // page movement followed by CursorPosition(1, 1), which obeys origin mode; mlterm's NP and PP move home
    // after vt_screen_goto_page.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    auto const [movement, target] = GENERATE(table<std::string_view, PageIndex>({
        { "\033[U", PageIndex(2) }, // NP
        { "\033[V", PageIndex(0) }, // PP
    }));
    INFO(e(movement));
    // Pages 1 and 3 scroll from line 2 to line 4. The cursor ends up on page 2, with origin mode set and
    // bold.
    mock.writeToScreen("\033[2;4r\033[3 P\033[2;4r\033[2 P\033[?6h\033[1m\033[3;5H");

    mock.writeToScreen(movement);
    REQUIRE(terminal.cursorPageIndex() == target);
    CHECK(terminal.currentScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) });
    CHECK(terminal.currentScreen().cursor().originMode);
    CHECK(terminal.currentScreen().cursor().graphicsRendition.flags.contains(CellFlag::Bold));
}

TEST_CASE("MultiPage.NP_PP.moveHomeOnTheLastAndTheFirstPage", "[screen]")
{
    // DEC STD 070's NP algorithm takes the highest page for one beyond it and then moves home; PP the first
    // page. Windows Terminal and mlterm move home after the page movement whatever page it reached.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    auto const [setup, movement, page] = GENERATE(table<std::string_view, std::string_view, PageIndex>({
        { "\033[15 P", "\033[U", PageIndex(14) }, // NP on page 15, the last
        { "", "\033[V", PageIndex(0) },           // PP on page 1
    }));
    INFO(e(movement));
    mock.writeToScreen(setup);
    mock.writeToScreen("\033[3;4H");
    mock.writeToScreen(movement);
    CHECK(terminal.cursorPageIndex() == page);
    CHECK(terminal.currentScreen().cursor().position == CellLocation {});
}

TEST_CASE("MultiPage.DECPCCM_coupling", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // DECPCCM is enabled by default — display follows cursor.
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));

    // Move to page 3. Display should follow.
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    CHECK(terminal.cursorPageIndex() == PageIndex(2));
    CHECK(terminal.displayedPageIndex() == PageIndex(2));

    // Disable DECPCCM: CSI ? 64 l
    mock.writeToScreen("\033[?64l"); // Reset DECPCCM
    CHECK(!terminal.isModeEnabled(DECMode::PageCursorCoupling));

    // Move to page 5. Display should NOT follow.
    mock.writeToScreen("\033[5 P"); // PPA to page 5
    CHECK(terminal.cursorPageIndex() == PageIndex(4));
    CHECK(terminal.displayedPageIndex() == PageIndex(2)); // still showing page 3

    // Re-enable DECPCCM: CSI ? 64 h — display should sync to cursor page.
    mock.writeToScreen("\033[?64h"); // Set DECPCCM
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
    CHECK(terminal.displayedPageIndex() == PageIndex(4)); // synced to cursor page
}

// The viewport is DEC STD 070's window: it shows the page on display, and "cannot be moved beyond the
// boundaries of the page" (SU/SD). Where the cursor writes plays no part in it. The primary page is the only
// one with a history; 40 lines on a 5-row page leave "line 36" to "line 39" on it and "line 0" to "line 35"
// above. The crossfade is off, so that a frame shows the page on display rather than the one it replaced.

TEST_CASE("MultiPage.DECPCCM_reset.anotherPageOnDisplayHasNothingToScrollInto", "[screen]")
{
    // The cursor writes to the primary page, whose history grows, while page 3, which has none, is on
    // display.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    mock.writeToScreen("\033[3 Ppage three");
    mock.writeToScreen("\033[?64l\033[1 P"); // DECPCCM reset, then the cursor back to page 1
    REQUIRE(terminal.cursorPageIndex() == PageIndex(0));
    REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
    printLines(mock, "line", 40);
    REQUIRE(terminal.primaryScreen().historyLineCount() == LineCount(36));

    CHECK(terminal.viewport().scrollingDisabled());
    CHECK_FALSE(terminal.viewport().scrollUp(LineCount(20)));
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));

    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "page three");
}

TEST_CASE("MultiPage.DECPCCM_reset.thePrimaryPageOnDisplayScrollsWhileTheCursorIsElsewhere", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    printLines(mock, "line", 40);
    mock.writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, then the cursor to page 3
    REQUIRE(terminal.cursorPageIndex() == PageIndex(2));
    REQUIRE(terminal.displayedPageIndex() == PageIndex(0));

    CHECK_FALSE(terminal.viewport().scrollingDisabled());
    CHECK(terminal.viewport().scrollUp(LineCount(20)));
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(20));

    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "line 16\nline 17\nline 18\nline 19\nline 20");

    // Setting DECPCCM puts the cursor's page on display, and the window comes back inside it.
    mock.writeToScreen("\033[?64h");
    REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());
}

TEST_CASE("MultiPage.DECPCCM_reset.outputToAnotherPageLeavesTheWindowWhereItIs", "[screen]")
{
    // Page 1 on display, scrolled back, while the cursor writes to page 3. Page 3 has no history, so every
    // line feed at its bottom scrolls it, but page 1's content does not move, and neither does the window
    // onto it: whether the view stays with what it shows or follows new output, only output to the page on
    // display moves it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    mock.following = GENERATE(OutputFollowing::Stay, OutputFollowing::Follow);
    terminal.settings().autoScrollOnUpdate = mock.following == OutputFollowing::Follow;
    printLines(mock, "line", 40);
    mock.writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, then the cursor to page 3
    REQUIRE(terminal.viewport().scrollUp(LineCount(20)));

    printLines(mock, "three", 20);

    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(20));
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "line 16\nline 17\nline 18\nline 19\nline 20");
}

namespace
{
/// Selects the first ten columns of row @p line of the page on display, as a drag would.
/// @param mock The terminal.
/// @param line The row.
void selectOnDisplay(MockTerm<>& mock, int line)
{
    mock.terminal.setSelector(std::make_unique<LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) },
        [] {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(line), .column = ColumnOffset(9) });
}

/// The order in which one batch of output scrolls a screen and moves the cursor between the page on display
/// and the host-writable status line.
enum class BatchOrder : uint8_t
{
    ScrollsThePageThenLeaves = 0, ///< The page on display scrolls, then the cursor moves to the status line.
    ScrollsTheStatusLineThenReturns, ///< The status line scrolls, then the cursor moves back to the page.
};
} // namespace

TEST_CASE("MultiPage.DECPCCM_reset.outputToAnotherPageLeavesTheViewersStateAlone", "[screen]")
{
    // The vi cursor and the selection are on the page on display, as the window is. Line feeds at page 3's
    // bottom scroll page 3 alone, so neither moves -- with the window at the bottom too, where nothing else
    // asks which page scrolled.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    printLines(mock, "line", 40);
    mock.writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, then the cursor to page 3
    REQUIRE(terminal.displayedPageIndex() == PageIndex(0));
    auto const viCursor = CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) };
    terminal.moveNormalModeCursorTo(viCursor);
    selectOnDisplay(mock, 1);

    printLines(mock, "three", 20);

    CHECK(terminal.normalModeCursorPosition() == viCursor);
    REQUIRE(terminal.selector() != nullptr);
    CHECK(terminal.selector()->from().line == LineOffset(1));
    CHECK(terminal.selector()->to().line == LineOffset(1));
    CHECK(terminal.extractSelectionText() == "line 37");
}

TEST_CASE("MultiPage.statusLineOutputLeavesThePageOnDisplayAlone", "[screen]")
{
    // DECSASD sends output to the host-writable status line, a one-row screen of its own: each line feed
    // there scrolls it, and nothing on the page on display -- the window, the vi cursor, the selection.
    // The status line is shown first: it takes a row from the page, which moves what is on it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[2$~"); // DECSSDT host-writable
    printLines(mock, "line", 40);
    mock.writeToScreen("\033[1$}"); // DECSASD status line
    REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::StatusLine);
    REQUIRE(terminal.viewport().scrollUp(LineCount(5)));
    auto const viCursor = CellLocation { .line = LineOffset(-2), .column = ColumnOffset(3) };
    terminal.moveNormalModeCursorTo(viCursor);
    selectOnDisplay(mock, -3);
    REQUIRE(terminal.extractSelectionText() == "line 33");
    auto const scrollOffset = terminal.viewport().scrollOffset(); // the vi cursor's own margin moved it
    REQUIRE(scrollOffset > ScrollOffset(0));

    mock.writeToScreen("x\r\nx\r\nx\r\n");

    CHECK(terminal.viewport().scrollOffset() == scrollOffset);
    CHECK(terminal.normalModeCursorPosition() == viCursor);
    REQUIRE(terminal.selector() != nullptr);
    CHECK(terminal.selector()->from().line == LineOffset(-3));
    CHECK(terminal.extractSelectionText() == "line 33");
}

TEST_CASE("MultiPage.theViewFollowsABatchThatScrolledThePageOnDisplay", "[screen]")
{
    // A view scrolled back follows new output (autoScrollOnUpdate) where the batch scrolled the page on
    // display, wherever the cursor ends it. DECSASD moves the cursor between the page and the host-writable
    // status line, a one-row screen of its own, within one batch.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().autoScrollOnUpdate = true;
    mock.following = OutputFollowing::Follow;
    mock.writeToScreen("\033[2$~"); // DECSSDT host-writable, before the output: it takes a row from the page
    printLines(mock, "line", 40);

    switch (GENERATE(BatchOrder::ScrollsThePageThenLeaves, BatchOrder::ScrollsTheStatusLineThenReturns))
    {
        case BatchOrder::ScrollsThePageThenLeaves:
            REQUIRE(terminal.viewport().scrollUp(LineCount(20)));
            mock.writeToScreen("more 1\r\nmore 2\r\n\033[1$}"); // one batch, ending in DECSASD status line
            REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::StatusLine);
            CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));
            break;
        case BatchOrder::ScrollsTheStatusLineThenReturns:
            mock.writeToScreen("\033[1$}"); // DECSASD status line
            REQUIRE(terminal.viewport().scrollUp(LineCount(20)));
            mock.writeToScreen("x\r\nx\r\n\033[0$}"); // one batch, ending in DECSASD main display
            REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::Main);
            CHECK(terminal.viewport().scrollOffset() == ScrollOffset(20));
            break;
    }
}

TEST_CASE("MultiPage.DECPCCM_reset.aPageIsNotFollowedForWhatItScrolledBeforeComingOnDisplay", "[screen]")
{
    // Page 1 scrolls while page 3 is on display. Brought on display, taken back into its history, and written
    // to without scrolling, it stays where the user took it: what it scrolled before it came on display is
    // no output to follow.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().autoScrollOnUpdate = true;
    mock.following = OutputFollowing::Follow;
    printLines(mock, "line", 20);
    mock.writeToScreen("\033[3 P\033[?64l\033[1 P"); // page 3 on display, DECPCCM reset, the cursor to page 1
    REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
    REQUIRE(terminal.cursorPageIndex() == PageIndex(0));
    mock.writeToScreen("\033[5;1H");
    printLines(mock, "more", 20); // scrolls page 1, not on display

    mock.writeToScreen("\033[?64h"); // DECPCCM set: the cursor's page, page 1, comes on display
    REQUIRE(terminal.displayedPageIndex() == PageIndex(0));
    REQUIRE(terminal.viewport().scrollUp(LineCount(10)));
    mock.writeToScreen("x"); // a batch that scrolls nothing

    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(10));
}

TEST_CASE("MultiPage.DECPCCM_set.theViewportScrollsAsItDid", "[screen]")
{
    // Coupled, the page on display is the cursor's, so the window and the cursor are on one page.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    printLines(mock, "line", 40);

    CHECK_FALSE(terminal.viewport().scrollingDisabled());
    CHECK(terminal.viewport().scrollUp(LineCount(20)));
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(20));
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "line 16\nline 17\nline 18\nline 19\nline 20");

    SECTION("a page with no history comes on display: the window comes back inside it")
    {
        mock.writeToScreen("\033[3 P\033[Hpage three");
        REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
        CHECK(terminal.viewport().scrollingDisabled());
        CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));
        terminal.refreshRenderBuffer();
        CHECK(trimmedTextScreenshot(mock) == "page three");

        mock.writeToScreen("\033[1 P");
        CHECK_FALSE(terminal.viewport().scrollingDisabled());
        CHECK(terminal.viewport().scrollUp(LineCount(20)));
        CHECK(terminal.viewport().scrollOffset() == ScrollOffset(20));
    }

    SECTION("the alternate screen comes on display: the window comes back inside it")
    {
        mock.writeToScreen("\033[?1049h\033[Hfull screen");
        CHECK(terminal.viewport().scrollingDisabled());
        CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));
        terminal.refreshRenderBuffer();
        CHECK(trimmedTextScreenshot(mock) == "full screen");
    }
}

TEST_CASE("MultiPage.theViewportScrollsThePageOnDisplayWhileTheStatusLineIsWritten", "[screen]")
{
    // DECSASD sends output to the host-writable status line, which is no page: the window stays on the page
    // on display, and scrolls as far into its history as it did before.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    auto& terminal = mock.terminal;
    printLines(mock, "line", 40);
    mock.writeToScreen("\033[2$~\033[1$}"); // DECSSDT host-writable, DECSASD status line
    REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::StatusLine);

    CHECK(terminal.viewport().scrollUp(LineCount(5)));
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(5));
}

// What is under the pointer is on the page on display: the pointer is a position on the page seen through the
// window -- DEC STD 070's locator reports it with that page's number (DECLRP's Pp) -- whichever page the
// cursor writes to.

namespace
{
/// Which of the two pages showOnePageOfTwo() leaves on display. The cursor is on the other one.
enum class OnDisplay : uint8_t
{
    Primary = 0, ///< Page 1 on display, the cursor on page 3.
    Other,       ///< Page 3 on display, the cursor on page 1.
};

/// Writes @p primary on page 1 and @p other on page 3, each from the page's home position, then resets
/// DECPCCM and leaves the page @p shown names on display while the cursor writes to the other one.
/// @param mock The terminal, its cursor home on page 1.
/// @param primary What page 1 holds.
/// @param other What page 3 holds.
/// @param shown Which page stays on display.
void showOnePageOfTwo(MockTerm<>& mock, std::string_view primary, std::string_view other, OnDisplay shown)
{
    mock.terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    mock.writeToScreen(primary);
    mock.writeToScreen("\033[3 P\033[H"); // PPA 3, coupled: page 3 on display; the cursor home on it
    mock.writeToScreen(other);
    switch (shown)
    {
        case OnDisplay::Primary: mock.writeToScreen("\033[1 P\033[?64l\033[3 P"); break;
        case OnDisplay::Other: mock.writeToScreen("\033[?64l\033[1 P"); break;
    }
    auto const expected = shown == OnDisplay::Primary ? PageIndex(0) : PageIndex(2);
    REQUIRE(mock.terminal.displayedPageIndex() == expected);
    REQUIRE(mock.terminal.cursorPageIndex() != expected);
}

/// Records a finished block that ran on another machine, and stamps row 0 of page 1 with it, as a daemon
/// snapshot hands a mirror its blocks.
/// @param mock The terminal.
void stampForeignBlockOnRow0(MockTerm<>& mock)
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId(7);
    record.state = CommandBlockState::Finished;
    record.workingDirectory =
        WorkingDirectorySnapshot { .path = "/elsewhere", .locality = ContextLocality::Foreign };
    mock.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
    mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)).adoptBlock(CommandBlockId(7));
}

/// The URI of the OSC 8 link under the pointer, as hyperlinkIdUnderPointer() names it.
/// @param terminal The terminal.
/// @return The link's URI; empty while the pointer is over none.
[[nodiscard]] std::string uriUnderPointer(Terminal const& terminal)
{
    auto const link = terminal.hyperlinks().hyperlinkById(terminal.hyperlinkIdUnderPointer());
    return link ? link->uri : std::string {};
}
} // namespace

TEST_CASE("MultiPage.DECPCCM_reset.theLinkUnderThePointerIsOnThePageOnDisplay", "[screen]")
{
    // Both pages carry a link in the same cells; the pointer, the hover tooltip and the frame's hover
    // decoration all name the one on display.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& terminal = mock.terminal;
    constexpr auto PageOne = "\033]8;;https://one.example/\033\\link\033]8;;\033\\"sv;
    constexpr auto PageThree = "\033]8;;https://three.example/\033\\link\033]8;;\033\\"sv;

    auto const [shown, uri] = GENERATE(table<OnDisplay, std::string_view>({
        { OnDisplay::Primary, "https://one.example/" },
        { OnDisplay::Other, "https://three.example/" },
    }));
    showOnePageOfTwo(mock, PageOne, PageThree, shown);
    pointAt(mock, 0, 1);

    CHECK(uriUnderPointer(terminal) == uri);
    auto const hovering = terminal.tryGetHoveringHyperlink();
    REQUIRE(hovering != nullptr);
    CHECK(hovering->uri == uri);
    auto const cells = renderedRow(mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(3).linkRole == LinkDecorationRole::Hover);
}

TEST_CASE("MultiPage.DECPCCM_reset.thePathUnderThePointerIsOnThePageOnDisplay", "[screen]")
{
    // Row 0 of page 1 belongs to a block that ran on another machine, which refuses its paths; row 0 of page
    // 3 belongs to no block.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& terminal = mock.terminal;

    SECTION("page 1 on display")
    {
        showOnePageOfTwo(mock, "see notes.txt", "see other.txt", OnDisplay::Primary);
        stampForeignBlockOnRow0(mock);
        pointAt(mock, 0, 6);

        CHECK(terminal.pathTextAtMousePosition() == "notes.txt");
        CHECK(terminal.localPathAtMousePosition() == std::unexpected(PathRefusal::RanElsewhere));
    }

    SECTION("page 3 on display")
    {
        showOnePageOfTwo(mock, "see notes.txt", "see other.txt", OnDisplay::Other);
        stampForeignBlockOnRow0(mock);
        pointAt(mock, 0, 6);

        CHECK(terminal.pathTextAtMousePosition() == "other.txt");
        CHECK(terminal.localPathAtMousePosition().has_value());
    }
}

TEST_CASE("MultiPage.DECPCCM_reset.theBlockUnderThePointerIsOnThePageOnDisplay", "[screen]")
{
    // `ls` runs on page 1; page 3 holds no block.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    runCommand(mock, "ls", { "a", "b" });

    SECTION("page 1 on display")
    {
        mock.writeToScreen("\033[?64l\033[3 P");
        pointAt(mock, 1, 0);
        CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Output) == "a\nb");
    }

    SECTION("page 3 on display")
    {
        mock.writeToScreen("\033[3 P\033[?64l\033[1 P");
        pointAt(mock, 1, 0);
        CHECK(terminal.commandBlockText(
                  CommandBlockTarget::Pointer, CommandBlockPart::Output, CaptureRendition::PlainText)
              == std::unexpected(CommandBlockActionError::NoBlock));
    }
}

TEST_CASE("MultiPage.DECPCCM_reset.hintModeLabelsThePageOnDisplay", "[screen]")
{
    // The labels are drawn over the page on display, so that is the page hint mode scans.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    auto& terminal = mock.terminal;

    auto const [shown, url] = GENERATE(table<OnDisplay, std::string_view>({
        { OnDisplay::Primary, "https://one.example/" },
        { OnDisplay::Other, "https://three.example/" },
    }));
    showOnePageOfTwo(mock, "visit https://one.example/ now", "visit https://three.example/ now", shown);

    terminal.activateHintMode(
        HintModeRequest { .patterns = HintModeHandler::builtinPatterns(), .action = HintAction::Copy });
    REQUIRE(terminal.hintMatches().size() == 1);
    CHECK(terminal.hintMatches().at(0).matchedText == url);
}

TEST_CASE("MultiPage.thePointerIsOnThePageOnDisplayWhileTheStatusLineIsWritten", "[screen]")
{
    // DECSASD sends output to the one-row host-writable status line; the pointer is still over the page on
    // display, and a cell below that row is a cell of it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[2$~\033[1$}"); // DECSSDT host-writable, DECSASD status line
    REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::StatusLine);

    pointAt(mock, 2, 3);
    CHECK(terminal.currentMouseGridPosition()
          == CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) });
}

TEST_CASE("MultiPage.pageMovementOnTheStatusLineLeavesTheMainDisplayAlone", "[screen]")
{
    // DEC STD 070 ch. 14 §2.3: with the host-writable status line selected, "most ANSI control functions
    // received will affect only the Status Display", a one-line display of its own on which vertical cursor
    // positioning is ignored. A page movement there is not the main display's.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto& terminal = mock.terminal;
    auto const movement = GENERATE(std::string_view { "\033[U" },   // NP
                                   std::string_view { "\033[V" },   // PP
                                   std::string_view { "\033[3 P" }, // PPA 3
                                   std::string_view { "\033[ Q" },  // PPR
                                   std::string_view { "\033[ R" }); // PPB
    INFO(e(movement));
    mock.writeToScreen("\033[2 Pmain\033[2$~\033[1$}"); // page 2; DECSSDT host-writable, DECSASD status line
    REQUIRE(terminal.activeStatusDisplay() == ActiveStatusDisplay::StatusLine);

    mock.writeToScreen(movement);
    CHECK(terminal.cursorPageIndex() == PageIndex(1));
    CHECK(terminal.displayedPageIndex() == PageIndex(1));
    CHECK(&terminal.currentScreen() == &terminal.hostWritableStatusLineDisplay());
    mock.writeToScreen("status\033[0$}!"); // DECSASD main display
    CHECK(terminal.hostWritableStatusLineDisplay().grid().lineText(LineOffset(0)).starts_with("status"));
    CHECK(terminal.pageAt(PageIndex(1)).grid().lineText(LineOffset(0)).starts_with("main!"));
}

TEST_CASE("MultiPage.DECSC_DECRC_onTheStatusLineLeaveTheMainDisplaysPageAlone", "[screen]")
{
    // DEC STD 070 ch. 14 §2.2: the host-writable status line keeps its own Cursor Save Buffer (DECSC and
    // DECRC). The page the main display's cursor saved is not the status line's to save or restore.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto& terminal = mock.terminal;

    SECTION("DECRC on the status line does not move the main display's cursor to the page it saved")
    {
        mock.writeToScreen("\0337\033[3 P");                 // DECSC on page 1, then to page 3
        mock.writeToScreen("\033[2$~\033[1$}\0338\033[0$}"); // DECRC on the status line
        CHECK(terminal.cursorPageIndex() == PageIndex(2));
    }

    SECTION("DECSC on the status line does not save the main display's page")
    {
        mock.writeToScreen("\033[3 P");                      // to page 3, nothing saved: DECRC means page 1
        mock.writeToScreen("\033[2$~\033[1$}\0337\033[0$}"); // DECSC on the status line
        mock.writeToScreen("\0338");                         // DECRC on the main display
        CHECK(terminal.cursorPageIndex() == PageIndex(0));
    }
}

TEST_CASE("MultiPage.theAlternateScreenSwitchedOnTheStatusLineCarriesTheMainCursor", "[screen]")
{
    // xterm switches the main display's buffer under an active status line and leaves the status line active
    // (SwitchBufPtrs copies the status row across); its cursor is the main display's. The status line keeps
    // its own active position (DEC STD 070 ch. 14 §2.2), so it is not that cursor which goes along.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto& terminal = mock.terminal;
    auto const mode = GENERATE(47, 1047, 1049);
    INFO(std::format("mode {}", mode));
    mock.writeToScreen("\033[2;3H\033[2$~\033[1$}status"); // the main cursor at 2;3, the status line's at 1;7

    mock.writeToScreen(std::format("\033[?{}h", mode));
    CHECK(terminal.isAlternateScreenBufferInUse());
    CHECK(&terminal.currentScreen() == &terminal.hostWritableStatusLineDisplay());
    mock.writeToScreen("!\033[0$}");
    CHECK(terminal.hostWritableStatusLineDisplay().grid().lineText(LineOffset(0)).starts_with("status!"));
    CHECK(terminal.currentScreen().cursor().position == CellLocation { LineOffset(1), ColumnOffset(2) });
}

TEST_CASE("MultiPage.DECRQDE_response", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        mock.writeToScreen("\033[\"v"); // DECRQDE
        // Expected response: CSI 5 ; 10 ; 1 ; 1 ; 1 " w
        CHECK(std::string("\033[5;10;1;1;1\"w") == terminal.peekInput());
    }

    SECTION("on page 3")
    {
        mock.writeToScreen("\033[3 P"); // PPA to page 3
        mock.writeToScreen("\033[\"v"); // DECRQDE
        CHECK(std::string("\033[5;10;1;1;3\"w") == terminal.peekInput());
    }
}

TEST_CASE("MultiPage.DECXCPR_page_number", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        mock.writeToScreen("\033[2;3H"); // CUP to row 2, col 3
        terminal.primaryScreen().reportExtendedCursorPosition();
        // Response includes page number: CSI row ; col ; page R
        CHECK(std::string("\033[?2;3;1R") == terminal.peekInput());
    }

    SECTION("on page 4")
    {
        mock.writeToScreen("\033[4 P");  // PPA to page 4
        mock.writeToScreen("\033[3;5H"); // CUP to row 3, col 5
        terminal.pageAt(PageIndex(3)).reportExtendedCursorPosition();
        CHECK(std::string("\033[?3;5;4R") == terminal.peekInput());
    }
}

TEST_CASE("MultiPage.DECCIR_page_number", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    REQUIRE(terminal.peekInput().empty());

    SECTION("on page 1")
    {
        // DECRQPSR with param 1 requests DECCIR
        mock.writeToScreen("\033[1$w");
        auto const response = terminal.peekInput();
        // Response: DCS 1 $ u Pr;Pc;Pp;... ST — verify Pp=1
        // Format: ESC P 1 $ u line;col;page;...
        CHECK(response.starts_with("\033P1$u1;1;1;"));
    }

    SECTION("on page 3")
    {
        mock.writeToScreen("\033[3 P"); // PPA to page 3
        mock.writeToScreen("\033[1$w"); // DECRQPSR for DECCIR
        auto const response = terminal.peekInput();
        // Verify Pp=3 in the DCS response
        CHECK(response.starts_with("\033P1$u1;1;3;"));
    }
}

TEST_CASE("MultiPage.theAlternateScreenReportsPageOne", "[screen]")
{
    // The alternate screen is no DEC page, and the page-number reports name it page 1: xterm's DECXCPR
    // "assumes the default page", Windows Terminal's PageManager::Get answers for its alternate buffer with
    // "a fixed page number of 1", and mlterm regards its alternate edit as page 1. Entered from page 3 here,
    // so that page 1 is not merely where the cursor came from.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[3 P\033[?1049h\033[2;4H");
    REQUIRE(terminal.isAlternateScreenBufferInUse());
    auto const replyTo = [&](std::string_view request) {
        mock.discardPendingReplies();
        mock.writeToScreen(request);
        return terminal.peekInput();
    };

    CHECK(e(replyTo("\033[?6n")) == e("\033[?2;4;1R"));       // DECXCPR
    CHECK(e(replyTo("\033[\"v")) == e("\033[5;10;1;1;1\"w")); // DECRQDE
    CHECK(replyTo("\033[1$w").starts_with("\033P1$u2;4;1;")); // DECCIR
}

TEST_CASE("MultiPage.DECSC_DECRC_saves_restores_page", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Move to page 3 and save cursor (DECSC = ESC 7).
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    mock.writeToScreen("\0337");    // DECSC

    CHECK(terminal.cursorPageNumber() == 3);

    // Switch to page 1.
    mock.writeToScreen("\033[1 P"); // PPA to page 1
    CHECK(terminal.cursorPageNumber() == 1);

    // Restore cursor (DECRC = ESC 8) — should return to page 3.
    mock.writeToScreen("\0338"); // DECRC
    CHECK(terminal.cursorPageNumber() == 3);
}

TEST_CASE("MultiPage.DECRC_without_DECSC_defaults_to_page1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Move to page 4 without saving cursor.
    mock.writeToScreen("\033[4 P"); // PPA to page 4
    CHECK(terminal.cursorPageNumber() == 4);

    // DECRC without prior DECSC should restore to page 1 (default saved page = 0).
    mock.writeToScreen("\0338"); // DECRC
    CHECK(terminal.cursorPageNumber() == 1);
}

TEST_CASE("MultiPage.DECRC_restoresWhatDECSCSavedOnItsPage", "[screen]")
{
    // DEC STD 070 keeps one cursor save buffer, the page in its position: DECRC returns to the page DECSC
    // saved and restores there what DECSC saved -- the position, the rendition, the character sets. Windows
    // Terminal restores the page and then the rest onto it (CursorRestoreState); xterm keeps one buffer per
    // screen buffer.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[3 P\033[2;3H\033[1m\033(0\0337");    // page 3, at 2;3, bold, line drawing: DECSC
    mock.writeToScreen("\033[4;4H\033[m\033(B\033[1 P\033[5;5H"); // elsewhere, plain, ASCII, on page 1

    mock.writeToScreen("\0338"); // DECRC
    REQUIRE(terminal.cursorPageIndex() == PageIndex(2));
    CHECK(terminal.currentScreen().cursor().position
          == CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) });
    CHECK(terminal.currentScreen().cursor().graphicsRendition.flags.contains(CellFlag::Bold));
    mock.writeToScreen("q");
    CHECK(terminal.pageAt(PageIndex(2)).grid().lineText(LineOffset(1)) == "  \u2500       ");
}

TEST_CASE("MultiPage.RIS_and_DECSTR_resetTheSaveBufferPageIncluded", "[screen]")
{
    // DEC STD 070 DECSC: "On initialization or reset the values of the save buffer are cleared to the
    // following state: position.line = 1; position.column = 1; position.page = 1; ...", and "Execution of
    // Soft Terminal Reset (DECSTR) also causes the Save Buffer to be set to the default values". Received on
    // the status line too: DEC STD 070's soft_terminal_reset() returns to the main display before it resets
    // the buffer (ch. 4).
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    auto const reset = GENERATE(std::string_view { "\033c" },                    // RIS
                                std::string_view { "\033[!p" },                  // DECSTR
                                std::string_view { "\033[2$~\033[1$}\033[!p" }); // DECSTR on the status line
    INFO(e(reset));

    SECTION("the page")
    {
        mock.writeToScreen("\033[3 P\0337"); // DECSC on page 3
        mock.writeToScreen(reset);
        mock.writeToScreen("\0338"); // DECRC
        CHECK(terminal.cursorPageIndex() == PageIndex(0));
    }

    SECTION("the position")
    {
        mock.writeToScreen("\033[2;3H\0337"); // DECSC at 2;3 on page 1
        mock.writeToScreen(reset);
        mock.writeToScreen("\033[4;5H\0338"); // DECRC
        CHECK(terminal.currentScreen().cursor().position == CellLocation {});
    }
}

TEST_CASE("MultiPage.DECCRA_cross_page_copy", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Write "ABCDE" on page 1.
    mock.writeToScreen("ABCDE");

    // Move to page 2.
    mock.writeToScreen("\033[2 P"); // PPA 2
    CHECK(terminal.cursorPageIndex() == PageIndex(1));

    // Page 2 should be empty.
    auto const page2text = terminal.pageAt(PageIndex(1)).renderMainPageText();
    CHECK(page2text == "          \n          \n          \n");

    // Copy from page 1 to page 2 using DECCRA.
    // DECCRA: CSI Pts;Pls;Pbs;Prs;Pps;Ptd;Pld;Ppd $ v
    // Source: top=1, left=1, bottom=1, right=5, page=1
    // Dest: top=2, left=1, page=2
    mock.writeToScreen("\033[1;1;1;5;1;2;1;2$v"); // DECCRA

    // Page 2, row 2 should now have "ABCDE".
    auto const page2after = terminal.pageAt(PageIndex(1)).renderMainPageText();
    CHECK(page2after == "          \nABCDE     \n          \n");
}

TEST_CASE("MultiPage.alternate_screen_compatibility", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Write content on primary screen.
    mock.writeToScreen("Prima");

    // Switch to alternate screen via DECSET 1049.
    mock.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK(terminal.cursorPageIndex() == AlternateScreenPageIndex);

    // Write content on alternate screen.
    mock.writeToScreen("Alt!!");

    // Switch back.
    mock.writeToScreen("\033[?1049l");
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.cursorPageIndex() == PageIndex(0));

    // Primary content should be preserved.
    CHECK(terminal.primaryScreen().renderMainPageText() == "Prima\n     \n     \n");
}

TEST_CASE("MultiPage.pageMovementStaysOnTheAlternateScreen", "[screen]")
{
    // DEC STD 070 defines no alternate screen. Windows Terminal, the one terminal with both VT420 pages and
    // an alternate buffer, keeps page movement on it: PageManager::MoveTo returns at once outside the main
    // buffer, and NextPage/PrecedingPage still move the cursor home after it. That is DEC's page movement on
    // a display of one page: NP and PP home the cursor, PPA, PPR and PPB change nothing but the page, which
    // stays.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    auto const [movement, column] = GENERATE(table<std::string_view, int>({
        { "\033[U", 0 },   // NP
        { "\033[V", 0 },   // PP
        { "\033[3 P", 3 }, // PPA 3
        { "\033[ Q", 3 },  // PPR
        { "\033[ R", 3 },  // PPB
    }));
    INFO(e(movement));
    mock.writeToScreen("main\033[?1049h\033[Halt");
    REQUIRE(terminal.cursorPageIndex() == AlternateScreenPageIndex);

    mock.writeToScreen(movement);
    CHECK(terminal.cursorPageIndex() == AlternateScreenPageIndex);
    CHECK(terminal.displayedPageIndex() == AlternateScreenPageIndex);
    CHECK(terminal.currentScreen().cursor().position
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(column) });
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "alt");

    mock.writeToScreen("\033[?1049l");
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "main");
}

TEST_CASE("MultiPage.theAlternateScreenIsEnteredFromAnyPage", "[screen]")
{
    // The alternate screen is one buffer beside the pages, entered from wherever the cursor is: xterm and
    // mlterm guard the switch on the buffer in use, not on the page. Leaving it, the cursor returns to the
    // page it came from, as Windows Terminal's ?1049 restores the page it saved with the cursor. DEC STD 070
    // defines no alternate screen; its DECSC, which saves the page with the active position, is the page rule
    // that ?1049 applies to the switch.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    auto const mode = GENERATE(47, 1047, 1049);
    INFO(std::format("mode {}", mode));
    auto const modeState = [&] {
        mock.discardPendingReplies();
        mock.writeToScreen(std::format("\033[?{}$p", mode)); // DECRQM
        return terminal.peekInput();
    };
    mock.writeToScreen("one\033[2 P\033[2;3Htwo");
    REQUIRE(terminal.cursorPageIndex() == PageIndex(1));
    auto const cursorOnPageTwo = terminal.currentScreen().cursor().position;

    mock.writeToScreen(std::format("\033[?{}h", mode));
    CHECK(terminal.cursorPageIndex() == AlternateScreenPageIndex);
    CHECK(terminal.displayedPageIndex() == AlternateScreenPageIndex);
    CHECK(e(modeState()) == e(std::format("\033[?{};1$y", mode)));
    mock.writeToScreen("\033[3;5Halt");
    auto const cursorOnTheAlternateScreen = terminal.currentScreen().cursor().position;

    mock.writeToScreen(std::format("\033[?{}l", mode));
    CHECK(terminal.cursorPageIndex() == PageIndex(1));
    CHECK(terminal.displayedPageIndex() == PageIndex(1));
    CHECK(e(modeState()) == e(std::format("\033[?{};2$y", mode)));
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "\n  two");
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset(0)) == "one       ");
    // 47 and 1047 carry the cursor across; 1049 leaves the page's own cursor where it was, its implicit
    // DECRC.
    CHECK(terminal.currentScreen().cursor().position
          == (mode == 1049 ? cursorOnPageTwo : cursorOnTheAlternateScreen));
}

TEST_CASE("MultiPage.DECPCCM_reset.leavingTheAlternateScreenResumesPaging", "[screen]")
{
    // Page 3 on display, the cursor on page 2. Leaving the alternate screen returns both where they were:
    // Windows Terminal restores the cursor's page, and moves the page on display only while DECPCCM is set.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    auto const mode = GENERATE(47, 1047, 1049);
    INFO(std::format("mode {}", mode));
    mock.writeToScreen("\033[3 Pthree\033[?64l\033[2 P\033[Htwo");
    REQUIRE(terminal.cursorPageIndex() == PageIndex(1));
    REQUIRE(terminal.displayedPageIndex() == PageIndex(2));

    mock.writeToScreen(std::format("\033[?{}h", mode));
    REQUIRE(terminal.displayedPageIndex() == AlternateScreenPageIndex);
    mock.writeToScreen(std::format("\033[?{}l", mode));
    CHECK(terminal.cursorPageIndex() == PageIndex(1));
    CHECK(terminal.displayedPageIndex() == PageIndex(2));
    CHECK(terminal.pageAt(PageIndex(1)).grid().lineText(LineOffset(0)) == "two       ");
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "three");
}

TEST_CASE("MultiPage.risLeavesTheAlternateScreenForPageOne", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    mock.writeToScreen("\033[2 P\033[?1049h");
    REQUIRE(terminal.isAlternateScreenBufferInUse());

    mock.writeToScreen("\033c"); // RIS
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));
    mock.writeToScreen("\033[?1049l"); // nothing to leave
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));
}

TEST_CASE("MultiPage.DECPCCM_reset.theAlternateScreenIsShownWhileInUse", "[screen]")
{
    // DECPCCM governs page movement (NP, PP, PPA, PPR, PPB). The alternate screen is no DEC page, and
    // switching to it none of those: xterm shows the buffer it switches to, and Windows Terminal keeps its
    // alternate buffer outside paging. Leaving it, Contour follows Windows Terminal, the one reference with
    // both pages and an alternate buffer: its cursor restore moves the page on display only while DECPCCM is
    // set, so with DECPCCM reset the page that was on display before is shown again.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;
    terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Classic;
    auto const mode = GENERATE(47, 1047, 1049);
    INFO(std::format("mode {}", mode));
    mock.writeToScreen("main");
    auto shownAfterwards = PageIndex(0);
    auto textAfterwards = std::string("main");
    auto beforeLeaving = std::string {};

    SECTION("the primary page on display")
    {
        mock.writeToScreen("\033[?64l"); // DECPCCM reset
    }

    SECTION("another page on display")
    {
        mock.writeToScreen(
            "\033[3 P\033[Hthree\033[?64l\033[1 P"); // page 3 stays on display, the cursor on page 1
        REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
        shownAfterwards = PageIndex(2);
        textAfterwards = "three";
    }

    SECTION("entered while DECPCCM is set, left while it is reset: the alternate screen goes off display")
    {
        beforeLeaving = "\033[?64l";
    }

    SECTION("another page on display, DECPCCM set before leaving: the display follows the cursor")
    {
        mock.writeToScreen("\033[3 P\033[Hthree\033[?64l\033[1 P");
        REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
        beforeLeaving = "\033[?64h";
    }

    mock.writeToScreen(std::format("\033[?{}h\033[Halt", mode));
    REQUIRE(terminal.isAlternateScreen());
    CHECK(terminal.displayedPageIndex() == AlternateScreenPageIndex);
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == "alt");
    CHECK(terminal.renderBuffer().get().cursor.has_value());

    mock.writeToScreen(beforeLeaving);
    mock.writeToScreen(std::format("\033[?{}l", mode));
    REQUIRE(terminal.isPrimaryScreen());
    CHECK(terminal.displayedPageIndex() == shownAfterwards);
    terminal.refreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock) == textAfterwards);
}

TEST_CASE("MultiPage.hard_reset", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Move to page 3 and write.
    mock.writeToScreen("\033[3 P"); // PPA to page 3
    mock.writeToScreen("Test!");
    CHECK(terminal.cursorPageIndex() == PageIndex(2));

    // Hard reset.
    mock.writeToScreen("\033c"); // RIS

    // Should be back on page 1, DECPCCM enabled.
    CHECK(terminal.cursorPageIndex() == PageIndex(0));
    CHECK(terminal.displayedPageIndex() == PageIndex(0));
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.isModeEnabled(DECMode::PageCursorCoupling));
}

TEST_CASE("MultiPage.page_content_isolation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& terminal = mock.terminal;

    // Write on page 1.
    mock.writeToScreen("Page1");

    // Move to page 2 and write.
    mock.writeToScreen("\033[1U"); // NP 1
    mock.writeToScreen("Page2");

    // Move to page 3 and write.
    mock.writeToScreen("\033[1U"); // NP 1
    mock.writeToScreen("Page3");

    // Go back to page 1 and verify isolation.
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.primaryScreen().renderMainPageText() == "Page1\n     \n");

    // Go to page 2 and verify.
    mock.writeToScreen("\033[2 P"); // PPA 2
    CHECK(terminal.pageAt(PageIndex(1)).renderMainPageText() == "Page2\n     \n");

    // Go to page 3 and verify.
    mock.writeToScreen("\033[3 P"); // PPA 3
    CHECK(terminal.pageAt(PageIndex(2)).renderMainPageText() == "Page3\n     \n");
}

TEST_CASE("MultiPage.PerPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom top/bottom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 2 — it should have default (full-screen) margins.
    mock.writeToScreen("\033[2 P"); // PPA 2
    auto& page2 = terminal.pageAt(PageIndex(1));
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });
    CHECK(page2.margin().horizontal == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Set different margins on page 2.
    mock.writeToScreen("\033[3;5r"); // DECSTBM(3, 5)
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(2), LineOffset(4) });

    // Switch back to page 1 — its custom margins should still be intact.
    mock.writeToScreen("\033[1 P"); // PPA 1
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Page 2 margins should still be independently preserved.
    CHECK(page2.margin().vertical == Margin::Vertical { LineOffset(2), LineOffset(4) });
}

TEST_CASE("MultiPage.AltScreenMarginCopy", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on primary screen (page 1).
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Enter alternate screen (DECSET 1049) — alt screen should inherit primary margins.
    mock.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK(terminal.alternateScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Modify margins on the alternate screen.
    mock.writeToScreen("\033[1;5r"); // DECSTBM(1, 5) — reset to full screen
    CHECK(terminal.alternateScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });

    // Leave alternate screen (DECSET 1049 off) — primary margins should be unchanged.
    mock.writeToScreen("\033[?1049l");
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });
}

TEST_CASE("MultiPage.ResizeResetsAllPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 4 and set custom margins.
    mock.writeToScreen("\033[4 P");  // PPA 4
    mock.writeToScreen("\033[2;3r"); // DECSTBM(2, 3)
    CHECK(terminal.pageAt(PageIndex(3)).margin().vertical
          == Margin::Vertical { LineOffset(1), LineOffset(2) });

    // Resize terminal — all margins should reset to defaults.
    terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });

    // Page 1 margins should be reset.
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(5) });
    CHECK(terminal.primaryScreen().margin().horizontal
          == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Page 4 margins should also be reset.
    CHECK(terminal.pageAt(PageIndex(3)).margin().vertical
          == Margin::Vertical { LineOffset(0), LineOffset(5) });
}

TEST_CASE("MultiPage.HardResetResetsAllPageMargins", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Set custom margins on page 1.
    mock.writeToScreen("\033[2;4r"); // DECSTBM(2, 4)
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(1), LineOffset(3) });

    // Switch to page 3 and set custom margins.
    mock.writeToScreen("\033[3 P");  // PPA 3
    mock.writeToScreen("\033[3;5r"); // DECSTBM(3, 5)
    CHECK(terminal.pageAt(PageIndex(2)).margin().vertical
          == Margin::Vertical { LineOffset(2), LineOffset(4) });

    // Hard reset.
    mock.writeToScreen("\033c"); // RIS

    // Page 1 margins should be reset to defaults.
    CHECK(terminal.primaryScreen().margin().vertical == Margin::Vertical { LineOffset(0), LineOffset(4) });
    CHECK(terminal.primaryScreen().margin().horizontal
          == Margin::Horizontal { ColumnOffset(0), ColumnOffset(9) });

    // Page 3 margins should also be reset.
    CHECK(terminal.pageAt(PageIndex(2)).margin().vertical
          == Margin::Vertical { LineOffset(0), LineOffset(4) });
}

TEST_CASE("VT525 keyboard/national modes are settable toggles", "[screen]")
{
    // These VT525 DEC private modes are not yet acted on, but Contour remembers and reports their
    // state: SM/RM toggle the bit and DECRQM answers Set (1) / Reset (2), rather than pretending the
    // mode is unknown (0). RightToLeftMode (DECRLM, 34) and HebrewEncodingMode (DECHEM, 36) are the
    // entry points for real bidirectional/Hebrew support. Mirrors
    // esctest's doModifiableDecTest for these modes.
    auto togglesCleanly = [](unsigned decNumber) {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        // Reset -> DECRQM reports 2.
        mock.writeToScreen(std::format("\033[?{}l\033[?{}$p", decNumber, decNumber));
        CHECK(e(mock.terminal.peekInput()) == e(std::format("\033[?{};2$y", decNumber)));
        mock.discardPendingReplies();
        // Set -> DECRQM reports 1.
        mock.writeToScreen(std::format("\033[?{}h\033[?{}$p", decNumber, decNumber));
        CHECK(e(mock.terminal.peekInput()) == e(std::format("\033[?{};1$y", decNumber)));
    };

    SECTION("DECRLM (34, right-to-left)")
    {
        togglesCleanly(34);
    }
    SECTION("DECHEM (36, Hebrew encoding)")
    {
        togglesCleanly(36);
    }
    SECTION("DECVCCM (61, vertical cursor coupling)")
    {
        togglesCleanly(61);
    }
    SECTION("DECAAM (100, auto answerback)")
    {
        togglesCleanly(100);
    }
    SECTION("DECOSCNM (106, overscan)")
    {
        togglesCleanly(106);
    }
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
