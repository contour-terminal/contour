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

TEST_CASE("DECDMAC: define macro with VT sequences", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 1 with SGR bold + text "Bold"
    mock.writeToScreen("\033P1;0;0!z\033[1mBold\033\\");
    mock.terminal.flushInput();
    // Invoke macro 1
    mock.writeToScreen("\033[1*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 4) == "Bold");
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

TEST_CASE("DECUDK: soft reset clears UDKs", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033P0;1|17/41\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.udkString(17).has_value());
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
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
    CHECK(mock.terminal.locatorState().enabled);
    CHECK_FALSE(mock.terminal.locatorState().oneShot);
}

TEST_CASE("DECELR: disable locator reporting", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    mock.writeToScreen("\033[0'z"); // disable
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().enabled);
}

TEST_CASE("DECELR: one-shot mode (Ps=2)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[2;2'z"); // one-shot, character cells
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    CHECK(mock.terminal.locatorState().oneShot);
}

TEST_CASE("DECELR: pixel coordinates (Pu=1)", "[screen]")
{
    using LocatorCoordUnit = Terminal::LocatorCoordUnit;
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;1'z"); // enable, pixel coords
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().coordUnit == LocatorCoordUnit::DevicePixels);
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

TEST_CASE("DECELR: soft reset disables locator", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1;2'z"); // enable
    mock.terminal.flushInput();
    CHECK(mock.terminal.locatorState().enabled);
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK_FALSE(mock.terminal.locatorState().enabled);
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
