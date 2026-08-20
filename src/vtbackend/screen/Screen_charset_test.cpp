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

TEST_CASE("VT52: enter, cursor movement, and leave", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[?2l"); // DECANM reset: enter VT52
    REQUIRE(mock.terminal.isVT52Mode());

    // ESC Y row col -- direct cursor address; each coordinate byte is value + 0x20.
    mock.writeToScreen("\033Y\x23\x25"); // row 0x23-0x20=3, col 0x25-0x20=5
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(3), ColumnOffset(5) });

    mock.writeToScreen("\033H"); // ESC H -- home (must be cursor-home in VT52, not HTS)
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(0), ColumnOffset(0) });

    mock.writeToScreen("\033B\033B\033C"); // down, down, right
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(1) });

    mock.writeToScreen("\033A\033D"); // up, left (ESC D is cursor-left in VT52, not IND)
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) });

    mock.writeToScreen("\033<"); // ESC < -- leave VT52
    REQUIRE_FALSE(mock.terminal.isVT52Mode());
    REQUIRE(mock.terminal.operatingLevel() == VTType::VT100); // VT52 exit enters ANSI at VT100

    // Back in ANSI mode, CSI cursor movement works again.
    mock.writeToScreen("\033[3;4H");
    REQUIRE(screen.cursor().position == CellLocation { LineOffset(2), ColumnOffset(3) });
}

TEST_CASE("VT52: identify responds with ESC / Z", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    mock.writeToScreen("\033[?2l\033Z"); // enter VT52, then ESC Z (identify)
    REQUIRE(mock.terminal.peekInput() == "\033/Z");
}

TEST_CASE("VT52: erase to end of line and screen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("abcd\033[?2l");  // fill row 0, enter VT52
    mock.writeToScreen("\033Y\x20\x22"); // ESC Y: row 0, col 2
    mock.writeToScreen("\033K");         // ESC K -- erase to end of line
    REQUIRE(screen.grid().lineText(LineOffset(0)) == "ab  ");
}

TEST_CASE("Eight-bit C1 controls on input", "[screen]")
{
    // A raw byte in 0x80..0x9F that begins a character is a C1 control, exactly the 8-bit form of the
    // 7-bit ESC sequence: 0x9B is CSI, 0x84 IND, 0x9D OSC, 0x9C the string terminator, and so on.
    SECTION("8-bit CSI (0x9B) drives a CSI sequence")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\x9b"
                           "3;5H"); // CSI 3 ; 5 H (CUP)
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(2), ColumnOffset(4) });
    }

    SECTION("8-bit IND (0x84) indexes down one line")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 3 });
        mock.writeToScreen("\x84"); // IND
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(3), ColumnOffset(3) });
    }

    SECTION("8-bit OSC (0x9D) with 8-bit ST (0x9C) sets the window title")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\x9d"
                           "2;hi\x9c"); // OSC 2 ; hi ST
        CHECK(mock.terminal.windowTitle() == "hi");
    }

    SECTION("a C1-range byte inside a UTF-8 sequence stays a continuation byte")
    {
        // U+0250 encodes as 0xC9 0x90; the 0x90 is in the C1 range but here it is a UTF-8 continuation,
        // so it must print the character rather than be taken for a DCS control.
        auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };
        auto& screen = mock.terminal.primaryScreen();
        mock.writeToScreen("\xc9\x90X"); // ɐX
        CHECK(screen.realCursorPosition() == CellLocation { LineOffset(0), ColumnOffset(2) });
    }
}

TEST_CASE("LS1 and LS0", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };

    auto const writeTickAndRender = [&](auto text) {
        mock.writeToScreen(text);
        mock.terminal.tick(1s);
        mock.terminal.ensureFreshRenderBuffer();
        logScreenText(mock.terminal.primaryScreen(), std::format("writeTickAndRender: {}", e(text)));
    };

    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G0, CharsetId::USASCII));
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G1, CharsetId::USASCII));
    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab");

    // Set G1 to Special
    mock.writeToScreen("\033)0");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetTable::G1, CharsetId::Special));

    // LS1: load G1 into GL
    mock.writeToScreen("\x0E");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetId::Special));

    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab▒␉");

    // LS0: load G0 into GL
    mock.writeToScreen("\x0F");
    REQUIRE(mock.terminal.primaryScreen().cursor().charsets.isSelected(CharsetId::USASCII));

    writeTickAndRender("ab");
    REQUIRE(trimmedTextScreenshot(mock) == "ab▒␉ab");
}

TEST_CASE("LS2 and LS3 (locking shift into GL)", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // Designate G2 and G3 to DEC Special so the locking shift has something observable.
    mock.writeToScreen("\033*0"); // SCS G2 = DEC Special
    mock.writeToScreen("\033+0"); // SCS G3 = DEC Special
    REQUIRE(charsets.isSelected(CharsetTable::G2, CharsetId::Special));
    REQUIRE(charsets.isSelected(CharsetTable::G3, CharsetId::Special));
    REQUIRE(charsets.selectedTable() == CharsetTable::G0); // GL starts at G0

    // LS2 (ESC n): invoke G2 into GL.
    mock.writeToScreen("\033n");
    CHECK(charsets.selectedTable() == CharsetTable::G2);
    CHECK(charsets.isSelected(CharsetId::Special));

    // LS3 (ESC o): invoke G3 into GL.
    mock.writeToScreen("\033o");
    CHECK(charsets.selectedTable() == CharsetTable::G3);
    CHECK(charsets.isSelected(CharsetId::Special));

    // LS0 (SI): back to G0 (USASCII).
    mock.writeToScreen("\x0F");
    CHECK(charsets.selectedTable() == CharsetTable::G0);
    CHECK(charsets.isSelected(CharsetId::USASCII));
}

TEST_CASE("LS1R LS2R LS3R (locking shift into GR)", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // GR defaults to G2 per the VT standard.
    REQUIRE(charsets.selectedTableGR() == CharsetTable::G2);

    // LS1R (ESC ~): invoke G1 into GR.
    mock.writeToScreen("\033~");
    CHECK(charsets.selectedTableGR() == CharsetTable::G1);

    // LS3R (ESC |): invoke G3 into GR.
    mock.writeToScreen("\033|");
    CHECK(charsets.selectedTableGR() == CharsetTable::G3);

    // LS2R (ESC }): invoke G2 into GR (back to the default slot).
    mock.writeToScreen("\033}");
    CHECK(charsets.selectedTableGR() == CharsetTable::G2);

    // GR locking shifts must not disturb GL.
    CHECK(charsets.selectedTable() == CharsetTable::G0);
}

TEST_CASE("SCS 96-charset designation (ESC - / . / / )", "[screen]")
{
    auto mock = MockTerm { ColumnCount(8), LineCount(4) };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    // 96-charsets go into G1, G2, G3 (never G0). Only ISO Latin-1 supplemental ('A') is defined.
    mock.writeToScreen("\033-A"); // designate G1 = ISO Latin-1 supplemental
    CHECK(charsets.charsetIdOf(CharsetTable::G1) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G1));

    mock.writeToScreen("\033.A"); // designate G2
    CHECK(charsets.charsetIdOf(CharsetTable::G2) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G2));

    mock.writeToScreen("\033/A"); // designate G3
    CHECK(charsets.charsetIdOf(CharsetTable::G3) == CharsetId::ISOLatin1Supplemental);
    CHECK(charsets.is96Charset(CharsetTable::G3));

    // A subsequent 94-charset designation clears the 96-charset flag for that G-set.
    mock.writeToScreen("\033)B"); // designate G1 = USASCII (94-charset)
    CHECK(charsets.charsetIdOf(CharsetTable::G1) == CharsetId::USASCII);
    CHECK_FALSE(charsets.is96Charset(CharsetTable::G1));
    // G0 stays a 94-charset throughout (it cannot hold a 96-charset).
    CHECK_FALSE(charsets.is96Charset(CharsetTable::G0));
}

TEST_CASE("DECRQUPSS reports DEC Supplemental Graphic before any DECAUPSS", "[screen]")
{
    // The power-up default is DEC Supplemental Graphic, matching xterm's DFT_UPSS and vttest's own
    // reset_upss(). Ps=0 because it is a 94-character set.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033[&u"); // DECRQUPSS
    CHECK(e(mock.terminal.peekInput()) == e("\033P0!u%5\033\\"));
}

TEST_CASE("DECAUPSS round-trips every set in the table", "[screen]")
{
    // Data-driven over UpssTable itself: a new set is a new row, not a new test case. Each row must
    // survive assignment and come back byte-identical, with Ps re-derived from the set's own size.
    //
    // Contour reports back what was assigned rather than forcing ASCII as xterm does in UTF-8 mode --
    // a deliberate divergence. Contour tracks charset designations faithfully and reports them (as
    // DECCIR already does) without re-mapping decoded codepoints; answering "US ASCII" to an
    // application that just assigned DEC Supplemental Graphic would be a lie about state it set.
    for (auto const& entry: UpssTable)
    {
        auto designator = std::string {};
        if (entry.intermediate != '\0')
            designator += entry.intermediate;
        designator += entry.final;

        auto const ps = entry.is96 ? '1' : '0';
        INFO(std::format("UPSS designator '{}' at Ps={}", designator, ps));

        // VT525 so that even the VT500-era sets are assignable.
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033[65;1\"p"); // DECSCL -> VT level 5
        mock.writeToScreen(std::format("\033P{}!u{}\033\\", ps, designator));
        mock.terminal.flushInput(); // drain whatever DECSCL produced

        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e(std::format("\033P{}!u{}\033\\", ps, designator)));
    }
}

TEST_CASE("DECAUPSS Ps names the set's size, not a free parameter", "[screen]")
{
    // The sharpest edge in DECAUPSS: Ps is the character-set size, so the *same* designator names two
    // different sets depending on it, and a Ps that disagrees with the designator names none at all.
    // xterm's decode_upss skips any table row whose size differs from Ps.

    SECTION("'A' at Ps=0 is US ASCII (94), not ISO Latin-1")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033P0!uA\033\\");
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!uA\033\\"));
    }

    SECTION("'A' at Ps=1 is ISO Latin-1 (96), not US ASCII")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033P1!uA\033\\");
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P1!uA\033\\"));
    }

    SECTION("a size that disagrees with the designator assigns nothing")
    {
        // '%5' is a 94-character set, so Ps=1 names no set. UPSS must be left alone -- NOT coerced to
        // the 94-character reading of the same designator.
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033P1!uA\033\\"); // establish a non-default UPSS first
        mock.terminal.flushInput();

        mock.writeToScreen("\033P1!u%5\033\\"); // mismatched: %5 is 94-character
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P1!uA\033\\")); // unchanged
    }
}

TEST_CASE("DECAUPSS treats an omitted Ps as zero", "[screen]")
{
    // An omitted parameter is stored as 0 *and counted*, so a handler reading it must not fold zero
    // onto a default -- here zero is itself the meaningful value (94-character), which is why this
    // reads paramOr() rather than paramPositiveOr().
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033P1!uA\033\\"); // move UPSS off the default (ISO Latin-1, 96)
    mock.terminal.flushInput();

    mock.writeToScreen("\033P!u>\033\\"); // no Ps at all: must read as Ps=0 -> DEC Technical (94)
    mock.writeToScreen("\033[&u");
    CHECK(e(mock.terminal.peekInput()) == e("\033P0!u>\033\\"));
}

TEST_CASE("DECAUPSS ignores a designator that names no set", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033P0!uZZ\033\\"); // names nothing
    mock.writeToScreen("\033[&u");
    CHECK(e(mock.terminal.peekInput()) == e("\033P0!u%5\033\\")); // still the default
}

TEST_CASE("DECAUPSS gates a set on the conformance level DEC introduced it at", "[screen]")
{
    // Two tiers of gating. DECAUPSS itself is VT320+, handled by its Function tag. But the DEC/ISO
    // Greek, Hebrew, Turkish and Cyrillic sets are VT500-era, which is finer than that tag can express
    // and so is checked per row.
    //
    // Note DECSCL resets the terminal, so UPSS must be assigned *after* it, never before.

    SECTION("a VT500 set is refused at VT320")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033[63;1\"p"); // DECSCL -> VT level 3 (VT320)
        mock.terminal.flushInput();

        mock.writeToScreen("\033P0!u\"?\033\\"); // DEC Greek: VT500-era
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!u%5\033\\")); // unchanged
    }

    SECTION("the same set is accepted at VT525")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033[65;1\"p"); // DECSCL -> VT level 5
        mock.terminal.flushInput();

        mock.writeToScreen("\033P0!u\"?\033\\");
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!u\"?\033\\"));
    }

    SECTION("DECRQUPSS itself is unrecognised below VT320")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033[61\"p"); // DECSCL -> VT level 1 (VT100)
        mock.terminal.flushInput();

        mock.writeToScreen("\033[&u");
        CHECK(mock.terminal.peekInput().empty()); // a VT100 does not answer DECRQUPSS
    }
}

TEST_CASE("UPSS survives a screen switch and a cursor save/restore", "[screen]")
{
    // UPSS is a terminal-wide user preference, not cursor state. The G-set designations live on the
    // cursor, so a UPSS kept there would be destroyed by DECSC/DECRC and by every alternate-screen
    // switch -- the same reason XTCHECKSUM is terminal-level.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033P1!uA\033\\"); // ISO Latin-1
    mock.terminal.flushInput();

    SECTION("across the alternate screen")
    {
        mock.writeToScreen("\033[?1049h"); // to alt
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P1!uA\033\\"));
        mock.terminal.flushInput();

        mock.writeToScreen("\033[?1049l"); // back to primary
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P1!uA\033\\"));
    }

    SECTION("across DECSC/DECRC")
    {
        mock.writeToScreen("\0337"); // DECSC
        mock.writeToScreen("\033P0!u>\033\\");
        mock.writeToScreen("\0338"); // DECRC must not roll UPSS back
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!u>\033\\"));
    }
}

TEST_CASE("UPSS returns to its configured value on both kinds of reset", "[screen]")
{
    // xterm restores the charsets from ReallyReset() unconditionally, i.e. on DECSTR as well as RIS,
    // so both resets must put UPSS back.

    SECTION("soft reset (DECSTR)")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033P1!uA\033\\");
        mock.terminal.flushInput();

        mock.writeToScreen("\033[!p"); // DECSTR
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!u%5\033\\"));
    }

    SECTION("hard reset (RIS)")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
        mock.writeToScreen("\033P1!uA\033\\");
        mock.terminal.flushInput();

        mock.writeToScreen("\033c"); // RIS
        mock.writeToScreen("\033[&u");
        CHECK(e(mock.terminal.peekInput()) == e("\033P0!u%5\033\\"));
    }
}

TEST_CASE("SCS designator '<' designates the User-Preferred Supplemental Set", "[screen]")
{
    // '<' is unlike every other designator: it names no fixed set, but resolves to whatever DECAUPSS
    // last assigned (xterm's nrc_DEC_UPSS).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto const& charsets = mock.terminal.primaryScreen().cursor().charsets;

    SECTION("DECCIR reports '<', not the set it resolves to")
    {
        // Sdesig must round-trip the designation the application made. The resolved set is not what
        // was designated, and is not recoverable back to '<'.
        mock.writeToScreen("\033(<");
        CHECK(charsets.isUserPreferred(CharsetTable::G0));

        mock.writeToScreen(DECRQPSR(1));
        // Sdesig = "<BBB": G0 holds UPSS, G1..G3 are still USASCII.
        CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;<BBB\033\\"));
    }

    SECTION("it is the sanctioned way for G0 to hold a 96-character set")
    {
        // DEC STD 070 tells applications not to assume they may designate a 96-charset into G0, "but
        // that it is possible to do this using UPSS" -- so the slot's syntax does not decide the size
        // here; the resolved set's does. Scss' G0 bit must therefore be set.
        mock.writeToScreen("\033P1!uA\033\\"); // UPSS = ISO Latin-1, a 96-character set
        mock.terminal.flushInput();
        mock.writeToScreen("\033(<");

        CHECK(charsets.is96Charset(CharsetTable::G0));

        mock.writeToScreen(DECRQPSR(1));
        // Scss 0x40|1 = 'A': G0 holds a 96-charset.
        CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;A;<BBB\033\\"));
    }

    SECTION("a later designation clears the UPSS marker")
    {
        // The flag records a property of the *designation*, so anything that re-designates the G-set
        // must clear it -- otherwise DECCIR keeps reporting '<' for a set that is no longer UPSS.
        mock.writeToScreen("\033(<");
        REQUIRE(charsets.isUserPreferred(CharsetTable::G0));

        mock.writeToScreen("\033(B"); // designate G0 = USASCII
        CHECK_FALSE(charsets.isUserPreferred(CharsetTable::G0));

        mock.writeToScreen(DECRQPSR(1));
        CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;BBBB\033\\"));
    }

    SECTION("every G-set can hold it")
    {
        mock.writeToScreen("\033)<");
        mock.writeToScreen("\033*<");
        mock.writeToScreen("\033+<");
        CHECK(charsets.isUserPreferred(CharsetTable::G1));
        CHECK(charsets.isUserPreferred(CharsetTable::G2));
        CHECK(charsets.isUserPreferred(CharsetTable::G3));
    }

    SECTION("below VT320 the designator is not recognised")
    {
        mock.writeToScreen("\033[61\"p"); // DECSCL -> VT level 1
        mock.terminal.flushInput();

        mock.writeToScreen("\033(<");
        CHECK_FALSE(charsets.isUserPreferred(CharsetTable::G0));
    }
}

TEST_CASE("foldC1ControlsToEightBit", "[screen]")
{
    using vtbackend::foldC1ControlsToEightBit;
    // Each ESC-introduced C1 control folds to its single 8-bit byte (adjacent string literals keep the
    // \x?? escapes from greedily swallowing the following digit).
    CHECK(foldC1ControlsToEightBit("\033[0c")
          == std::string("\x9b"
                         "0c")); // CSI
    CHECK(foldC1ControlsToEightBit("\033P1$r5;6r\033\\")
          == std::string("\x90"
                         "1$r5;6r"
                         "\x9c")); // DCS..ST
    CHECK(foldC1ControlsToEightBit("\033]0;t\033\\")
          == std::string("\x9d"
                         "0;t"
                         "\x9c")); // OSC..ST
    // A non-C1 ESC (charset designation, intermediate 0x28 < 0x40), a lone trailing ESC, and plain text
    // all pass through untouched.
    CHECK(foldC1ControlsToEightBit("\033(B") == "\033(B");
    CHECK(foldC1ControlsToEightBit("ab\033") == "ab\033");
    CHECK(foldC1ControlsToEightBit("").empty());
    CHECK(foldC1ControlsToEightBit("no controls") == "no controls");
}

TEST_CASE("NRCS: British charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate British to G0: ESC ( A
    mock.writeToScreen("\033(A");
    // Write '#' which should map to '£' (U+00A3) in British charset
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // £ is U+00A3 — the UTF-8 encoding is 0xC2 0xA3
    CHECK(text.contains("\xC2\xA3"));
}

TEST_CASE("NRCS: German charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate German to G0: ESC ( K
    mock.writeToScreen("\033(K");
    // In German charset: '[' (0x5B) maps to 'Ä' (U+00C4)
    mock.writeToScreen("[");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.contains("\xC3\x84")); // Ä in UTF-8
}

TEST_CASE("NRCS: French charset substitution", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate French to G0: ESC ( R
    mock.writeToScreen("\033(R");
    // In French charset: '#' (0x23) maps to '£' (U+00A3)
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.contains("\xC2\xA3")); // £ in UTF-8
}

TEST_CASE("NRCS: switch back to USASCII", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set British
    mock.writeToScreen("\033(A");
    mock.writeToScreen("#"); // Should be £
    // Switch back to USASCII
    mock.writeToScreen("\033(B");
    mock.writeToScreen("#"); // Should be #
    mock.terminal.flushInput();
    // Column 0 has £, column 1 has #
    auto const col0 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(0) });
    auto const col1 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) });
    CHECK(col0 == "\xC2\xA3"); // £ in UTF-8
    CHECK(col1 == "#");
}

TEST_CASE("NRCS: G1 charset via locking shift", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate British to G1: ESC ) A
    mock.writeToScreen("\033)A");
    // Locking shift G1 (LS1 = SO = 0x0E)
    mock.writeToScreen("\x0E");
    // Write '#' — should map through G1 (British) → £
    mock.writeToScreen("#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.contains("\xC2\xA3"));
}

TEST_CASE("NRCS: DA1 includes ext 9", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(9)); // ext 9 = NationalReplacementCharacterSets
}

TEST_CASE("NRCS: two-byte DRCS designator accepted via SCS fallback", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // ESC ) <space> A — designate a DRCS set with two-byte designator into G1.
    // This must not produce an "Unknown VT sequence" error.
    mock.writeToScreen("\033) A");
    mock.terminal.flushInput();
    // Verify the sequence was consumed without error by writing text after it.
    // If the ESC sequence was rejected, the parser would have left stray characters on screen.
    mock.writeToScreen("OK");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "OK");
}

TEST_CASE("NRCS: single-byte SCS fallback designates British to G2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // ESC * A — designate British to G2 via the generic fallback path
    mock.writeToScreen("\033*A");
    mock.terminal.flushInput();
    // Verify G2 was set to British by using SS2 (single shift G2) and writing '#'
    // SS2 = ESC N, then '#' should map to '£' through British charset
    mock.writeToScreen("\033N#");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.contains("\xC2\xA3")); // £ in UTF-8
}

TEST_CASE("Technical charset: designate and use", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Designate DEC Technical to G0: ESC ( >
    mock.writeToScreen("\033(>");
    // Write 'A' (0x41) which maps to Α (Greek Alpha, U+0391) in Technical charset
    mock.writeToScreen("A");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // Α (U+0391) in UTF-8 is 0xCE 0x91
    CHECK(text.contains("\xCE\x91"));
}

TEST_CASE("Technical charset: pi mapping", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033(>");
    // 0x70 = 'p' maps to π (U+03C0) in Technical charset
    mock.writeToScreen("p");
    mock.terminal.flushInput();
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    // π (U+03C0) in UTF-8 is 0xCF 0x80
    CHECK(text.contains("\xCF\x80"));
}

TEST_CASE("Technical charset: switch back to USASCII", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033(>");
    mock.writeToScreen("A"); // Should be Α (Greek Alpha)
    mock.writeToScreen("\033(B");
    mock.writeToScreen("A"); // Should be regular A
    mock.terminal.flushInput();
    auto const col0 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(0) });
    auto const col1 =
        mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) });
    CHECK(col0 == "\xCE\x91"); // Α in UTF-8
    CHECK(col1 == "A");
}

TEST_CASE("Technical charset: ext 15 implied at level 65, listed at level 62", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(15)); // required at level 5, implied by 65

    // Downgrade to level 62 where ext 15 is optional
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(15)); // optional at level 2
}

TEST_CASE("DECDLD: define single character glyph", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 0, starting char 0x21 ('!'), erase all, 10x20 matrix
    // DCS 0;0;0;10;10;0;20;0 { <designator> <sixel data> ST
    // Simple glyph: first sixel column with bit 0 set = '?' (0x3F + 0 = 0x3F = '?'), wait that's 0.
    // Actually 0x3F is the base (all zeros). 0x40 = bit 0 set, 0x41 = bits 0+1, etc.
    // Let's define a minimal glyph with a single pixel set at (0,0): '@' = 0x40 - 0x3F = 1 bit
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    CHECK(charset->glyphs.contains(0x21)); // First glyph at starting position
}

TEST_CASE("DECDLD: erase control clears existing", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define first glyph
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) != nullptr);
    // Redefine with erase all (Pe=0)
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{B@\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    // The old glyph should be cleared, only new one exists
    CHECK(charset->glyphs.contains(0x21));
}

TEST_CASE("DECDLD: multiple glyphs separated by semicolons", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define two glyphs: '@' and 'A' (both just a single column with different bits)
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@;A\033\\");
    mock.terminal.flushInput();
    auto const* charset = mock.terminal.drcsCharset(0);
    REQUIRE(charset != nullptr);
    CHECK(charset->glyphs.contains(0x21)); // First glyph
    CHECK(charset->glyphs.contains(0x22)); // Second glyph
}

TEST_CASE("DECDLD: soft reset clears DRCS", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033P0;0;0;10;10;0;20;0{A@\033\\");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) != nullptr);
    // Soft reset
    mock.writeToScreen("\033[!p");
    mock.terminal.flushInput();
    CHECK(mock.terminal.drcsCharset(0) == nullptr);
}

TEST_CASE("DECDLD: DA1 includes ext 7 (SoftCharacterSet)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK(exts.contains(7)); // ext 7 = SoftCharacterSet
}

TEST_CASE("DECDLD: cell has image fragment after writing DRCS character", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 1 with designator ' A', single glyph at position 0x21 ('!')
    mock.writeToScreen("\033P1;0;0;10;10;0;20;0{ A@\033\\");
    mock.terminal.flushInput();

    // Verify DRCS font was stored with correct designator
    REQUIRE(mock.terminal.drcsDesignatorToFont(" A").has_value());
    CHECK(mock.terminal.drcsDesignatorToFont(" A").value() == 1);
    REQUIRE(mock.terminal.drcsCharset(1) != nullptr);
    CHECK(mock.terminal.drcsCharset(1)->glyphs.contains(0x21));

    // Designate DRCS font into G1: ESC ) <space> A
    mock.writeToScreen("\033) A");
    mock.terminal.flushInput();

    // Verify G1 was set to DRCS font 1
    REQUIRE(mock.terminal.currentScreen().cursor().charsets.drcsFont(CharsetTable::G1).has_value());
    CHECK(mock.terminal.currentScreen().cursor().charsets.drcsFont(CharsetTable::G1).value() == 1);

    // Switch to G1 (SO = 0x0E), write '!' (position 0x21), switch back (SI = 0x0F)
    mock.writeToScreen("\x0E");
    mock.terminal.flushInput();

    // Verify DRCS font is active
    REQUIRE(mock.terminal.currentScreen().cursor().charsets.activeDRCSFont().has_value());

    mock.writeToScreen("!");
    mock.writeToScreen("\x0F");
    mock.terminal.flushInput();

    // The cell at (0,0) should have an image fragment (the DRCS glyph bitmap)
    auto const& line = mock.terminal.currentScreen().grid().lineAt(LineOffset(0));
    CHECK(line.storage().imageFragments.has_value());
    if (line.storage().imageFragments.has_value())
        CHECK(line.storage().imageFragments->contains(0));
}

TEST_CASE("DECDLD: switching away from DRCS uses normal font", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define DRCS font 1
    mock.writeToScreen("\033P1;0;0;10;10;0;20;0{ A@\033\\");
    mock.terminal.flushInput();
    // Designate DRCS to G1 and switch to it
    mock.writeToScreen("\033) A\x0E!\x0F");
    mock.terminal.flushInput();
    // Write normal character 'X' through G0 (USASCII)
    mock.writeToScreen("X");
    mock.terminal.flushInput();
    // Column 1 ('X') should NOT have an image fragment
    auto const& line = mock.terminal.currentScreen().grid().lineAt(LineOffset(0));
    auto const hasImageAtCol1 =
        line.storage().imageFragments.has_value() && line.storage().imageFragments->contains(1);
    CHECK_FALSE(hasImageAtCol1);
    // But it should have the character 'X'
    CHECK(mock.terminal.currentScreen().cellTextAt({ .line = LineOffset(0), .column = ColumnOffset(1) })
          == "X");
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
