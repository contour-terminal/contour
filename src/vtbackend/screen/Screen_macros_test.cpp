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

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

TEST_CASE("DECSCA: enable and disable character protection", "[screen]")
{
    // Verifies that DECSCA Ps=1 enables CharacterProtected on subsequent characters,
    // and DECSCA Ps=0/2 disables it.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();

    // A unprotected, BC protected, D unprotected (Ps=0), EF unprotected (Ps=2)
    mock.writeToScreen(std::format("A{0}BC{1}D{2}EF", "\033[1\"q", "\033[0\"q", "\033[2\"q"));

    REQUIRE("ABCDEF" == screen.grid().lineText(LineOffset(0)));

    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected));       // B
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected));       // C
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // D
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::CharacterProtected)); // E
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::CharacterProtected)); // F
}

TEST_CASE("DECSCA: default parameter disables protection", "[screen]")
{
    // DECSCA with no parameter (default) should disable protection.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // AB protected, then DECSCA with default (0) disables protection, CD unprotected
    mock.writeToScreen(std::format("{0}AB{1}CD", "\033[1\"q", "\033[\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected));       // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected));       // B
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected)); // C
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // D
}

TEST_CASE("DECSCA: protection is independent of SGR rendition", "[screen]")
{
    // DECSCA protection attribute is independent of SGR visual attributes.
    // Setting SGR bold or other attributes should not affect the protection state.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // Enable protection, then set bold, then write characters — protection should persist.
    mock.writeToScreen(std::format("{0}\033[1mAB{1}CD", "\033[1\"q", "\033[0\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // A
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected)); // B
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Bold));               // A bold
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Bold));               // B bold
}

TEST_CASE("DECSCA: save and restore cursor preserves protection state", "[screen]")
{
    // DECSC/DECRC should save and restore the CharacterProtected attribute.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();

    // Enable protection, save cursor, disable protection, write AB (unprotected),
    // restore cursor (re-enables protection), write CD (protected, overwrites AB).
    mock.writeToScreen(std::format("{0}\0337{1}AB\0338CD", "\033[1\"q", "\033[0\"q"));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CharacterProtected)); // C
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CharacterProtected)); // D
    CHECK_FALSE(
        screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CharacterProtected)); // empty
    CHECK_FALSE(
        screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CharacterProtected)); // empty
}

TEST_CASE("SPA/EPA: ED respects ISO protection", "[screen]")
{
    // Mirrors esctest ED_respectsISOProtection: a cell written between SPA (ESC V) and EPA (ESC W)
    // survives a *regular* ED, while the unprotected cells around it are erased.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W"); // a, b, SPA, protected c, EPA
    REQUIRE(e(mainPageText(screen)) == "abc\\n");

    mock.writeToScreen("\033[H\033[J"); // CUP home, then ED to end of screen
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: EL respects ISO protection", "[screen]")
{
    // Mirrors esctest EL_respectsISOProtection: EL 2 (erase whole line) spares the protected cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[H\033[2K"); // CUP home, EL 2 (entire line)
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: ECH respects ISO protection", "[screen]")
{
    // Mirrors esctest ECH_respectsISOProtection: ECH 3 erases three cells but spares the protected one.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[H\033[3X"); // CUP home, ECH 3
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: 8-bit C1 forms behave like ESC V / ESC W", "[screen]")
{
    // The parser folds a lone C1 byte onto ESC + (byte - 0x40): 0x96 -> SPA, 0x97 -> EPA. So the
    // 8-bit forms must guard cells identically to the 7-bit ESC V / ESC W (esctest S8C1T_SPA_EPA).
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\x96"
                       "c\x97"); // a, b, SPA(0x96), protected c, EPA(0x97)
    mock.writeToScreen("\033[H\033[J");
    REQUIRE(e(mainPageText(screen)) == "  c\\n");
}

TEST_CASE("SPA/EPA: 8-bit C1 protection survives inside a coalesced text run", "[screen]")
{
    // Regression for the real-PTY case: the bytes arrive in one buffer, so the 8-bit SPA (0x96) sits
    // mid-run followed by a long text tail -- the condition under which the bulk text scanner would
    // swallow the C1 as U+FFFD instead of leaving it for the state machine to fold. The guarded cell
    // must still survive a later erase. (esctest S8C1T_SPA_EPA is the end-to-end counterpart.)
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // "ab", SPA, protected "c", EPA, then a long ASCII tail -- all one write, i.e. one parser buffer.
    mock.writeToScreen("ab\x96"
                       "c\x97"
                       "defghijklmnop");
    mock.writeToScreen("\033[H\033[K"); // CUP home, EL to end of line
    REQUIRE(e(mainPageText(screen)).substr(0, 3) == "  c");
}

TEST_CASE("DECSCA: regular ED does not respect DEC protection", "[screen]")
{
    // Mirrors esctest ED_doesNotRespectDECProtection: DECSCA protection is honoured only by the
    // *selective* erases, so a regular ED erases a DECSCA-protected cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033[1\"qc\033[0\"q");  // a, b, DECSCA(1), protected c, DECSCA(0)
    mock.writeToScreen("\033[H\033[J");           // CUP home, ED to end
    REQUIRE(e(mainPageText(screen)) == "   \\n"); // c erased too
}

TEST_CASE("SPA/EPA: soft reset clears ISO protection mode", "[screen]")
{
    // xterm's ReallyReset zeroes protected_mode unconditionally, so a DECSTR must return the screen
    // to the unprotected model: a subsequent regular ED then erases even a previously guarded cell.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(3) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("ab\033Vc\033W");
    mock.writeToScreen("\033[!p");                // DECSTR (soft reset)
    mock.writeToScreen("\033[H\033[J");           // CUP home, ED to end
    REQUIRE(e(mainPageText(screen)) == "   \\n"); // guarded c is now erasable
}

TEST_CASE("SPA/EPA: selective erases do NOT respect ISO protection", "[screen]")
{
    // The inverse pairing: DECSED/DECSEL/DECSERA spare DEC (DECSCA) protection only. An ISO-guarded
    // cell (SPA/EPA) is erased by them -- mirrors esctest DECSED/DECSEL/DECSERA_doesNotRespectISOProtect.
    SECTION("DECSED erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W"); // a, SPA, ISO-guarded b, EPA
        mock.writeToScreen("\033[?2J");     // DECSED 2 (selective erase display)
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
    SECTION("DECSEL erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W");
        mock.writeToScreen("\033[?2K"); // DECSEL 2 (selective erase line)
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
    SECTION("DECSERA erases an ISO-guarded cell")
    {
        auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("a\033Vb\033W");
        mock.writeToScreen("\033[1;1;1;2${"); // DECSERA over the row
        REQUIRE(e(mainPageText(screen)) == "  \\n");
    }
}

TEST_CASE("DECSCA: selective erase still respects DEC protection after the ISO split", "[screen]")
{
    // Regression guard for the two-flag split: DECSED must keep sparing DECSCA-protected cells.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(2) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("a\033[1\"qb\033[0\"q"); // a, DECSCA(1), DEC-protected b, DECSCA(0)
    mock.writeToScreen("\033[?2J");             // DECSED 2 spares the DEC-protected b
    REQUIRE(e(mainPageText(screen)) == " b\\n");
}

TEST_CASE("REP.basic_ascii", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Send "|" followed by CSI 9 b (repeat '|' 9 more times).
    // This mimics what ncurses sends via the rep terminfo capability.
    mock.writeToScreen("|\033[9b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "||||||||||          ");
}

TEST_CASE("REP.omitted_parameter_repeats_once", "[screen]")
{
    // REP's parameter defaults to 1, so `CSI b` on its own is legal. It was declared as requiring at
    // least one parameter, though, so a bare `CSI b` matched no function at all and was silently
    // dropped. vttest sends it.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("X\033[b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "XX      ");
}

TEST_CASE("REP.explicit_zero_repeats_once", "[screen]")
{
    // REP's parameter is a one-based count, so an explicit zero means the same as an omitted one --
    // xterm folds both with one_if_default(). Taken literally it repeated nothing and swallowed the
    // character, which is what paramOr() did here while every sibling sequence had moved on to
    // paramPositiveOr().
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(8) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("X\033[0b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "XX      ");
}

TEST_CASE("REP.after_bulk_text", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    // Send a longer ASCII string to exercise the bulk text path,
    // then immediately follow with REP.
    mock.writeToScreen("Hello|\033[3b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "Hello||||           ");
}

TEST_CASE("REP.wraps_at_left_right_margin", "[screen]")
{
    // REP repeats through the normal text path, so past the right margin it autowraps to the left
    // margin of the next line rather than stopping. Mirrors esctest test_REP_RespectsLeftRightMargins.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
    mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4
    mock.writeToScreen("\033[1;2H"); // CUP row 1, col 2 (the left margin)
    mock.writeToScreen("a");         // 'a' at the left margin
    mock.writeToScreen("\033[3b");   // REP 3
    // Two more fill the band on row 1; the third wraps to the left margin of row 2.
    CHECK(" aaa \n a   \n" == screen.renderMainPageText());
}

TEST_CASE("REP.scrolls_at_bottom_margin", "[screen]")
{
    // At the bottom margin REP's autowrap scrolls the region, exactly as ordinary text would.
    // Mirrors esctest test_REP_RespectsTopBottomMargins.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(6) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("\033[2;4r"); // DECSTBM 2;4 -> rows 2..4 (offsets 1..3)
    mock.writeToScreen("\033[4;4H"); // CUP row 4 (bottom margin), col 4
    mock.writeToScreen("a");
    mock.writeToScreen("\033[3b"); // REP 3: fills the row's tail, then wraps + scrolls the region up
    CHECK("      \n      \n   aaa\na     \n      \n" == screen.renderMainPageText());
}

TEST_CASE("REP.no_preceding_char", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    // CSI 5 b with no preceding graphic character does nothing.
    mock.writeToScreen("\033[5b");

    CHECK(screen.grid().lineText(LineOffset(0)) == "          ");
}

TEST_CASE("DECDMAC: define and invoke simple text macro", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 0 with plain text "Hello"
    mock.writeToScreen("\033P0;0;0!zHello\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 5) == "Hello");
}

TEST_CASE("DECINVM: a macro invoked through Terminal::writeToScreen runs within that call", "[screen]")
{
    // Terminal::writeToScreen() bypasses the PTY: the macro body must be on screen as soon as the write
    // returns.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.terminal.writeToScreen("\033P0;0;0!zHello\033\\"); // DECDMAC 0 = "Hello"
    mock.terminal.writeToScreen("\033[0*z");                // DECINVM 0
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 5) == "Hello");
}

TEST_CASE("DECINVM: a macro runs where it is invoked", "[screen]")
{
    // DEC STD 070: "The terminal substitutes the DECINVM sequence with the contents of the macro definition,
    // and executes the substituted text." What follows the invocation comes after the body, not before it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.terminal.writeToScreen(macroDefinition(1, "B"));

    SECTION("through the pty")
    {
        mock.writeToScreen("A\033[1*zC");
    }
    SECTION("through writeToScreen")
    {
        mock.terminal.writeToScreen("A\033[1*zC");
    }

    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 3) == "ABC");
}

TEST_CASE("DECINVM: stepped through in trace mode, a macro runs with its invocation", "[screen]")
{
    // The trace modes queue what is parsed and execute it as it is stepped through, after the parse has moved
    // on: the body is parsed then, and executed along with the DECINVM rather than queued behind the rest.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const& grid = mock.terminal.primaryScreen().grid();
    mock.terminal.writeToScreen(macroDefinition(1, "B"));

    mock.terminal.setExecutionMode(ExecutionMode::SingleStep);
    mock.terminal.writeToScreen("A\033[1*zC");
    REQUIRE(grid.lineText(LineOffset(0)).substr(0, 3) == "   ");

    mock.terminal.setExecutionMode(ExecutionMode::Normal);
    mock.terminal.processInputOnce(); // executes what was queued

    CHECK(grid.lineText(LineOffset(0)).substr(0, 4) == "ABC ");
}

TEST_CASE("DECINVM: single-stepped, a macro's body runs with the step that invokes it", "[screen]")
{
    // A step executes one queued event, with the terminal already back in the waiting mode, where the
    // parser's events are queued. The body of the DECINVM a step executes is parsed then, and runs then too:
    // queued, it would wait behind the events the DECINVM came before.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const& grid = mock.terminal.primaryScreen().grid();
    mock.terminal.writeToScreen(macroDefinition(1, "B"));

    mock.terminal.setExecutionMode(ExecutionMode::SingleStep);
    mock.terminal.writeToScreen("A\033[1*zC");
    REQUIRE(mock.terminal.traceHandler().pendingSequences().size() == 3);

    for ([[maybe_unused]] auto const step: std::views::iota(0, 2)) // the "A", then the DECINVM
    {
        mock.terminal.setExecutionMode(ExecutionMode::SingleStep);
        mock.terminal.processInputOnce();
    }

    CHECK(grid.lineText(LineOffset(0)).substr(0, 3) == "AB ");
    CHECK(mock.terminal.traceHandler().pendingSequences().size() == 1); // the "C"
}

TEST_CASE("DECINVM: a control string in a macro body is collected and answered", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[5;6r");                          // DECSTBM 5;6
    mock.writeToScreen(macroDefinition(0, "\033P$qr\033\\")); // DECRQSS "r"
    mock.discardPendingReplies();

    mock.writeToScreen("\033[0*z");

    CHECK(e(mock.terminal.peekInput()) == e("\033P1$r5;6r\033\\"));
}

// The three below hold a macro body to DEC STD 070's "The terminal substitutes the DECINVM sequence with the
// contents of the macro definition": what the parser reading the stream makes of the body, and of what
// follows it, is what it would make of the same bytes had the host sent them there.

TEST_CASE("DECINVM: a switch to VT52 in a macro body governs the rest of it and what follows it", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // DECANM reset enters VT52, whose ESC Y <row> <column> addresses the cursor: row 2, column 3 here.
    mock.writeToScreen(macroDefinition(0, "\033[?2l\033Y\041\042B"));

    mock.writeToScreen("\033[0*z\033CA"); // VT52's ESC C moves the cursor right; ANSI has no ESC C

    CHECK(mock.terminal.isVT52Mode());
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 6) == "      ");
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(1)).substr(0, 6) == "  B A ");
}

TEST_CASE("DECINVM: REP repeats the last character a macro body printed, in it and after it", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    SECTION("in the body")
    {
        mock.writeToScreen(macroDefinition(0, "y\033[2b"));
        mock.writeToScreen("x\033[0*z");
    }
    SECTION("after the body")
    {
        mock.writeToScreen(macroDefinition(0, "y"));
        mock.writeToScreen("x\033[0*z\033[2b");
    }

    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 5) == "xyyy ");
}

TEST_CASE("DECINVM: a sequence a macro body leaves unfinished is finished by the input after it", "[screen]")
{
    // Windows Terminal's InvokeMacro hands the body to the state machine reading the stream, which goes on
    // from whatever state the body leaves it in.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    SECTION("a control sequence")
    {
        mock.writeToScreen(macroDefinition(0, "\033[1")); // SGR 1, short of its final byte

        mock.writeToScreen("\033[0*zmX");

        auto const& screen = mock.terminal.primaryScreen();
        CHECK(screen.grid().lineText(LineOffset(0)).substr(0, 2) == "X ");
        CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Bold));
    }
    SECTION("a control string")
    {
        mock.writeToScreen("\033[5;6r");                   // DECSTBM 5;6
        mock.writeToScreen(macroDefinition(0, "\033P$q")); // DECRQSS, short of its request and ST
        mock.discardPendingReplies();

        mock.writeToScreen("\033[0*zr\033\\");

        CHECK(e(mock.terminal.peekInput()) == e("\033P1$r5;6r\033\\"));
    }
}

TEST_CASE("DECINVM: a hard reset in a macro body ends every body running", "[screen]")
{
    // RIS clears the macro definitions, and with them the bodies running: Windows Terminal's
    // ClearMacrosIfInUse replaces them "with NUL characters to prevent any further output". What the host
    // sends after the invocation still prints.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    SECTION("in the body invoked")
    {
        mock.writeToScreen(macroDefinition(0, "A\033cB"));
    }
    SECTION("in a body it invokes")
    {
        mock.writeToScreen(macroDefinition(1, "A\033cB"));
        mock.writeToScreen(macroDefinition(0, "\033[1*zC"));
    }

    mock.writeToScreen("\033[0*zD");

    CHECK_FALSE(mock.terminal.macroBody(0).has_value());
    CHECK_FALSE(mock.terminal.macroBody(1).has_value());
    // The reset erased the "A"; neither "B" nor "C" comes after it.
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "D ");
}

TEST_CASE("DECDMAC: a macro body defines no macro", "[screen]")
{
    // DEC STD 070: "Macro definitions cannot be nested." A DECDMAC in a running body is ignored whole -- it
    // deletes nothing either -- and the body runs on past it.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen(macroDefinition(1, "Q"));
    auto body = std::string {};

    SECTION("of another id")
    {
        body = std::format("A{}B", macroDefinition(2, "Z"));
    }
    SECTION("of its own id")
    {
        body = std::format("A{}B", macroDefinition(0, "Z"));
    }
    SECTION("deleting every macro first (Pdt=1)")
    {
        body = "A\033P2;1;0!zZ\033\\B";
    }
    mock.writeToScreen(macroDefinition(0, body));

    mock.writeToScreen("\033[0*z");

    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 3) == "AB ");
    CHECK(mock.terminal.macroBody(0) == std::optional { std::string_view { body } });
    CHECK(mock.terminal.macroBody(1) == std::optional { std::string_view { "Q" } });
    CHECK_FALSE(mock.terminal.macroBody(2).has_value());
}

TEST_CASE("DECDMAC: a soft reset keeps the macros, and a hard reset clears them", "[screen]")
{
    // DEC STD 070: "Soft Terminal Reset (DECSTR) has no effect on stored macro definitions", and "All macro
    // definitions are cleared on receipt of a Reset to Initial State (RIS) or Secure Reset (DECSR)."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen(macroDefinition(0, "kept"));

    mock.writeToScreen("\033[!p"); // DECSTR
    CHECK(mock.terminal.macroBody(0) == std::optional { std::string_view { "kept" } });

    mock.writeToScreen("\033c"); // RIS
    CHECK_FALSE(mock.terminal.macroBody(0).has_value());
}

TEST_CASE("DECINVM: macros nest at most sixteen deep", "[screen]")
{
    // DEC STD 070: "Macros can invoke other macros with up to 16 levels of nesting, but cannot invoke
    // themselves recursively."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };

    // Macro i prints the i-th letter, then invokes macro i + 1: a chain of eighteen runs sixteen of them.
    for (auto const id: std::views::iota(0, 18))
        mock.writeToScreen(
            macroDefinition(id, std::format("{}\033[{}*z", static_cast<char>('a' + id), id + 1)));
    mock.writeToScreen("\033[0*z");

    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 17) == "abcdefghijklmnop ");
}

TEST_CASE("DECINVM: what one invocation runs is bounded in bytes, however the input is chunked", "[screen]")
{
    // Nesting alone does not bound the work: a body may invoke the next macro many times over. Windows
    // Terminal's MacroBuffer::InvokeMacro bounds the bytes of all the bodies one invocation from the stream
    // runs, its own included, afresh for each such invocation -- so where the reads happen to split the
    // stream makes no difference.
    auto const screenAfter = [](auto const& feed) {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
        mock.writeToScreen("\033P1;0;1!z79!4095;00;\033\\"); // macro 1: a "y", and NULs up to 4096 bytes
        auto fanOut = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, 400))
            fanOut += "\033[1*z";
        mock.writeToScreen(macroDefinition(0, fanOut)); // macro 0: invokes macro 1 400 times, in 2000 bytes
        REQUIRE(mock.terminal.macroBody(1).value_or(std::string_view {}).size() == 4096);
        REQUIRE(mock.terminal.macroBody(0).value_or(std::string_view {}).size() == 2000);

        feed(mock.terminal, std::string_view { "\033[0*z\r\n\033[0*z" });
        return mock.terminal.primaryScreen().renderMainPageText();
    };

    auto const inOnePiece =
        screenAfter([](Terminal& terminal, std::string_view stream) { terminal.writeToScreen(stream); });
    auto const byteByByte = screenAfter([](Terminal& terminal, std::string_view stream) {
        for (auto const& ch: stream)
            terminal.writeToScreen(std::string_view { &ch, 1 });
    });

    CHECK(inOnePiece == byteByByte);

    // Each invocation of macro 0 runs its own 2000 bytes, then as many 4096-byte runs of macro 1 as stay
    // below the bound -- Windows Terminal admits a body while it is shorter than what is left.
    auto const runsOfMacro1 =
        static_cast<std::ptrdiff_t>((Terminal::MaxMacroBytesPerInvocation - 2000 - 1) / 4096);
    CHECK(std::ranges::count(inOnePiece, 'y') == 2 * runsOfMacro1);
}

TEST_CASE("DECDMAC: the macros share six kilobytes, and one that does not fit is ignored", "[screen]")
{
    // DEC STD 070: "6k bytes of RAM is available for storage of macro definitions", and "Any macro
    // definitions which cannot fit in the remaining macro storage space are ignored."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const define = [&](int id, size_t bytes, int deleteControl = 0) {
        mock.writeToScreen(std::format("\033P{};{};0!z{}\033\\", id, deleteControl, std::string(bytes, 'x')));
    };
    auto const bodyBytes = [&](int id) {
        return mock.terminal.macroBody(id).value_or(std::string_view {}).size();
    };

    define(0, 6000);
    REQUIRE(bodyBytes(0) == 6000);

    SECTION("a definition past the room left is ignored whole, never cut")
    {
        define(1, 200);
        CHECK_FALSE(mock.terminal.macroBody(1).has_value());
        define(1, 144); // exactly what is left
        CHECK(bodyBytes(1) == 144);
    }
    SECTION("a redefinition counts on the room its old body leaves")
    {
        define(0, 6100);
        CHECK(bodyBytes(0) == 6100);
    }
    SECTION("deleting every macro first makes the room for the one defined")
    {
        define(1, 6100, 1);
        CHECK_FALSE(mock.terminal.macroBody(0).has_value());
        CHECK(bodyBytes(1) == 6100);
    }
    SECTION("larger than all the room there is")
    {
        define(1, 6145, 1);
        CHECK_FALSE(mock.terminal.macroBody(1).has_value());
    }
}

TEST_CASE("DECDMAC: a body far larger than the macro space is consumed and ignored", "[screen]")
{
    // The VT520 manual: "The terminal ignores any macro definitions that cannot fit into this space." What
    // follows the definition is parsed as ever.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen(macroDefinition(0, "kept"));
    auto const spaceBefore = mock.terminal.macroSpaceAvailable();

    SECTION("as text")
    {
        mock.writeToScreen(std::format("\033P1;0;0!z{}\033\\", std::string(size_t { 1024 } * 1024, 'x')));
    }
    SECTION("hex-encoded")
    {
        mock.writeToScreen(std::format("\033P1;0;1!z{}\033\\", std::string(size_t { 2048 } * 1024, '7')));
    }
    mock.writeToScreen("OK");

    CHECK_FALSE(mock.terminal.macroBody(1).has_value());
    CHECK(mock.terminal.macroBody(0) == std::optional { std::string_view { "kept" } });
    CHECK(mock.terminal.macroSpaceAvailable() == spaceBefore);
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 3) == "OK ");
}

TEST_CASE("DECDMAC: a parameter out of range makes the control do nothing", "[screen]")
{
    // DEC STD 070: "Pid must be a number between 0 and 63; Pdt 0 or 1; and Pen 0 or 1; otherwise the control
    // will be ignored." All of it: what Pdt would delete stays.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen(macroDefinition(0, "kept"));
    auto const spaceBefore = mock.terminal.macroSpaceAvailable();

    SECTION("Pid past 63, deleting every macro first")
    {
        mock.writeToScreen("\033P64;1;0!zX\033\\");
    }
    SECTION("Pdt neither 0 nor 1")
    {
        mock.writeToScreen("\033P1;2;0!zX\033\\");
    }
    SECTION("Pen neither 0 nor 1")
    {
        mock.writeToScreen("\033P1;0;2!zX\033\\");
    }

    CHECK(mock.terminal.macroBody(0) == std::optional { std::string_view { "kept" } });
    CHECK_FALSE(mock.terminal.macroBody(1).has_value());
    CHECK(mock.terminal.macroSpaceAvailable() == spaceBefore);
}

TEST_CASE("DECDMAC: control characters in a body are not part of the macro", "[screen]")
{
    // DEC STD 070: "characters from 0/8 through 0/13 can be included to format the device control string,
    // but are not part of the macro definition." Windows Terminal ignores every C0 control there.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    SECTION("as text")
    {
        mock.writeToScreen("\033P1;0;0!zH\r\ni\033\\");
    }
    SECTION("hex-encoded, between pairs")
    {
        mock.writeToScreen("\033P1;0;1!z48\r\n69\033\\");
    }
    SECTION("hex-encoded, inside a pair")
    {
        mock.writeToScreen("\033P1;0;1!z4\r\n869\033\\");
    }

    CHECK(mock.terminal.macroBody(1) == std::optional { std::string_view { "Hi" } });
}

TEST_CASE("DECDMAC: a hex body holding what is not hex is ignored whole", "[screen]")
{
    // DEC STD 070: "If a non-hex pair is encountered that is not a valid repeat sequence, the entire macro
    // definition is ignored."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    SECTION("as the first digit of a pair")
    {
        mock.writeToScreen("\033P1;0;1!z48G9\033\\");
    }
    SECTION("as the second digit of a pair")
    {
        mock.writeToScreen("\033P1;0;1!z484G\033\\");
    }

    CHECK_FALSE(mock.terminal.macroBody(1).has_value());
}

TEST_CASE("DECDMAC: a hex body repeats what a repeat sequence encloses", "[screen]")
{
    // DEC STD 070's repeat introducer, "! Pn ; D...D ;": "If Pn is not specified, the sequence is inserted
    // into the definition once", and "If a string terminator is encountered before the final semicolon, the
    // repeat string is terminated normally."
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const define = [&](std::string_view hexBody) {
        mock.writeToScreen(std::format("\033P1;0;1!z{}\033\\", hexBody));
        return mock.terminal.macroBody(1);
    };
    using Body = std::optional<std::string_view>;

    CHECK(define("41!;42;43") == Body { "ABC" });
    CHECK(define("41!3;4243;44") == Body { "ABCBCBCD" });
    CHECK(define("41!2;42") == Body { "ABB" });

    // The repeated bytes take macro space like any other: past it, the whole definition is ignored.
    CHECK(define(std::format("!{};41;", Terminal::MacroSpaceBytes + 1)) == std::nullopt);
    CHECK(mock.terminal.macroSpaceAvailable() == Terminal::MacroSpaceBytes);
    CHECK(define(std::format("!{};41;", Terminal::MacroSpaceBytes)).value_or(std::string_view {}).size()
          == Terminal::MacroSpaceBytes);
}

TEST_CASE("DECMSR reports the room left for macros, in units of sixteen bytes", "[screen]")
{
    // DECMSR's parameter is "the number of bytes available for macro definitions divided by 16 (rounded
    // down)" (DEC STD 070), asked for with DSR ?62.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const report = [&]() {
        mock.discardPendingReplies();
        mock.writeToScreen("\033[?62n");
        return std::string { mock.terminal.peekInput() };
    };

    CHECK(e(report()) == e("\033[384*{")); // 6144 / 16
    mock.writeToScreen(macroDefinition(0, std::string(100, 'x')));
    CHECK(e(report()) == e("\033[377*{"));    // 6044 / 16, rounded down
    mock.writeToScreen("\033P0;1;0!z\033\\"); // delete all
    CHECK(e(report()) == e("\033[384*{"));
}

TEST_CASE("DECCKSR reports the checksum of the macro definitions", "[screen]")
{
    // DSR ?63 asks for "a checksum of the current text macro definitions" (DEC STD 070), in four hex digits.
    // The sum is Windows Terminal's MacroBuffer::CalculateChecksum, meant to match the VT420's: every byte
    // of every definition, negated and added up in 16 bits.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const report = [&]() {
        mock.discardPendingReplies();
        mock.writeToScreen("\033[?63;7n");
        return std::string { mock.terminal.peekInput() };
    };

    CHECK(e(report()) == e("\033P7!~0000\033\\"));
    mock.writeToScreen(macroDefinition(0, "AB"));
    CHECK(e(report()) == e("\033P7!~FF7D\033\\")); // -(0x41 + 0x42)
    mock.writeToScreen(macroDefinition(5, "C"));
    CHECK(e(report()) == e("\033P7!~FF3A\033\\")); // -(0x41 + 0x42 + 0x43)
    mock.writeToScreen(macroDefinition(9, "\xFF"));
    CHECK(e(report()) == e("\033P7!~FE3B\033\\")); // a byte counts as unsigned: -(0xC6 + 0xFF)
}

TEST_CASE("DECMSR and DECCKSR answer from VT420 on", "[screen]")
{
    // DEC STD 070 makes both reports Level 4, as the macros they report on are; xterm, too, leaves them
    // unanswered at VT220.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const repliesAtLevel = [&](int level) {
        mock.writeToScreen(std::format("\033[{};1\"p", level)); // DECSCL
        mock.discardPendingReplies();
        mock.writeToScreen("\033[?62n\033[?63;5n");
        return std::string { mock.terminal.peekInput() };
    };

    CHECK(e(repliesAtLevel(64)) == e("\033[384*{\033P5!~0000\033\\")); // VT420
    CHECK(e(repliesAtLevel(62)).empty());                              // VT220
}

TEST_CASE("DECDMAC: define macro with VT sequences", "[screen]")
{
    // Hex-encoded (Pen=1): in a plain body the ESC of the SGR would end the DCS, defining a macro of nothing
    // and printing "Bold" on the spot.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.writeToScreen(macroDefinition(1, "\033[1mBold"));
    REQUIRE(mock.terminal.macroBody(1) == std::optional { std::string_view { "\033[1mBold" } });
    CHECK(screen.grid().lineText(LineOffset(0)).starts_with("    ")); // defining shows nothing

    mock.writeToScreen("\033[1*z");
    mock.writeToScreen("!");

    CHECK(screen.grid().lineText(LineOffset(0)).substr(0, 5) == "Bold!");
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Bold));
    // DEC STD 070: "The current state of the machine is not saved or restored when macros are invoked."
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::Bold));
}

TEST_CASE("DECDMAC: hex-encoded macro (Pen=1)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 2 with hex encoding: "Hi" = 0x48 0x69
    mock.writeToScreen("\033P2;0;1!z4869\033\\");
    mock.terminal.flushInput();
    // Invoke macro 2
    mock.writeToScreen("\033[2*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "Hi");
}

TEST_CASE("DECDMAC: delete all macros (Pdt=1)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 0
    mock.writeToScreen("\033P0;0;0!zFirst\033\\");
    mock.terminal.flushInput();
    // Define macro 5 with delete-all (Pdt=1)
    mock.writeToScreen("\033P5;1;0!zSecond\033\\");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.macroBody(0).has_value());
    CHECK(mock.terminal.macroBody(5).has_value());
}

TEST_CASE("DECDMAC: overwrite existing macro", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 0 with "Old"
    mock.writeToScreen("\033P0;0;0!zOld\033\\");
    mock.terminal.flushInput();
    // Redefine macro 0 with "New"
    mock.writeToScreen("\033P0;0;0!zNew\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 3) == "New");
}

TEST_CASE("DECDMAC: max 64 macros", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macros 0-63
    for (auto i = 0; i < 64; ++i)
        mock.writeToScreen(std::format("\033P{};0;0!zM{}\033\\", i, i));
    mock.terminal.flushInput();
    CHECK(mock.terminal.macroBody(0).has_value());
    CHECK(mock.terminal.macroBody(63).has_value());
    // Macro 64 should be rejected (out of range)
    mock.writeToScreen("\033P64;0;0!zBad\033\\");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.macroBody(64).has_value());
}

TEST_CASE("DECDMAC: empty macro body", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 3 with empty body
    mock.writeToScreen("\033P3;0;0!z\033\\");
    mock.terminal.flushInput();
    // Empty body erases the macro
    CHECK_FALSE(mock.terminal.macroBody(3).has_value());
    // Invoke empty macro — should produce no output
    mock.writeToScreen("\033[3*z");
    mock.writeToScreen("X");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 1) == "X");
}

TEST_CASE("DECDMAC: ext 32 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(32)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 32 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(32)); // optional at level 2
}

TEST_CASE("DECUDK: program single key", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (key 17) with "Hello" (hex: 48656C6C6F)
    mock.writeToScreen("\033P0;1|17/48656C6C6F\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    CHECK(mock.terminal.udkString(17).value() == "Hello");
}

TEST_CASE("DECUDK: program multiple keys", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (17) with "A" (hex: 41) and F7 (18) with "B" (hex: 42)
    mock.writeToScreen("\033P0;1|17/41;18/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A");
    CHECK(mock.terminal.udkString(18).value() == "B");
}

TEST_CASE("DECUDK: clear all before loading (Pc=0)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First program F6
    mock.writeToScreen("\033P1;1|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    // Then program F7 with clear-all (Pc=0)
    mock.writeToScreen("\033P0;1|18/42\033\\");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.udkString(17).has_value()); // F6 should be cleared
    CHECK(mock.terminal.udkString(18).has_value());       // F7 should exist
}

TEST_CASE("DECUDK: keep existing (Pc=1)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First program F6
    mock.writeToScreen("\033P1;1|17/41\033\\");
    mock.terminal.flushInput();
    // Then program F7, keeping existing (Pc=1)
    mock.writeToScreen("\033P1;1|18/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value()); // F6 should still exist
    CHECK(mock.terminal.udkString(18).has_value()); // F7 should also exist
}

TEST_CASE("DECUDK: lock keys (Pl=0)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 with lock (Pl=0)
    mock.writeToScreen("\033P0;0|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A");
    // Attempt to reprogram F6 — should be rejected because keys are locked
    mock.writeToScreen("\033P0;0|17/42\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).value() == "A"); // Still "A", not "B"
}

TEST_CASE("DECUDK: hex decode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (17) with hex "1B5B31m" → ESC [ 1 m (SGR bold)
    mock.writeToScreen("\033P0;1|17/1B5B316D\033\\");
    mock.terminal.flushInput();
    auto const str = mock.terminal.udkString(17);
    REQUIRE(str.has_value());
    CHECK(str.value() == "\033[1m");
}

TEST_CASE("DECUDK: a soft reset keeps the UDKs, and a hard reset clears them", "[screen]")
{
    // DEC STD 070 lists the UDKs among what is "NOT affected by the soft reset"; RIS "Clears user-defined
    // keys" (the VT520 manual).
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033P0;1|17/41\033\\");
    REQUIRE(mock.terminal.udkString(17) == std::optional { std::string { "A" } });

    mock.writeToScreen("\033[!p"); // DECSTR
    CHECK(mock.terminal.udkString(17) == std::optional { std::string { "A" } });

    mock.writeToScreen("\033c"); // RIS
    CHECK_FALSE(mock.terminal.udkString(17).has_value());
}

TEST_CASE("DECUDK: ext 8 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(8)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 8 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(8)); // optional at level 2
}

TEST_CASE("DECUDK: udkStringForKey maps Key enum to UDK ID", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Program F6 (key 17) with "test"
    mock.writeToScreen("\033P0;1|17/74657374\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkStringForKey(Key::F6).has_value());
    CHECK(mock.terminal.udkStringForKey(Key::F6).value() == "test");
    CHECK_FALSE(mock.terminal.udkStringForKey(Key::F5).has_value()); // F5 is not programmable
}

TEST_CASE("DECELR: enable locator reporting", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Enable locator: CSI 1 ; 2 ' z  (Ps=1 enable, Pu=2 character cells)
    mock.writeToScreen("\033[1;2'z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Continuous);
}

TEST_CASE("DECELR: disable locator reporting", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Continuous);
    mock.writeToScreen("\033[0'z"); // disable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Off);
}

TEST_CASE("DECELR: one-shot mode (Ps=2)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[2;2'z"); // one-shot, character cells
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::OneShot);
}

TEST_CASE("DECELR: pixel coordinates (Pu=1)", "[screen]")
{
    using LocatorCoordUnit = Terminal::LocatorCoordUnit;
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;1'z"); // enable, pixel coords
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().coordUnit == LocatorCoordUnit::DevicePixels);
}

TEST_CASE("DECELR: an undefined parameter ignores the sequence", "[screen]")
{
    // DEC STD 070 ch. 13 DECELR: "Undefined values for Ps or Pu causes the entire sequence to be ignored."
    using LocatorCoordUnit = Terminal::LocatorCoordUnit;
    auto const sequence = GENERATE(std::string_view { "\033[5;1'z" },  // Ps undefined
                                   std::string_view { "\033[1;7'z" }); // Pu undefined
    INFO(e(sequence));
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[2;2'z"); // one-shot, character cells
    mock.writeToScreen(sequence);
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::OneShot);
    CHECK(mock.terminal.locatorState().coordUnit == LocatorCoordUnit::CharacterCells);
}

TEST_CASE("DECSLE: select locator events", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable locator
    mock.terminal.flushInput();
    // Disable button down, enable button up: CSI 2 ; 3 ' {
    mock.writeToScreen("\033[2;3'{");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().reportButtonDown);
    CHECK(mock.terminal.locatorState().reportButtonUp);
}

TEST_CASE("DECRQLP: request locator position", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Request locator position when not enabled — should report locator unavailable
    mock.resetReplyData();
    mock.writeToScreen("\033[0'|");
    mock.terminal.flushInput();
    auto const reply = mock.replyData();
    // Should contain DECLRP format: CSI 0 ; ... & w
    CHECK(reply.contains("&w"));
}

TEST_CASE("DECLRP: the page is the page on display", "[screen]")
{
    // DEC STD 070 ch. 13: "Pp is the page coordinate of the locator position". The locator points through the
    // window at the page shown in it -- the page on display, not the cursor's -- and the alternate screen is
    // page 1, as every page-number report names it.
    auto const [setup, page] = GENERATE(table<std::string_view, int>({
        { "", 1 },
        { "\033[3 P\033[?64l\033[1 P", 3 }, // page 3 on display, the cursor on page 1
        { "\033[3 P\033[?1049h", 1 },       // the alternate screen, entered from page 3
    }));
    INFO(e(setup));
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(setup);
    mock.writeToScreen("\033[1;2'z"); // DECELR: reports on, in character cells
    pointAt(mock, 2, 3);

    mock.resetReplyData();
    mock.writeToScreen("\033['|"); // DECRQLP
    CHECK(e(mock.replyData()) == e(std::format("\033[1;0;3;4;{}&w", page)));
}

TEST_CASE("DECLRP: each button reports its own events and its bit", "[screen]")
{
    // DEC STD 070 ch. 13: Pe is 2/3 for the left button going down/up, 4/5 for the middle one, 6/7 for the
    // right one; Pb names the buttons held down, right 1, middle 2, left 4. xterm's SendLocatorPosition sends
    // 2 + (button << 1) and 3 + (button << 1), and the mask after the event.
    auto const [button, down, up, bit] = GENERATE(table<MouseButton, int, int, int>({
        { MouseButton::Left, 2, 3, 4 },
        { MouseButton::Middle, 4, 5, 2 },
        { MouseButton::Right, 6, 7, 1 },
    }));
    CAPTURE(down);
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z\033[1;3'{"); // DECELR on; DECSLE: report presses and releases

    mock.resetReplyData();
    pressAt(mock, button, 2, 3);
    CHECK(e(mock.replyData()) == e(std::format("\033[{};{};3;4;1&w", down, bit)));
    mock.resetReplyData();
    release(mock, button);
    CHECK(e(mock.replyData()) == e(std::format("\033[{};0;3;4;1&w", up)));
}

TEST_CASE("DECLRP: the button mask names every button held down", "[screen]")
{
    // Pb is the state of all the buttons after the event, not the one that moved: DEC STD 070 ch. 13
    // ("indicating which, if any, of up to 4 buttons are down"), and xterm's "mask should be the state after
    // the button press/release". A request (Pe 1) names them too.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z\033[1;3'{");
    auto const reportOf = [&](auto const& event) {
        mock.resetReplyData();
        event();
        return e(mock.replyData());
    };
    CHECK(reportOf([&] { pressAt(mock, MouseButton::Left, 0, 0); }) == e("\033[2;4;1;1;1&w"));
    CHECK(reportOf([&] { pressAt(mock, MouseButton::Right, 0, 0); }) == e("\033[6;5;1;1;1&w"));
    CHECK(reportOf([&] { mock.writeToScreen("\033['|"); }) == e("\033[1;5;1;1;1&w"));
    CHECK(reportOf([&] { release(mock, MouseButton::Left); }) == e("\033[3;1;1;1;1&w"));
    CHECK(reportOf([&] { release(mock, MouseButton::Right); }) == e("\033[7;0;1;1;1&w"));
}

TEST_CASE("DECLRP: an unavailable locator sends no other parameter", "[screen]")
{
    // DEC STD 070 ch. 13: "If Pe is 0 or omitted, all other parameters shall not be sent because the locator
    // is unavailable". xterm answers CSI 0 & w both while the locator is off and while the pointer is outside
    // its window (GetLocatorPosition, SendLocatorPosition).
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    SECTION("the locator is off")
    {
        mock.resetReplyData();
        mock.writeToScreen("\033['|");
        CHECK(e(mock.replyData()) == e("\033[0&w"));
    }

    SECTION("the pointer is off the grid")
    {
        mock.writeToScreen("\033[1;2'z\033[1;3'{");
        pressAt(mock, MouseButton::Left, 2, 3);
        mock.terminal.sendMouseLeaveEvent();
        mock.resetReplyData();
        mock.writeToScreen("\033['|");
        CHECK(e(mock.replyData()) == e("\033[0&w"));
        mock.resetReplyData();
        release(mock, MouseButton::Left);
        CHECK(e(mock.replyData()) == e("\033[0&w"));
    }
}

TEST_CASE("DECLRP: one report in one-shot mode disables the locator", "[screen]")
{
    // DEC STD 070 ch. 13 DECLRP: "If locator reporting has been enabled to one shot mode (DECELR with Ps =
    // 2), a single locator report (DECLRP) shall reset locator reporting mode to disabled". xterm resets
    // after every report it sends, the answer to a request (GetLocatorPosition) as a button's
    // (SendLocatorPosition), and the locator-unavailable one too.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[2;2'z\033[1;3'{"); // DECELR: one shot, in cells; DECSLE: presses and releases
    pointAt(mock, 2, 3);

    SECTION("the answer to a request")
    {
        mock.resetReplyData();
        mock.writeToScreen("\033['|");
        CHECK(e(mock.replyData()) == e("\033[1;0;3;4;1&w"));
    }

    SECTION("the answer to a request, the pointer off the grid")
    {
        mock.terminal.sendMouseLeaveEvent();
        mock.resetReplyData();
        mock.writeToScreen("\033['|");
        CHECK(e(mock.replyData()) == e("\033[0&w"));
    }

    SECTION("a button's report")
    {
        mock.resetReplyData();
        pressAt(mock, MouseButton::Left, 2, 3);
        CHECK(e(mock.replyData()) == e("\033[2;4;3;4;1&w"));
    }

    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Off);
    mock.resetReplyData();
    mock.writeToScreen("\033['|");
    CHECK(e(mock.replyData()) == e("\033[0&w"));
}

TEST_CASE("DECLRP: the coordinates are in the unit DECELR selected", "[screen]")
{
    // DEC STD 070 ch. 13: "The parameter values Pr, Pc, and Pp, are expressed in the units selected by the
    // last received Enable Locator Reports (DECELR) command", Pu 1 being "Device physical pixels". xterm's
    // LocatorCoords reports y + 1 and x + 1 in its pixel mode, and a position outside its window as out of
    // range, which it answers with CSI 0 & w. Over the window's padding the cell is still the one at the
    // edge, but the pixel is off the grid. The page stays a page number. The grid is 20 x 10 cells of 10 x 20
    // pixels.
    auto const [decelr, line, column, x, y, report] =
        GENERATE(table<std::string_view, int, int, int, int, std::string_view>({
            { "\033[1;1'z", 2, 3, 35, 47, "\033[1;0;48;36;1&w" },      // device pixels
            { "\033[1;1'z", 9, 19, 199, 199, "\033[1;0;200;200;1&w" }, // the grid's last pixel
            { "\033[1;1'z", 2, 0, -3, 47, "\033[0&w" },                // over the left padding
            { "\033[1;1'z", 0, 3, 35, -1, "\033[0&w" },                // over the top padding
            { "\033[1;1'z", 2, 19, 200, 47, "\033[0&w" },              // past the right edge
            { "\033[1;1'z", 9, 3, 35, 200, "\033[0&w" },               // past the bottom edge
            { "\033[1;2'z", 2, 3, 35, 47, "\033[1;0;3;4;1&w" },        // character cells
            { "\033[1;2'z", 2, 0, -3, 47, "\033[1;0;3;1;1&w" }, // cells: the edge cell, whatever the pixel
            { "\033[1'z", 2, 3, 35, 47, "\033[1;0;3;4;1&w" },   // character cells, the default
        }));
    INFO(std::format("{} at pixel ({}, {})", e(decelr), x, y));
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    mock.writeToScreen(decelr);
    pointAt(
        mock, line, column, PixelCoordinate { .x = PixelCoordinate::X { x }, .y = PixelCoordinate::Y { y } });

    mock.resetReplyData();
    mock.writeToScreen("\033['|");
    CHECK(e(mock.replyData()) == e(report));
}

TEST_CASE("DECELR: soft reset disables locator", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Continuous);
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().reporting == Terminal::LocatorReporting::Off);
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
