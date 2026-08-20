// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <crispy/Escape.hpp>
#include <crispy/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

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

    // PPA: CSI 5 SP P — move to page 5, preserve cursor position.
    mock.writeToScreen("\033[5 P");                    // PPA to page 5
    CHECK(terminal.cursorPageIndex() == PageIndex(4)); // 0-based index
    // Cursor position should be preserved (still at the same logical position).
    // Note: cursor is on a NEW page, position within that page starts at home (0,0)
    // unless explicitly preserved by the terminal. Actually, PPA preserves position.
    // The cursor position is local to the Screen, so it was at home (0,0) on the new page.
    // Wait - setPage with moveCursorHome=false means we DON'T move cursor home,
    // but the cursor state is local to each Screen. When switching pages, the cursor
    // position that matters is the one stored in the _new_ screen's cursor state.

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
