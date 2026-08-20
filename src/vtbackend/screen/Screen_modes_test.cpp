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
using crispy::escape;
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::literals::chrono_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

TEST_CASE("DSR.Unsolicited_ColorPaletteUpdated", "[screen]")
{
    auto const lightModeColors = []() -> ColorPalette {
        ColorPalette palette {};
        palette.defaultForeground = RGBColor { 0x00, 0x00, 0x00 };
        palette.defaultBackground = RGBColor { 0xff, 0xff, 0xff };
        return palette;
    }();

    auto const darkModeColors = []() -> ColorPalette {
        ColorPalette palette {};
        palette.defaultForeground = RGBColor { 0xff, 0xff, 0xff };
        palette.defaultBackground = RGBColor { 0x00, 0x00, 0x00 };
        return palette;
    }();

    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(3) } };

    REQUIRE_FALSE(mock.terminal.isModeEnabled(DECMode::ReportColorPaletteUpdated));

    // Set light mode colors
    mock.terminal.resetColorPalette(lightModeColors);

    // This must not trigger an unsolicited DSR by default.
    REQUIRE(escape(mock.replyData()).empty());

    // Request unsolicited DSRs for color palette updates.
    mock.writeToScreen(DECSM(toDECModeNum(DECMode::ReportColorPaletteUpdated)));
    // mock.terminal.setMode(DECMode::ReportColorPaletteUpdated, true); // FIXME (above)
    REQUIRE(mock.terminal.isModeEnabled(DECMode::ReportColorPaletteUpdated));

    // Set dark mode colors
    mock.terminal.resetColorPalette(lightModeColors);

    // This must trigger an unsolicited DSR.
    REQUIRE(escape(mock.replyData()) == escape("\033[?997;2n"sv));
    mock.resetReplyData();

    // Set light mode colors
    mock.terminal.resetColorPalette(darkModeColors);

    // This must trigger an unsolicited DSR.
    REQUIRE(escape(mock.replyData()) == escape("\033[?997;1n"sv));
    mock.resetReplyData();
}

TEST_CASE("InsertMode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGHIJ");
    screen.moveCursorTo(LineOffset(0), ColumnOffset(3));

    SECTION("basic insert shifts text right")
    {
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("XY");      // Insert "XY" at column 3
        CHECK(screen.renderMainPageText() == "ABCXYDEFGH\n");
        // "IJ" pushed past the right margin are lost
    }

    SECTION("disable insert returns to overwrite")
    {
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("X");       // Insert 'X' at column 3 -> "ABCXDEFGHI"
        mock.writeToScreen("\033[4l"); // Disable IRM
        mock.writeToScreen("Z");       // Overwrite at column 4 -> "ABCXZEFGHI"
        CHECK(screen.renderMainPageText() == "ABCXZEFGHI\n");
    }

    SECTION("insert single character at end of line")
    {
        screen.moveCursorTo(LineOffset(0), ColumnOffset(9));
        mock.writeToScreen("\033[4h"); // Enable IRM
        mock.writeToScreen("X");       // Insert at last column
        CHECK(screen.renderMainPageText() == "ABCDEFGHIX\n");
    }
}

TEST_CASE("LNM.VT_and_FF_honor_linefeed_mode", "[screen]")
{
    // In linefeed mode (LNM), VT and FF -- like LF -- return the carriage after indexing.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[20l");                                                     // RM LNM (off)
    mock.writeToScreen("\033[1;5H");                                                    // CUP row 1, col 5
    mock.writeToScreen("\013");                                                         // VT
    CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(4) }); // column kept

    mock.writeToScreen("\033[20h");                                                     // SM LNM (on)
    mock.writeToScreen("\033[1;5H");                                                    // CUP row 1, col 5
    mock.writeToScreen("\014");                                                         // FF
    CHECK(screen.cursor().position == CellLocation { LineOffset(1), ColumnOffset(0) }); // carriage returned
}

TEST_CASE("DECSCL.conformance_level_gating", "[screen]")
{
    SECTION("DECRQM (a VT300 feature) is silently gated below VT level 3")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

        mock.writeToScreen("\033[62;1\"p"); // DECSCL 62 -> VT level 2
        mock.resetReplyData();
        mock.writeToScreen("\033[4$p"); // DECRQM (ANSI) for IRM
        mock.terminal.flushInput();
        CHECK(mock.replyData().empty()); // level 2 does not answer DECRQM

        mock.writeToScreen("\033[63;1\"p"); // DECSCL 63 -> VT level 3
        mock.resetReplyData();
        mock.writeToScreen("\033[4$p"); // DECRQM (ANSI) for IRM
        mock.terminal.flushInput();
        CHECK(mock.replyData().contains("$y")); // level 3 answers CSI 4 ; Ps $ y
    }

    SECTION("DECSLRM (a VT420 feature) is inert below VT level 4")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

        mock.writeToScreen("\033[63;1\"p"); // DECSCL 63 -> VT level 3
        mock.writeToScreen("\033[?69h");    // DECSET DECLRMM (ignored below VT level 4)
        mock.writeToScreen("\033[5;6s");    // DECSLRM 5;6 (gated below VT level 4)
        mock.writeToScreen("\033[1;5H");    // CUP row 1, col 5
        mock.writeToScreen("abc");
        // No left/right margin is in force, so the cursor flows on to column 8 (col 5 + "abc").
        CHECK(mock.terminal.primaryScreen().cursor().position.column == ColumnOffset(7));
    }

    SECTION("DECSCL stays reachable at every level so the level can always be raised again")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

        mock.writeToScreen("\033[61\"p"); // DECSCL 61 -> VT level 1 (single-parameter form)
        CHECK(mock.terminal.operatingLevel() == VTType::VT100);
        mock.writeToScreen("\033[65;1\"p"); // DECSCL 65 -> VT level 5, from level 1
        CHECK(mock.terminal.operatingLevel() == VTType::VT525);
    }
}

TEST_CASE("DECSTR.resets_left_right_margin_mode", "[screen]")
{
    // DEC STD 070: a soft reset (DECSTR) resets DECLRMM, so a subsequent DECSLRM is inert.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    mock.writeToScreen("\033[?69h"); // DECSET DECLRMM
    CHECK(mock.terminal.isModeEnabled(DECMode::LeftRightMargin));

    mock.writeToScreen("\033[!p"); // DECSTR (soft reset)
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::LeftRightMargin));

    // With DECLRMM off, DECSLRM sets no margin: "abc" from column 3 flows on to column 6.
    mock.writeToScreen("\033[2;4s"); // DECSLRM 2;4 (inert)
    mock.writeToScreen("\033[1;3H"); // CUP row 1, col 3
    mock.writeToScreen("abc");
    CHECK(mock.terminal.primaryScreen().cursor().position.column == ColumnOffset(5));
}

TEST_CASE("DECRQSS reports the scroll-region margins", "[screen]")
{
    // DECRQSS replies DCS 1 $ r <setting> ST. The margins are stored 0-based and inclusive, so both
    // bounds convert back to the 1-based values the sequence was given. Mirrors esctest
    // test_DECRQSS_DECSTBM and test_DECRQSS_DECSLRM.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    SECTION("DECSTBM top;bottom")
    {
        mock.writeToScreen("\033[5;6r");      // DECSTBM 5;6
        mock.writeToScreen("\033P$qr\033\\"); // DECRQSS "r"
        CHECK("\033P1$r5;6r\033\\" == mock.terminal.peekInput());
    }

    SECTION("DECSLRM left;right")
    {
        mock.writeToScreen("\033[?69h");      // DECSET DECLRMM
        mock.writeToScreen("\033[3;4s");      // DECSLRM 3;4
        mock.writeToScreen("\033P$qs\033\\"); // DECRQSS "s"
        CHECK("\033P1$r3;4s\033\\" == mock.terminal.peekInput());
    }
}

TEST_CASE("DECRQSS reports the current SGR", "[screen]")
{
    // The SGR report leads with a 0 (reset) and then lists only the attributes that differ from the
    // default -- default colours are implied by the reset, not spelled out. Mirrors esctest
    // test_DECRQSS_SGR.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };

    SECTION("a single attribute, default colours omitted")
    {
        mock.writeToScreen("\033[1m");        // SGR bold
        mock.writeToScreen("\033P$qm\033\\"); // DECRQSS "m"
        CHECK("\033P1$r0;1m\033\\" == mock.terminal.peekInput());
    }

    SECTION("several attributes")
    {
        mock.writeToScreen("\033[1;3m");      // SGR bold + italic
        mock.writeToScreen("\033P$qm\033\\"); // DECRQSS "m"
        CHECK("\033P1$r0;1;3m\033\\" == mock.terminal.peekInput());
    }

    SECTION("colours, including an INDEXED underline colour")
    {
        // The report and `capture-pane -e` now share one encoder (vtbackend/SgrWriter.h). They used
        // to hold a copy each, and both copies were wrong in a different place: the capture's knew
        // nothing about SGR 58 at all, and this one only ever spelled an RGB underline colour — so
        // `\e[58:5:1m`, which this terminal happily parses, was reported back as no colour at all.
        mock.writeToScreen("\033[31;44;4:3;58:5:1m"); // red fg, blue bg, curly underline in red
        mock.writeToScreen("\033P$qm\033\\");         // DECRQSS "m"
        CHECK("\033P1$r0;4:3;31;44;58;5;1m\033\\" == mock.terminal.peekInput());
    }
}

TEST_CASE("DECRQSS reports the attribute change extent (DECSACE)", "[screen]")
{
    // DECSACE selects stream vs rectangle for DECCARA/DECRARA; DECRQSS "*x" reports it. Mirrors esctest
    // test_DECRQSS_DECSACE, which had no mapping at all.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };

    SECTION("stream mode reports 0")
    {
        mock.writeToScreen("\033[0*x");        // DECSACE 0 (stream)
        mock.writeToScreen("\033P$q*x\033\\"); // DECRQSS "*x"
        CHECK("\033P1$r0*x\033\\" == mock.terminal.peekInput());
    }

    SECTION("rectangle mode reports 2")
    {
        mock.writeToScreen("\033[2*x");        // DECSACE 2 (rectangle)
        mock.writeToScreen("\033P$q*x\033\\"); // DECRQSS "*x"
        CHECK("\033P1$r2*x\033\\" == mock.terminal.peekInput());
    }
}

TEST_CASE("DECRQSS reports the VT525 keyboard settings", "[screen]")
{
    // DECELF (CSI Pn +q), DECLFKC (CSI Pn *}) and DECSMKR (CSI Pn +r) are keyboard settings Contour
    // remembers and hands back verbatim through DECRQSS, though nothing acts on them. Mirrors esctest
    // test_DECRQSS_DECELF/DECLFKC/DECSMKR (which had no mapping at all).
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };

    SECTION("DECELF")
    {
        mock.writeToScreen("\033[0+q");        // DECELF 0
        mock.writeToScreen("\033P$q+q\033\\"); // DECRQSS "+q"
        CHECK("\033P1$r0+q\033\\" == mock.terminal.peekInput());
    }

    SECTION("DECLFKC")
    {
        mock.writeToScreen("\033[0*}");        // DECLFKC 0
        mock.writeToScreen("\033P$q*}\033\\"); // DECRQSS "*}"
        CHECK("\033P1$r0*}\033\\" == mock.terminal.peekInput());
    }

    SECTION("DECSMKR")
    {
        mock.writeToScreen("\033[0+r");        // DECSMKR 0
        mock.writeToScreen("\033P$q+r\033\\"); // DECRQSS "+r"
        CHECK("\033P1$r0+r\033\\" == mock.terminal.peekInput());
    }

    SECTION("a non-default value is remembered and reported")
    {
        mock.writeToScreen("\033[2+r");        // DECSMKR 2
        mock.writeToScreen("\033P$q+r\033\\"); // DECRQSS "+r"
        CHECK("\033P1$r2+r\033\\" == mock.terminal.peekInput());
    }
}

TEST_CASE("S8C1T selects 8-bit C1 control transmission for replies", "[screen]")
{
    // With S7C1T (the default) the terminal frames its replies with 7-bit ESC-introduced C1 controls;
    // with S8C1T selected at VT level >= 2 it uses the single-byte 8-bit forms instead. @see
    // Terminal::reply(), foldC1ControlsToEightBit().
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("default is 7-bit")
    {
        mock.writeToScreen("\033[6n"); // DSR: report cursor position
        CHECK(mock.terminal.peekInput() == "\033[1;1R");
    }

    SECTION("8-bit after S8C1T")
    {
        mock.writeToScreen("\033 G");  // S8C1T (ESC SP G)
        mock.writeToScreen("\033[6n"); // DSR
        CHECK(mock.terminal.peekInput()
              == std::string("\x9b"
                             "1;1R")); // 8-bit CSI introducer
    }
}

TEST_CASE("DECID identifies the terminal like DA1", "[screen]")
{
    // DECID (ESC Z) is the VT100 "identify terminal" control; it is answered with the primary device
    // attributes, exactly as DA1 (CSI c). Mirrors esctest test_DECID_8bit (which sends it 8-bit).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033Z"); // DECID
    auto const viaDecid = std::string { mock.terminal.peekInput() };
    mock.discardPendingReplies();

    mock.writeToScreen("\033[c"); // DA1
    auto const viaDa1 = std::string { mock.terminal.peekInput() };

    CHECK(viaDecid.starts_with("\033[?"));
    CHECK(viaDecid.ends_with("c"));
    CHECK(viaDecid == viaDa1);
}

TEST_CASE("ReportCursorPosition", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(mock.terminal.peekInput().empty());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("with Origin mode disabled")
    {
        screen.reportCursorPosition();
        CHECK("\033[2;3R" == mock.terminal.peekInput());
    }

    SECTION("with margins and origin mode enabled")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });

        screen.reportCursorPosition();
        CHECK("\033[3;2R" == mock.terminal.peekInput());
    }
}

TEST_CASE("ReportExtendedCursorPosition", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo(LineOffset { 1 }, ColumnOffset { 2 });

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderMainPageText());
    REQUIRE(mock.terminal.peekInput().empty());
    REQUIRE(screen.logicalCursorPosition() == CellLocation { LineOffset(1), ColumnOffset(2) });

    SECTION("with Origin mode disabled")
    {
        screen.reportExtendedCursorPosition();
        CHECK("\033[?2;3;1R" == mock.terminal.peekInput());
    }

    SECTION("with margins and origin mode enabled")
    {
        mock.terminal.setMode(DECMode::LeftRightMargin, true);
        mock.terminal.setLeftRightMargin(ColumnOffset { 1 }, ColumnOffset { 3 });
        mock.terminal.setTopBottomMargin(LineOffset { 1 }, LineOffset { 3 });
        mock.terminal.setMode(DECMode::Origin, true);
        screen.moveCursorTo(LineOffset { 2 }, ColumnOffset { 1 });

        screen.reportExtendedCursorPosition();
        CHECK("\033[?3;2;1R" == mock.terminal.peekInput());
    }
}

TEST_CASE("RequestMode", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) } };

    constexpr auto AnsiInsertModeNum = toAnsiModeNum(AnsiMode::Insert);

    SECTION("ANSI modes: enabled")
    {
        mock.writeToScreen(SM(AnsiInsertModeNum));
        mock.writeToScreen(DECRQM_ANSI(AnsiInsertModeNum));
        REQUIRE(e(mock.terminal.peekInput())
                == e(std::format("\033[{};1$y", toAnsiModeNum(AnsiMode::Insert))));
    }

    SECTION("ANSI modes: disabled")
    {
        mock.writeToScreen(RM(AnsiInsertModeNum));
        mock.writeToScreen(DECRQM_ANSI(AnsiInsertModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[{};2$y", AnsiInsertModeNum)));
    }

    SECTION("ANSI modes: unknown")
    {
        auto const m = 1234u;
        mock.writeToScreen(SM(m));
        mock.writeToScreen(DECRQM_ANSI(m));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[{};0$y", m)));
    }

    constexpr auto DecOriginModeNum = toDECModeNum(DECMode::Origin);

    SECTION("DEC modes: enabled")
    {
        mock.writeToScreen(DECSM(DecOriginModeNum));
        mock.writeToScreen(DECRQM(DecOriginModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};1$y", DecOriginModeNum)));
    }

    SECTION("DEC modes: disabled")
    {
        mock.writeToScreen(DECRM(DecOriginModeNum));
        mock.writeToScreen(DECRQM(DecOriginModeNum));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};2$y", DecOriginModeNum)));
    }

    SECTION("DEC modes: unknown")
    {
        auto const m = std::numeric_limits<uint16_t>::max();
        mock.writeToScreen(DECSM(m));
        mock.writeToScreen(DECRQM(m));
        REQUIRE(e(mock.terminal.peekInput()) == e(std::format("\033[?{};0$y", m)));
    }
}

TEST_CASE("DECNKM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Enable application keypad via DECSM 66
    mock.writeToScreen(DECSM(66));
    CHECK(mock.terminal.isModeEnabled(DECMode::ApplicationKeypad));

    // Disable via DECRM 66
    mock.writeToScreen(DECRM(66));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::ApplicationKeypad));

    // DECRQM should report correctly
    mock.writeToScreen(DECSM(66));
    mock.writeToScreen(DECRQM(66));
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?66;1$y"));
}

TEST_CASE("DECARM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Auto-repeat should be enabled by default (VT100 spec)
    CHECK(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    // Disable auto-repeat via DECRM 8
    mock.writeToScreen(DECRM(8));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    // Re-enable via DECSM 8
    mock.writeToScreen(DECSM(8));
    CHECK(mock.terminal.isModeEnabled(DECMode::AutoRepeat));

    SECTION("DECRQM reports set when enabled")
    {
        mock.writeToScreen(DECSM(8));
        mock.writeToScreen(DECRQM(8));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?8;1$y"));
    }

    SECTION("DECRQM reports reset when disabled")
    {
        mock.writeToScreen(DECRM(8));
        mock.writeToScreen(DECRQM(8));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?8;2$y"));
    }
}

TEST_CASE("DECBKM", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    // Backarrow key mode should be disabled by default (VT340/VT420 spec)
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    // Enable backarrow key mode via DECSM 67
    mock.writeToScreen(DECSM(67));
    CHECK(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    // Disable via DECRM 67
    mock.writeToScreen(DECRM(67));
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BackarrowKey));

    SECTION("DECRQM reports set when enabled")
    {
        mock.writeToScreen(DECSM(67));
        mock.writeToScreen(DECRQM(67));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?67;1$y"));
    }

    SECTION("DECRQM reports reset when disabled")
    {
        mock.writeToScreen(DECRM(67));
        mock.writeToScreen(DECRQM(67));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[?67;2$y"));
    }
}

TEST_CASE("DECTABSR", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(35) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("default tabstops")
    {
        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u1/9/17/25/33\033\\"));
    }

    SECTION("cleared tabs")
    {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);
        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u1/9/17/25/33\033\\"));
    }

    SECTION("custom tabstops")
    {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);

        screen.moveCursorToColumn(ColumnOffset(1));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(3));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(7));
        screen.horizontalTabSet();

        screen.moveCursorToColumn(ColumnOffset(15));
        screen.horizontalTabSet();

        screen.requestTabStops();
        CHECK(e(mock.terminal.peekInput()) == e("\033P2$u2/4/8/16\033\\"));
    }
}

TEST_CASE("DECSTR", "[screen]")
{
    // Create a 10x3x5 grid and render a 7x5 image causing one a line-scroll by one.
    auto const pageSize = PageSize { LineCount(4), ColumnCount(10) };
    auto mock = MockTerm { pageSize, LineCount(5) };
    mock.writeToScreen("ABCD\r\nDEFG\r\n");
    CHECK(mock.terminal.primaryScreen().cursor().position.line == LineOffset(2));
    CHECK(mock.terminal.primaryScreen().cursor().position.column == ColumnOffset(0));

    mock.writeToScreen("\033[!p");
    REQUIRE(mock.terminal.primaryScreen().cursor().position
            == CellLocation { LineOffset(2), ColumnOffset(0) });
    REQUIRE(mock.terminal.primaryScreen().savedCursorState().position
            == CellLocation { LineOffset(0), ColumnOffset(0) });
}

TEST_CASE("DECTST", "[screen]")
{
    // DECTST runs the terminal's built-in confidence tests. Contour has no hardware to test, so every
    // test passes and nothing is reported -- a failure would be DRAWN as a diagnostic code, never
    // replied. The one observable effect is the power-up self test, which resets the terminal.
    auto const pageSize = PageSize { LineCount(4), ColumnCount(10) };

    auto const isPristine = [](MockTerm<vtpty::MockPty>& mock) {
        return mock.terminal.primaryScreen().renderMainPageText()
               == "          \n          \n          \n          \n";
    };

    SECTION("the VT100 invoke opcode (2) runs the power-up self test, which resets")
    {
        // vttest sends exactly this, to every terminal it meets, whatever the terminal reports itself
        // to be. @see vttest reset.c:36 `dectst(1)` -> esc.c:1126 `brc2(2, pn, 'y')`.
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        REQUIRE_FALSE(isPristine(mock));

        mock.writeToScreen("\033[2;1y");
        CHECK(isPristine(mock));
        // A terminal that passes its tests says nothing at all.
        CHECK(mock.terminal.peekInput().empty());
    }

    SECTION("the VT510 invoke opcode (4) does the same")
    {
        // The sequence changed shape between generations -- VT100 invokes with 2, VT510 with 4 -- and a
        // terminal reporting VT525 must still answer VT100-era software. @see vt100.net DECTST.
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[4;1y");
        CHECK(isPristine(mock));
    }

    SECTION("test 0 runs all tests, and so resets too")
    {
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[2;0y");
        CHECK(isPristine(mock));
    }

    SECTION("a loopback test has no hardware to drive, so it changes nothing")
    {
        // 2 = RS-232 data loopback: there is no port, nothing to loop, and nothing that can fail. It
        // must not be mistaken for the power-up test and reset the screen.
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[2;2y");
        CHECK_FALSE(isPristine(mock));
        CHECK(mock.terminal.peekInput().empty());
    }

    SECTION("naming no test runs no test")
    {
        // DECTST invokes what it is asked for, and `CSI 2 y` asks for nothing.
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[2y");
        CHECK_FALSE(isPristine(mock));
    }

    SECTION("a Ps1 that is not an invoke opcode is not DECTST")
    {
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[3;1y");
        CHECK_FALSE(isPristine(mock));
    }

    SECTION("an unassigned test number is rejected, and resets nothing on the way")
    {
        // The power-up test is named first and the invalid one second: validating the whole string
        // before running anything is what keeps a rejected DECTST from leaving the terminal half-reset.
        auto mock = MockTerm { pageSize };
        mock.writeToScreen("ABCD");
        mock.writeToScreen("\033[2;1;42y");
        CHECK_FALSE(isPristine(mock));
    }
}

TEST_CASE("DECCIR.default_state", "[screen]")
{
    // Verify DECCIR response with all defaults: cursor at (1,1), no attributes, no wrap pending,
    // GL=G0, GR=G2, all charsets USASCII.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(DECRQPSR(1));

    // Expected: DCS 1 $ u 1;1;1;@;@;@;0;2;@;BBBB ST
    //   Pr=1, Pc=1, Pp=1
    //   Srend='@' (0x40, no attributes)
    //   Satt='@' (0x40, no protection)
    //   Sflag='@' (0x40, no flags)
    //   Pgl=0 (G0 in GL)
    //   Pgr=2 (G2 in GR, default)
    //   Scss='@' (0x40, all 94-char sets)
    //   Sdesig="BBBB" (all USASCII)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.cursor_position", "[screen]")
{
    // Verify DECCIR correctly reports cursor position after movement.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };

    mock.writeToScreen(CUP(3, 7)); // Move to line 3, column 7

    mock.writeToScreen(DECRQPSR(1));

    // Pr=3, Pc=7
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u3;7;1;@;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.bold_and_underline", "[screen]")
{
    // Verify Srend field encodes bold (bit 1) and underline (bit 2).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(1)); // Bold
    mock.writeToScreen(SGR(4)); // Underline
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x01 (bold) + 0x02 (underline) = 0x43 = 'C'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;C;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.blinking_and_inverse", "[screen]")
{
    // Verify Srend field encodes blinking (bit 3) and inverse (bit 4).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(5)); // Blinking
    mock.writeToScreen(SGR(7)); // Inverse
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x04 (blink) + 0x08 (inverse) = 0x4C = 'L'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;L;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.all_rendition_attributes", "[screen]")
{
    // Verify Srend field with all attributes enabled: bold+underline+blink+inverse.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SGR(1)); // Bold
    mock.writeToScreen(SGR(4)); // Underline
    mock.writeToScreen(SGR(5)); // Blinking
    mock.writeToScreen(SGR(7)); // Inverse
    mock.writeToScreen(DECRQPSR(1));

    // Srend = 0x40 + 0x01 + 0x02 + 0x04 + 0x08 = 0x4F = 'O'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;O;@;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.character_protection", "[screen]")
{
    // Verify Satt field reports DECSCA character protection attribute.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(DECSCA(1)); // Enable character protection
    mock.writeToScreen(DECRQPSR(1));

    // Satt = 0x41 = 'A' (bit 1 set for protection)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;A;@;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.origin_mode", "[screen]")
{
    // Verify Sflag bit 1 reports origin mode (DECOM).
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };

    mock.writeToScreen(DECSM(toDECModeNum(DECMode::Origin)));
    mock.writeToScreen(DECRQPSR(1));

    // Sflag = 0x40 + 0x01 = 0x41 = 'A' (origin mode set)
    // Note: cursor is at (1,1) because origin mode homes the cursor.
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;A;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.wrap_pending", "[screen]")
{
    // Verify Sflag bit 4 reports wrap-pending state.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    // Write exactly enough characters to reach the right margin and trigger wrap pending.
    mock.writeToScreen("ABCDE");
    mock.writeToScreen(DECRQPSR(1));

    // Cursor is at column 5, wrap pending. Sflag = 0x40 + 0x08 = 0x48 = 'H'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;5;1;@;@;H;0;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.charset_designation_special", "[screen]")
{
    // Verify Sdesig reports DEC Special charset when designated into G0.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SCS_G0_SPECIAL()); // Designate G0 = DEC Special
    mock.writeToScreen(DECRQPSR(1));

    // Sdesig: G0='0' (Special), G1-G3='B' (USASCII)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;0BBB\033\\"));
}

TEST_CASE("DECCIR.charset_designation_g1", "[screen]")
{
    // Verify Sdesig reports charset designated into G1.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen(SCS_G1_SPECIAL()); // Designate G1 = DEC Special
    mock.writeToScreen(DECRQPSR(1));

    // Sdesig: G0='B', G1='0' (Special), G2-G3='B'
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;@;B0BB\033\\"));
}

TEST_CASE("DECCIR.gl_charset_after_locking_shift", "[screen]")
{
    // Verify Pgl reports G1 after a locking shift (SO → LS1 maps G1 into GL).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\x0E"); // SO (Shift Out) = LS1 → map G1 into GL
    mock.writeToScreen(DECRQPSR(1));

    // Pgl=1 (G1 in GL)
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;1;2;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.gr_charset_after_locking_shift", "[screen]")
{
    // Verify Pgr reports the GR register after LS3R maps G3 into GR (default is G2).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033|"); // LS3R → map G3 into GR
    mock.writeToScreen(DECRQPSR(1));

    // Pgr=3 (G3 in GR); Pgl stays 0.
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;3;@;BBBB\033\\"));
}

TEST_CASE("DECCIR.scss_reports_96_charset", "[screen]")
{
    // Verify Scss sets the per-G-set size bit and Sdesig reports 'A' when a 96-charset is designated.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033-A"); // designate G1 = ISO Latin-1 supplemental (96-charset)
    mock.writeToScreen(DECRQPSR(1));

    // Scss = 0x40 | (1 << 1) = 0x42 = 'B'; Sdesig G1 = 'A' (Latin-1). Pgl=0, Pgr=2 (defaults).
    CHECK(e(mock.terminal.peekInput()) == e("\033P1$u1;1;1;@;@;@;0;2;B;BABB\033\\"));
}

TEST_CASE("DA1: response reports level 65 for VT525", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DA1: optional extensions at level 65", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());

    // Optional at level 5 — should be listed
    CHECK(exts.contains(1));  // Columns132
    CHECK(exts.contains(4));  // SixelGraphics
    CHECK(exts.contains(18)); // Windowing
    CHECK(exts.contains(21)); // HorizontalScrolling
    CHECK(exts.contains(22)); // AnsiColor

    // Required at level 5 — implied by 65, must NOT be listed
    CHECK_FALSE(exts.contains(6));  // SelectiveErase (required at level 5)
    CHECK_FALSE(exts.contains(8));  // UserDefinedKeys (required at level 5)
    CHECK_FALSE(exts.contains(11)); // StatusDisplay (required at level 3+)
    CHECK_FALSE(exts.contains(15)); // TechnicalCharacters (required at level 5)
    CHECK_FALSE(exts.contains(28)); // RectangularEditing (required at level 4+)
    CHECK_FALSE(exts.contains(32)); // TextMacros (required at level 5)

    // Non-DEC extensions — always listed
    CHECK(exts.contains(52));  // ClipboardExtension
    CHECK(exts.contains(314)); // CaptureScreenBuffer
}

TEST_CASE("DECSCL: DA1 always reports max level 65", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set conformance level to 62 (VT220), 7-bit C1
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    // DA1 always reports max device capability, not operating level
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DECSCL: level 62 reveals required-at-5 extensions as optional", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    auto const exts = parseDA1Extensions(mock.replyData());
    // At level 2, all these become optional and should be listed
    CHECK(exts.contains(6));  // SelectiveErase (optional at 2-4)
    CHECK(exts.contains(8));  // UserDefinedKeys (optional at 2-4)
    CHECK(exts.contains(11)); // StatusDisplay (optional at 2)
    CHECK(exts.contains(15)); // TechnicalCharacters (optional at 2-4)
    CHECK(exts.contains(28)); // RectangularEditing (optional at 2-3)
    CHECK(exts.contains(32)); // TextMacros (optional at 2-4)
}

TEST_CASE("DECSCL: level 63 hides StatusDisplay (required at 3+)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[63;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(11)); // StatusDisplay required at level 3+
    CHECK(exts.contains(6));        // SelectiveErase still optional at level 3
    CHECK(exts.contains(28));       // RectangularEditing still optional at level 3
}

TEST_CASE("DECSCL: level 64 hides RectangularEditing (required at 4+)", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[64;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
    auto const exts = parseDA1Extensions(mock.replyData());
    CHECK_FALSE(exts.contains(11)); // StatusDisplay required at 3+
    CHECK_FALSE(exts.contains(28)); // RectangularEditing required at 4+
    CHECK(exts.contains(6));        // SelectiveErase still optional at level 4
    CHECK(exts.contains(8));        // UserDefinedKeys still optional at level 4
    CHECK(exts.contains(15));       // TechnicalCharacters still optional at level 4
    CHECK(exts.contains(32));       // TextMacros still optional at level 4
}

TEST_CASE("DECSCL: set level 65 round-trip", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // First downgrade to 62
    mock.writeToScreen("\033[62;1\"p");
    mock.terminal.flushInput();
    // Then upgrade back to 65
    mock.writeToScreen("\033[65;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();
    CHECK(parseDA1Level(mock.replyData()) == 65);
}

TEST_CASE("DECSCL: implies soft reset", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Move cursor away from origin
    mock.writeToScreen("\033[3;5H"); // cursor to row 3, col 5
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().cursor().position.line != LineOffset(0));

    // Set conformance level — this implies soft reset, which should reset margins
    // but cursor position is reset by the soft reset
    mock.writeToScreen("\033[65;1\"p");
    mock.terminal.flushInput();

    // After soft reset, origin mode is off and cursor is at home position
    // softReset resets DECOM, so cursor should be at the top-left area
    // Note: soft reset doesn't explicitly move cursor, but resets margins and modes
    CHECK(mock.terminal.operatingLevel() == VTType::VT525);
}

TEST_CASE("DECSCL: C1 mode 7-bit", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;1\"p"); // Ps2=1 → 7-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S7C1T);
}

TEST_CASE("DECSCL: C1 mode 8-bit", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;0\"p"); // Ps2=0 → 8-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);
}

TEST_CASE("DECSCL: C1 mode 8-bit with Ps2=2", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[65;2\"p"); // Ps2=2 → 8-bit C1
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);
}

TEST_CASE("DECSCL: level 61 forces 7-bit C1", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // VT100 (level 61) always uses 7-bit, regardless of Ps2
    mock.writeToScreen("\033[61;0\"p");
    mock.terminal.flushInput();
    CHECK(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S7C1T);
}

TEST_CASE("DECSCL: DECRQSS reports current level", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Set to level 64 (VT420) — DECRQSS requires VT420, so we must stay at level 4+
    mock.writeToScreen("\033[64;1\"p");
    mock.terminal.flushInput();
    mock.resetReplyData();
    // Query DECSCL via DECRQSS
    mock.writeToScreen("\033P$q\"p\033\\");
    mock.terminal.flushInput();
    auto const reply = mock.replyData();
    // Should contain "64;1" for level 64 with 7-bit C1
    CHECK(reply.contains("64;1\"p"));
}

TEST_CASE("S8C1T: DECRQSS reply uses 8-bit C1 at VT level >= 2", "[screen]")
{
    // Mirrors esctest S8C1TTests.test_S8C1T_DCS: DECSTBM(5,6), select 8-bit C1 transmission, then read
    // the DECRQSS(DECSTBM) reply, which must be framed with the 8-bit DCS (0x90) and ST (0x9c).
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[5;6r"); // DECSTBM top=5 bottom=6
    mock.writeToScreen("\033 G");    // S8C1T (ESC SP G): select 8-bit C1 transmission
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);

    mock.resetReplyData();
    mock.writeToScreen("\033P$qr\033\\"); // DECRQSS(DECSTBM)
    mock.terminal.flushInput();
    CHECK(mock.replyData()
          == std::string("\x90"
                         "1$r5;6r"
                         "\x9c"));
}

TEST_CASE("S8C1T: replies revert to 7-bit after a VT52 round-trip", "[screen]")
{
    // 8-bit C1 transmission is a VT200+ capability. Leaving VT52 (ESC <) drops the operating level to
    // VT100, where it is unavailable, so replies revert to 7-bit even though S8C1T remains selected
    // (xterm's CASE_VT52_FINISH rule). Uses DSR-CPR, which is available at VT100 level.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033 G"); // S8C1T at the default VT525 level
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.c1TransmissionMode() == ControlTransmissionMode::S8C1T);

    // At VT525 level the CPR reply is 8-bit (0x9b introducer).
    mock.writeToScreen("\033[3;4H"); // CUP row 3 col 4
    mock.resetReplyData();
    mock.writeToScreen("\033[6n"); // DSR: cursor position report
    mock.terminal.flushInput();
    CHECK(mock.replyData()
          == std::string("\x9b"
                         "3;4R"));

    // Round-trip through VT52; on exit the operating level is VT100.
    mock.writeToScreen("\033[?2l\033<"); // enter VT52 (DECANM reset), then leave it (ESC <)
    mock.terminal.flushInput();
    REQUIRE(mock.terminal.operatingLevel() == VTType::VT100);

    mock.writeToScreen("\033[3;4H");
    mock.resetReplyData();
    mock.writeToScreen("\033[6n");
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\033[3;4R"); // 7-bit CSI introducer
}

TEST_CASE("DECSCL resets the terminal (esctest DECSCL_RISOnChange)", "[screen]")
{
    // DECSCL erases the screen, returns the saved cursor to the origin, and clears insert mode -- the
    // observable reset effects the suite checks -- while leaving hardware-capability modes alone.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    mock.writeToScreen("x");          // 'x' at (0,0)
    mock.writeToScreen("\033[3;4H");  // CUP to (row 3, col 4)
    mock.writeToScreen("\0337");      // DECSC: save the cursor away from the origin
    mock.writeToScreen("\033[4h");    // SM IRM: insert mode on
    mock.writeToScreen("\033[61\"p"); // DECSCL(61): drop to VT100, resetting the terminal

    // The screen is erased.
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)) == "          ");

    // The saved cursor is back at the origin: DECRC returns there.
    mock.writeToScreen("\0338"); // DECRC
    CHECK(mock.terminal.currentScreen().cursor().position == CellLocation { LineOffset(0), ColumnOffset(0) });

    // Insert mode was cleared, so a second write replaces rather than shifts.
    mock.writeToScreen("\033[1;1Ha\033[1;1Hb");
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "b ");
}

TEST_CASE("DECSET 41 (MoreFix): a tab honours a pending wrap", "[screen]")
{
    // esctest DECSETTests.test_DECSET_MoreFix. Fill the line to the right margin (so a wrap is pending),
    // then TAB. With MoreFix on the tab honours the pending wrap and lands on the next line's first tab
    // stop; without it (the default) the tab is swallowed at the right margin and the wrap waits.
    SECTION("MoreFix ON: the tab wraps to the next line's first tab stop")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) } };
        mock.writeToScreen("\033[?7h");   // DECSET DECAWM (autowrap)
        mock.writeToScreen("\033[?41h");  // DECSET MoreFix
        mock.writeToScreen("xxxxxxxxxx"); // fill 10 columns -> cursor at the right margin, wrap pending
        REQUIRE(mock.terminal.currentScreen().cursor().position
                == CellLocation { LineOffset(0), ColumnOffset(9) });
        mock.writeToScreen("\t"); // TAB: honour the pending wrap, then tab from the left margin
        CHECK(mock.terminal.currentScreen().cursor().position
              == CellLocation { LineOffset(1), ColumnOffset(8) });
    }

    SECTION("MoreFix OFF (default): the tab is swallowed at the right margin")
    {
        auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) } };
        mock.writeToScreen("\033[?7h");   // DECSET DECAWM
        mock.writeToScreen("xxxxxxxxxx"); // fill 10 columns
        mock.writeToScreen("\t");         // TAB is swallowed at the right margin
        CHECK(mock.terminal.currentScreen().cursor().position
              == CellLocation { LineOffset(0), ColumnOffset(9) });
        mock.writeToScreen("2"); // only now does the pending wrap execute
        CHECK(mock.terminal.currentScreen().cursor().position.line == LineOffset(1));
    }
}

TEST_CASE("DECINVM: invoke undefined macro", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Invoke non-existent macro 42 — should do nothing, no crash
    mock.writeToScreen("\033[42*z");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).find_first_not_of(' ')
          == std::string::npos);
}

TEST_CASE("DECINVM: nested macro invocation", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 1 that writes "B"
    mock.writeToScreen("\033P1;0;0!zB\033\\");
    mock.terminal.flushInput();
    // Define macro 0 that writes "A", invokes macro 1, then writes "C"
    mock.writeToScreen("\033P0;0;0!zA\033[1*zC\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    // Macro 0 body outputs "A" then "C" (deferred macro 1 runs after), then macro 1 outputs "B"
    auto const text = mock.terminal.currentScreen().grid().lineText(LineOffset(0));
    CHECK(text.contains('A'));
    CHECK(text.contains('B'));
    CHECK(text.contains('C'));
}

TEST_CASE("DECINVM: recursive macro guard", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    // Define macro 0 that invokes itself (infinite recursion attempt)
    mock.writeToScreen("\033P0;0;0!z\033[0*z\033\\");
    mock.terminal.flushInput();
    // Invoke macro 0 — should NOT infinite loop, recursion depth is bounded
    mock.writeToScreen("\033[0*z");
    mock.terminal.flushInput();
    // Verify the terminal is still responsive after bounded recursion
    mock.writeToScreen("OK");
    mock.terminal.flushInput();
    CHECK(mock.terminal.currentScreen().grid().lineText(LineOffset(0)).substr(0, 2) == "OK");
}

TEST_CASE("LNM: set makes LF also return the carriage", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    // Without LNM, a bare LF only moves down: the second line keeps the column.
    mock.writeToScreen("ab\ncd");
    CHECK(mock.terminal.primaryScreen().renderMainPageText() == "ab   \n  cd \n     \n");

    auto other = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    other.writeToScreen("\033[20h"); // LNM set
    other.writeToScreen("ab\ncd");
    CHECK(other.terminal.primaryScreen().renderMainPageText() == "ab   \ncd   \n     \n");
}

TEST_CASE("LNM: set makes the Return key send CR LF", "[screen]")
{
    // LNM has two halves, and vttest exercises both. The output half is above; this is the input
    // half, which lives in the input generator and so must be told about the mode explicitly.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    mock.sendKeyEvent(Key::Enter);
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\r");

    mock.writeToScreen("\033[20h"); // LNM set
    mock.terminal.flushInput();
    mock.mockPty().stdinBuffer().clear();

    mock.sendKeyEvent(Key::Enter);
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\r\n");

    mock.writeToScreen("\033[20l"); // LNM reset
    mock.terminal.flushInput();
    mock.mockPty().stdinBuffer().clear();

    mock.sendKeyEvent(Key::Enter);
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\r");
}

TEST_CASE("KAM: set locks the keyboard out", "[screen]")
{
    // KAM was implemented all along -- Terminal::allowInput() reads it -- but `CSI 2 h` was rejected
    // as unsupported, so nothing could ever turn it on.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    mock.writeToScreen("\033[2h"); // KAM set: keyboard locked
    mock.terminal.flushInput();
    mock.mockPty().stdinBuffer().clear();

    mock.sendCharEvent('x');
    mock.terminal.flushInput();
    CHECK(mock.replyData().empty());

    mock.writeToScreen("\033[2l"); // KAM reset: keyboard unlocked
    mock.terminal.flushInput();

    mock.sendCharEvent('x');
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "x");
}

TEST_CASE("SRM turns local echo on and off", "[screen]")
{
    // SRM is *set* by default -- the host echoes, and the terminal does not. Reset it and the terminal
    // echoes everything it sends, which is what a host with no echo of its own relies on.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    SECTION("set by default, so nothing is echoed")
    {
        CHECK(mock.terminal.isModeEnabled(AnsiMode::SendReceive));

        mock.sendCharSequence("abc");
        CHECK(mock.replyData() == "abc");                        // sent to the host...
        CHECK(screen.grid().lineText(LineOffset(0)) == "     "); // ...and not to the screen
    }

    SECTION("reset, so the terminal echoes what it sends")
    {
        mock.writeToScreen("\033[12l");
        CHECK_FALSE(mock.terminal.isModeEnabled(AnsiMode::SendReceive));

        mock.sendCharSequence("abc");
        CHECK(mock.replyData() == "abc");                        // still sent to the host...
        CHECK(screen.grid().lineText(LineOffset(0)) == "abc  "); // ...and now to the screen as well
    }

    SECTION("set again, and the echo stops")
    {
        mock.writeToScreen("\033[12l");
        mock.writeToScreen("\033[12h");
        CHECK(mock.terminal.isModeEnabled(AnsiMode::SendReceive));

        mock.sendCharSequence("abc");
        CHECK(screen.grid().lineText(LineOffset(0)) == "     ");
    }
}

TEST_CASE("SRM: echoing input that queries keeps the send buffer intact", "[screen]")
{
    // The echo parses the bytes being sent, and a query among them replies -- which appends to the very
    // std::string the bytes are being read from, reallocating it. Sending them afterwards read freed
    // memory. The bytes are owned across the echo now; ASan is what fails this if it regresses.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };

    mock.writeToScreen("\033[12l"); // SRM reset -> local echo
    mock.terminal.sendRawInput("\033[6n");

    // What was typed reached the host, and the DSR the echo executed queued its answer behind it.
    CHECK(mock.replyData() == "\033[6n");
    mock.terminal.flushInput();
    CHECK(mock.replyData() == "\033[6n\033[1;1R");
}

TEST_CASE("SRM: a reply issued from inside the parser is echoed, not deadlocked", "[screen]")
{
    // A reply can be issued from *inside* the parser -- DSR 996 is answered by
    // Screen::reportColorPaletteUpdate(), which flushes on the spot -- and with SRM reset that flush
    // echoes, which parses. Parsing there would re-enter the parser mid-sequence and take the
    // non-recursive state lock the parse already holds. The echo is deferred to a barrier instead.
    // A regression hangs this test rather than failing it.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[12l");   // SRM reset -> local echo
    mock.writeToScreen("\033[?996n"); // DSR: report the color palette, answered from inside the parser

    CHECK(mock.replyData().starts_with("\033[?997;"));

    // And the terminal is still usable afterwards: the deferred echo drained and the parser is at rest.
    mock.writeToScreen("ok");
    CHECK(screen.grid().lineText(LineOffset(0)) == "ok                  ");
}

TEST_CASE("RIS restores SRM", "[screen]")
{
    // hardReset() clears the whole mode register, and a *reset* SRM means local echo is ON -- so RIS
    // leaving it cleared echoed every keystroke on top of the shell's own echo, forever after.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("\033[12l");
    REQUIRE_FALSE(mock.terminal.isModeEnabled(AnsiMode::SendReceive));

    mock.writeToScreen("\033c"); // RIS
    CHECK(mock.terminal.isModeEnabled(AnsiMode::SendReceive));

    mock.sendCharSequence("abc");
    CHECK(screen.grid().lineText(LineOffset(0)) == "     "); // the host echoes again, not us
}

TEST_CASE("DECRQM answers for a mode it knows but can never turn on", "[screen]")
{
    // 0 says "I have never heard of this mode"; 4 says "I know exactly what you mean, and it can never
    // be on here". They are different claims, and Contour used to make the first when it meant the
    // second -- disclaiming knowledge of a dozen modes ECMA-48 defines.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    SECTION("a mode Contour implements reports its live state")
    {
        mock.writeToScreen("\033[4$p"); // IRM
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[4;2$y"));
        mock.discardPendingReplies();

        mock.writeToScreen("\033[4h");
        mock.writeToScreen("\033[4$p");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[4;1$y"));
    }

    SECTION("a mode Contour knows but hard-wires off reports permanently reset")
    {
        mock.writeToScreen("\033[1$p"); // GATM
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[1;4$y"));
        mock.discardPendingReplies();

        mock.writeToScreen("\033[19$p"); // EBM
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[19;4$y"));
    }

    SECTION("a mode Contour has never heard of reports not recognized")
    {
        mock.writeToScreen("\033[123$p");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[123;0$y"));
    }
}

TEST_CASE("DECRQM reports a DEC mode the terminal remembers but cannot act on", "[screen]")
{
    // A terminal that faithfully remembers what it was told is not lying; it would only be lying if it
    // claimed an effect. Contour has no printer, but it can still say whether DECPFF is set.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    mock.writeToScreen("\033[?18$p"); // DECPFF
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?18;2$y"));
    mock.discardPendingReplies();

    mock.writeToScreen("\033[?18h");
    mock.writeToScreen("\033[?18$p");
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?18;1$y"));
    mock.discardPendingReplies();

    mock.writeToScreen("\033[?18l");
    mock.writeToScreen("\033[?18$p");
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?18;2$y"));
}

TEST_CASE("DECRQM answers 4 for a DEC mode that can never turn on", "[screen]")
{
    // DECHCCM (horizontal cursor coupling) is meaningless on a page that never scrolls horizontally, so
    // Contour reports PermanentlyReset (4) rather than Reset (2) -- "I know it, and it cannot be on
    // here". Mirrors esctest test_DECRQM_DEC_DECHCCM.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };

    mock.writeToScreen("\033[?60$p"); // DECRQM DECHCCM
    CHECK(e(mock.terminal.peekInput()) == e("\033[?60;4$y"));
}

TEST_CASE("DECREQTPARM: reports the terminal's communication parameters", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(10) } };

    SECTION("Ps omitted defaults to 0, so Psol is 2")
    {
        mock.writeToScreen("\033[x");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033[2;1;1;128;128;1;0x");
    }

    SECTION("Ps = 0 reports Psol = 2")
    {
        mock.writeToScreen("\033[0x");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033[2;1;1;128;128;1;0x");
    }

    SECTION("Ps = 1 reports Psol = 3")
    {
        mock.writeToScreen("\033[1x");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033[3;1;1;128;128;1;0x");
    }

    SECTION("any other Ps is rejected without a reply")
    {
        mock.writeToScreen("\033[2x");
        mock.terminal.flushInput();
        CHECK(mock.replyData().empty());
    }
}

TEST_CASE("DECSNLS: selects the number of lines per screen", "[screen]")
{
    // DECSNLS used to read its parameter as a *column* count, so `CSI 24 * |` silently narrowed an
    // 80-column page to 24 columns and left the line count alone -- the exact opposite of what the
    // sequence means. esctest found it by crashing the engine: narrowing the page reflows it, which
    // walked the cursor off the bottom.
    auto mock = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("it sets the lines, and leaves the columns alone")
    {
        mock.writeToScreen("\033[24*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(24));
        CHECK(mock.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("an omitted parameter changes nothing")
    {
        mock.writeToScreen("\033[*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(25));
        CHECK(mock.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("a zero parameter changes nothing")
    {
        mock.writeToScreen("\033[0*|");
        CHECK(mock.terminal.pageSize().lines == LineCount(25));
    }

    SECTION("DECRQSS reports back what was selected")
    {
        // esctest asserts exactly this round trip.
        mock.writeToScreen("\033[24*|");
        mock.mockPty().stdinBuffer().clear();
        mock.writeToScreen("\033P$q*|\033\\");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033P1$r24*|\033\\");
    }

    SECTION("the cursor stays inside the page when it shrinks")
    {
        mock.writeToScreen("\033[25;1H"); // the last line of a 25-line page
        REQUIRE(mock.terminal.primaryScreen().cursor().position.line == LineOffset(24));

        mock.writeToScreen("\033[10*|");

        CHECK(mock.terminal.pageSize().lines == LineCount(10));
        CHECK(*mock.terminal.primaryScreen().cursor().position.line
              < *mock.terminal.primaryScreen().pageSize().lines);
    }
}

TEST_CASE("DECSLPP sets the page's length", "[screen]")
{
    // DECSLPP shares its final byte with XTWINOPS; xterm tells them apart by the first parameter, which
    // for DECSLPP is the line count and therefore always 24 or more.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mock.writeToScreen("\033[42t");
    REQUIRE(mock.requestedPageSize.has_value());
    CHECK(*mock.requestedPageSize == PageSize { LineCount(42), ColumnCount(20) });
}

TEST_CASE("DECDSR answers for the devices the terminal does not have", "[screen]")
{
    // A terminal with no printer, no user-defined keys, no macro memory and no session multiplexer still
    // has to *say so*, in the words the standard gives it. Silence is not an answer: an application that
    // asked is waiting, and will read whatever comes next in its place.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    auto const check = [&](std::string_view request, std::string_view expected) {
        INFO("request: " << crispy::escape(request));
        mock.discardPendingReplies();
        mock.writeToScreen(request);
        CHECK(e(mock.terminal.peekInput()) == e(expected));
    };

    check("\033[?15n", "\033[?13n");       // printer port: none
    check("\033[?25n", "\033[?20n");       // user-defined keys: unlocked
    check("\033[?26n", "\033[?27;0;0;5n"); // keyboard
    check("\033[?53n", "\033[?50n");       // locator status: none
    check("\033[?55n", "\033[?50n");       // locator status, xterm's spelling
    check("\033[?56n", "\033[?57;0n");     // locator type: unknown
    check("\033[?62n", "\033[0*{");        // DECMSR: no macro space
    check("\033[?75n", "\033[?70n");       // data integrity: no errors
    check("\033[?85n", "\033[?83n");       // sessions: not configured for multiple
}

TEST_CASE("DECXCPR reports the cursor's position and its page", "[screen]")
{
    // `CSI ? 6 n`. It used to be registered with a final byte of '6' -- which is not a final byte at all
    // -- so the sequence matched nothing and the implementation behind it was unreachable.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mock.writeToScreen("\033[5;6H");
    mock.discardPendingReplies();

    mock.writeToScreen("\033[?6n");
    INFO(mock.terminal.peekInput());
    REQUIRE(e(mock.terminal.peekInput()) == e("\033[?5;6;1R"));
}

TEST_CASE("DECCKSR carries back the id it was asked with", "[screen]")
{
    // There is no macro memory to checksum, so the checksum is zero -- but the reply still has to carry
    // the request's id, which is how an application with several requests in flight tells them apart.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) } };

    mock.writeToScreen("\033[?63;123n");
    INFO(mock.terminal.peekInput());
    REQUIRE(e(mock.terminal.peekInput()) == e("\033P123!~0000\033\\"));
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
