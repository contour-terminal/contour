// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <crispy/Base64.hpp>
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

TEST_CASE("InBandWindowResize", "[screen]")
{
    // DEC mode 2048. The point of it is that an application learns the terminal's size on the same
    // channel it reads everything else on -- SIGWINCH plus an ioctl is unavailable to anything
    // reading over a pipe, a socket or an ssh multiplexer.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(9), Height(18) });

    // The report is asserted where it actually has to ARRIVE -- the PTY -- rather than in the input
    // generator it passes through. The whole point of the mode is an application that cannot receive
    // SIGWINCH, so it is blocked on a read: a report still sitting in the generator has not been
    // delivered, and nothing else is going to happen to flush it. @see reportInBandWindowResize.
    SECTION("reports once on being enabled")
    {
        mock.writeToScreen(std::format("\033[?{}h", toDECModeNum(DECMode::InBandWindowResize)));
        // rows, cols, then pixel height and width -- the opposite order to XTWINOPS.
        CHECK(mock.replyData() == "\033[48;10;20;180;180t");
    }

    SECTION("reports again on every resize")
    {
        mock.writeToScreen(std::format("\033[?{}h", toDECModeNum(DECMode::InBandWindowResize)));
        mock.terminal.resizeScreen(PageSize { LineCount(24), ColumnCount(80) });
        // Both reports reached the PTY: the one from enabling, then the one from the resize. The
        // second arrives from the GUI thread, outside the parser loop that flushes every other reply.
        CHECK(mock.replyData() == "\033[48;10;20;180;180t\033[48;24;80;432;720t");
        CHECK(mock.terminal.peekInput().empty()); // nothing left queued behind it
    }

    SECTION("stays silent while the mode is reset")
    {
        mock.terminal.resizeScreen(PageSize { LineCount(24), ColumnCount(80) });
        CHECK(mock.replyData().empty());
        CHECK(mock.terminal.peekInput().empty());
    }

    SECTION("is reported as a supported, changeable mode")
    {
        auto const modeNum = toDECModeNum(DECMode::InBandWindowResize);
        mock.writeToScreen(std::format("\033[?{}$p", modeNum));
        // Ps=2 is "reset but supported"; ucs-detect reads exactly this to decide the mode exists.
        CHECK(mock.terminal.peekInput() == std::format("\033[?{};2$y", modeNum));
    }
}

TEST_CASE("captureBuffer", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(5) }, LineCount { 5 } };
    auto& screen = mock.terminal.primaryScreen();

    //           [...      history ...  ...][main page area]
    mock.writeToScreen("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    SECTION("lines: 0")
    {
        screen.captureBuffer(LineCount(0), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;\033\\"));
    }
    SECTION("lines: 1")
    {
        screen.captureBuffer(LineCount(1), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;KLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 2")
    {
        screen.captureBuffer(LineCount(2), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;FGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 3")
    {
        screen.captureBuffer(LineCount(3), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput()) == e("\033^314;ABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 4")
    {
        screen.captureBuffer(LineCount(4), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 5")
    {
        screen.captureBuffer(LineCount(5), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;12345\n67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
    SECTION("lines: 5 (+1 overflow)")
    {
        screen.captureBuffer(LineCount(6), false);
        INFO(e(mock.terminal.peekInput()));
        CHECK(e(mock.terminal.peekInput())
              == e("\033^314;12345\n67890\nABCDE\nFGHIJ\nKLMNO\n\033\\\033^314;\033\\"));
    }
}

TEST_CASE("OSC.2.Unicode")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    auto const u32title = u32string_view(U"\U0001F600");
    auto const title = unicode::convert_to<char>(u32title);

    mock.writeToScreen(U"\033]2;\U0001F600\033\\");
    INFO(mock.terminal.peekInput());
    CHECK(e(mock.windowTitle) == e(title));
}

TEST_CASE("OSC.4")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    SECTION("query")
    {
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(e(mock.terminal.peekInput()));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:c0c0/c0c0/c0c0\033\\"));
    }

    SECTION("set color via format rgb:RR/GG/BB")
    {
        mock.writeToScreen("\033]4;7;rgb:ab/cd/ef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("set color via format #RRGGBB")
    {
        mock.writeToScreen("\033]4;7;#abcdef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(e(mock.terminal.peekInput()));
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("set color via format #RGB")
    {
        mock.writeToScreen("\033]4;7;#abc\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:a0a0/b0b0/c0c0\033\\"));
    }

    SECTION("set color via format rgb:RRRR/GGGG/BBBB")
    {
        // The four-digit form is the one Contour itself reports back, and the one applications
        // overwhelmingly send. It used to be rejected outright, leaving the palette untouched.
        mock.writeToScreen("\033]4;7;rgb:abab/cdcd/efef\033\\");
        mock.writeToScreen("\033]4;7;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;7;rgb:abab/cdcd/efef\033\\"));
    }

    SECTION("several index/specification pairs in one sequence")
    {
        mock.writeToScreen("\033]4;0;rgb:f0f0/f0f0/f0f0;1;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]4;0;?;1;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]4;0;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]4;1;rgb:f0f0/0000/0000\033\\"));
    }
}

TEST_CASE("OSC.10-19")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    SECTION("set and query the foreground")
    {
        mock.writeToScreen("\033]10;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]10;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]10;rgb:f0f0/f0f0/f0f0\033\\"));
    }

    SECTION("one sequence walks upward through the colors")
    {
        // OSC 10 with two specifications sets the foreground *and* the background.
        mock.writeToScreen("\033]10;rgb:f0f0/f0f0/f0f0;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]10;?;?\033\\");
        INFO(mock.terminal.peekInput());

        // Each answer is tagged with the OSC command of the color it reports, not with the one the
        // sequence began at. Contour used to read "?;?" as a single color specification, fail to parse
        // it, and answer with nothing at all.
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]10;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]11;rgb:f0f0/0000/0000\033\\"));
    }

    SECTION("a sequence may begin at any color")
    {
        mock.writeToScreen("\033]11;rgb:0101/0202/0303;rgb:0404/0505/0606\033\\");
        mock.writeToScreen("\033]11;?;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]11;rgb:0101/0202/0303\033\\"  // background
                     "\033]12;rgb:0404/0505/0606\033\\") // cursor
        );
    }

    SECTION("an empty specification skips its color")
    {
        mock.writeToScreen("\033]10;rgb:0f0f/0f0f/0f0f\033\\");
        mock.writeToScreen("\033]10;;rgb:f0f0/0000/0000\033\\");
        mock.writeToScreen("\033]10;?;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]10;rgb:0f0f/0f0f/0f0f\033\\"  // untouched
                     "\033]11;rgb:f0f0/0000/0000\033\\") // set by the second specification
        );
    }

    SECTION("a color we do not model still consumes its specification")
    {
        // Specifications 6, 7 and 9 address xterm's Tektronix colors (OSC 15, 16 and 18), which Contour
        // does not model. They must still be consumed, so that the eighth lands on OSC 17 -- the
        // highlight background -- and the tenth on OSC 19 -- the highlight foreground -- rather than
        // shifting onto some earlier color. An eleventh specification runs past OSC 19 and addresses
        // nothing at all.
        mock.writeToScreen("\033]10;rgb:0101/0101/0101;rgb:0202/0202/0202;rgb:0303/0303/0303"
                           ";rgb:0404/0404/0404;rgb:0505/0505/0505;rgb:0606/0606/0606"
                           ";rgb:0707/0707/0707;rgb:0808/0808/0808;rgb:0909/0909/0909"
                           ";rgb:0a0a/0a0a/0a0a;rgb:0b0b/0b0b/0b0b\033\\");

        auto const& palette = mock.terminal.colorPalette();
        CHECK(palette.defaultForeground == RGBColor { 0x01, 0x01, 0x01 });  // OSC 10
        CHECK(palette.defaultBackground == RGBColor { 0x02, 0x02, 0x02 });  // OSC 11
        CHECK(get<RGBColor>(palette.cursor.color) == RGBColor { 3, 3, 3 }); // OSC 12
        CHECK(palette.mouseForeground == RGBColor { 0x04, 0x04, 0x04 });    // OSC 13
        CHECK(palette.mouseBackground == RGBColor { 0x05, 0x05, 0x05 });    // OSC 14
                                                                            // OSC 15, 16: Tektronix
        CHECK(get<RGBColor>(palette.selection.background)                   // OSC 17
              == RGBColor { 0x08, 0x08, 0x08 });                            //
                                                                            // OSC 18: Tektronix
        CHECK(get<RGBColor>(palette.selection.foreground)                   // OSC 19
              == RGBColor { 0x0A, 0x0A, 0x0A });                            //
    }

    SECTION("a malformed specification ends the sequence")
    {
        mock.writeToScreen("\033]10;rgb:0b0b/0b0b/0b0b\033\\");
        mock.resetReplyData();

        // As in xterm, the first specification that cannot be parsed stops the walk, so the background
        // is left alone -- but the foreground set before it stands.
        mock.writeToScreen("\033]10;not-a-color;rgb:0c0c/0c0c/0c0c\033\\");
        mock.writeToScreen("\033]10;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]10;rgb:0b0b/0b0b/0b0b\033\\"));
    }

    SECTION("the highlight colors are addressable in their own right")
    {
        // OSC 17 and OSC 19 could be reached by walking up from OSC 10, but not named directly: only
        // their reset counterparts (OSC 117 and OSC 119) were ever registered, so a highlight color
        // could be reset but never set.
        mock.writeToScreen("\033]17;rgb:1111/2222/3333\033\\");
        mock.writeToScreen("\033]19;rgb:4444/5555/6666\033\\");
        mock.writeToScreen("\033]17;?\033\\");
        mock.writeToScreen("\033]19;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]17;rgb:1111/2222/3333\033\\"
                     "\033]19;rgb:4444/5555/6666\033\\"));
    }

    SECTION("a color following the cell's own color is still reported")
    {
        // The highlight foreground follows the cell's foreground by default rather than naming a color
        // of its own. A query must still be answered -- silence would leave the application reading
        // some later sequence's reply in place of this one.
        mock.writeToScreen("\033]10;rgb:1212/3434/5656\033\\"); // the default foreground
        mock.resetReplyData();

        mock.writeToScreen("\033]19;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]19;rgb:1212/3434/5656\033\\"));
    }

    SECTION("resetting a highlight color restores the configured one")
    {
        mock.writeToScreen("\033]17;rgb:1111/2222/3333\033\\");
        mock.writeToScreen("\033]117\033\\"); // RCOLORHIGHLIGHTBG
        mock.resetReplyData();

        mock.writeToScreen("\033]17;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e(std::format("\033]17;{}\033\\",
                                 colorSpecification(get<RGBColor>(
                                     mock.terminal.defaultColorPalette().selection.background)))));
    }
}

TEST_CASE("XTGETTCAP")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(2) } };

    // Decodes the hex-encoded value from a valid XTGETTCAP response
    // "\033P1+r<hex-name>[=<hex-value>]\033\\"
    auto const extractValue = [](std::string_view reply) -> std::optional<std::string> {
        auto const eq = reply.find('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        auto const st = reply.find("\033\\", eq);
        if (st == std::string_view::npos)
            return std::nullopt;
        return crispy::fromHexString(reply.substr(eq + 1, st - eq - 1));
    };

    auto const queryValue = [&](std::string_view name) -> std::optional<std::string> {
        mock.resetReplyData();
        mock.writeToScreen(std::format("\033P+q{}\033\\", crispy::toHexString(name)));
        auto const reply = std::string(mock.terminal.peekInput());
        INFO(std::format("Reply: {}", crispy::escape(reply)));
        if (!reply.starts_with("\033P1+r"))
            return std::nullopt;
        return extractValue(reply);
    };

    SECTION("string: RGB")
    {
        auto const value = queryValue("RGB");
        REQUIRE(value.has_value());
        CHECK(*value == "8/8/8");
    }

    SECTION("numeric: colors")
    {
        auto const value = queryValue("colors");
        REQUIRE(value.has_value());
        CHECK(*value == "256");
    }

    SECTION("boolean: am")
    {
        auto const value = queryValue("am");
        // Boolean capabilities respond with just the name, no =value.
        CHECK(!value.has_value());
    }

    SECTION("nonexistent")
    {
        mock.resetReplyData();
        mock.writeToScreen(std::format("\033P+q{:02X}{:02X}\033\\", 'x', 'x'));
        // Note how 'xx' is not in the return reply, meaning "not found"
        CHECK(std::string(mock.terminal.peekInput()) == "\033P0+r\033\\");
    }
}

TEST_CASE("XTSMTITLE: hex/UTF-8 title set and query modes", "[screen]")
{
    // Mirrors esctest SMTitleTests. XTSMTITLE (CSI > Ps t) enables and XTRMTITLE (CSI > Ps T) disables
    // the four title-mode features: 0=set-hex, 1=query-hex, 2=set-utf8, 3=query-utf8.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };

    SECTION("SetHex + QueryUTF8: a hex OSC argument decodes; the report stays plain")
    {
        mock.writeToScreen("\033[>2;1T");        // RM_Title(SET_UTF8, QUERY_HEX): disable
        mock.writeToScreen("\033[>0;3t");        // SM_Title(SET_HEX, QUERY_UTF8): enable
        mock.writeToScreen("\033]2;6162\033\\"); // OSC 2: window title = hex "6162" -> "ab"
        mock.terminal.flushInput();
        CHECK(mock.terminal.windowTitle() == "ab");

        mock.resetReplyData();
        mock.writeToScreen("\033[21t"); // report window title
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033]lab\033\\"); // plain UTF-8
    }

    SECTION("SetUTF8 + QueryHex: a plain OSC argument is stored; the report is hex")
    {
        mock.writeToScreen("\033[>0;3T");      // RM_Title(SET_HEX, QUERY_UTF8): disable
        mock.writeToScreen("\033[>2;1t");      // SM_Title(SET_UTF8, QUERY_HEX): enable
        mock.writeToScreen("\033]2;ab\033\\"); // OSC 2: window title = "ab" (plain)
        mock.terminal.flushInput();
        CHECK(mock.terminal.windowTitle() == "ab");

        mock.resetReplyData();
        mock.writeToScreen("\033[21t");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033]l6162\033\\"); // hex
    }

    SECTION("the icon title honours the same modes")
    {
        mock.writeToScreen("\033[>0;1t");        // enable SetHex + QueryHex
        mock.writeToScreen("\033]1;6162\033\\"); // OSC 1: icon title = hex "6162" -> "ab"
        mock.terminal.flushInput();
        CHECK(mock.terminal.iconTitle() == "ab");

        mock.resetReplyData();
        mock.writeToScreen("\033[20t"); // report icon title
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033]L6162\033\\");
    }

    SECTION("XTRMTITLE with no parameter resets every title mode to default")
    {
        mock.writeToScreen("\033[>0;1t"); // enable SetHex + QueryHex
        REQUIRE(mock.terminal.isTitleModeEnabled(TitleModeFeature::SetHex));
        REQUIRE(mock.terminal.isTitleModeEnabled(TitleModeFeature::QueryHex));
        mock.writeToScreen("\033[>T"); // reset all
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.isTitleModeEnabled(TitleModeFeature::SetHex));
        CHECK_FALSE(mock.terminal.isTitleModeEnabled(TitleModeFeature::QueryHex));
    }

    SECTION("RIS resets all title modes (esctest RIS.test_RIS_ResetTitleMode)")
    {
        mock.writeToScreen("\033[>0;1t"); // enable SetHex + QueryHex
        REQUIRE(mock.terminal.isTitleModeEnabled(TitleModeFeature::SetHex));
        mock.writeToScreen("\033c"); // RIS
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.isTitleModeEnabled(TitleModeFeature::SetHex));
        CHECK_FALSE(mock.terminal.isTitleModeEnabled(TitleModeFeature::QueryHex));
    }

    SECTION("XTSMTITLE owns the bare `CSI > Ps t` opcode (single non-zero parameter)")
    {
        // The bare `CSI > Ps t` is now XTSMTITLE, not the relocated XTCAPTURE. A single non-zero mode
        // parameter is honoured; `CSI > 0 t` alone is not exercised because Contour's parser conflates a
        // lone explicit 0 with an omitted parameter (esctest always sends two parameters).
        mock.writeToScreen("\033[>1t"); // enable QueryHex only
        mock.terminal.flushInput();
        CHECK(mock.terminal.isTitleModeEnabled(TitleModeFeature::QueryHex));
        CHECK_FALSE(mock.terminal.isTitleModeEnabled(TitleModeFeature::SetHex));
    }
}

TEST_CASE("OSC 52: clipboard write and gated read", "[screen]")
{
    // esctest ManipulateSelectionDataTests.test_ManipulateSelectionData_default. OSC 52 with base64 data
    // sets the clipboard; OSC 52 with "?" reads it back as `OSC 52 ; s0 ; <base64> ST`. Reading is gated
    // by Settings::allowClipboardRead (MockTerm enables it). base64("testing 123") == "dGVzdGluZyAxMjM=".
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };

    SECTION("write then read round-trips through the clipboard")
    {
        mock.writeToScreen("\033]52;;dGVzdGluZyAxMjM=\033\\"); // set, empty Pc
        mock.terminal.flushInput();
        CHECK(mock.clipboardData == "testing 123");

        mock.resetReplyData();
        mock.writeToScreen("\033]52;;?\033\\"); // read, empty Pc -> reported back as "s0"
        mock.terminal.flushInput();
        CHECK(mock.replyData() == "\033]52;s0;dGVzdGluZyAxMjM=\033\\");
    }

    SECTION("read is silent when the policy forbids it")
    {
        mock.terminal.settings().allowClipboardRead = false;
        mock.writeToScreen("\033]52;;dGVzdGluZyAxMjM=\033\\");
        mock.terminal.flushInput();
        mock.resetReplyData();
        mock.writeToScreen("\033]52;;?\033\\");
        mock.terminal.flushInput();
        CHECK(mock.replyData().empty()); // no reply: the clipboard is not exposed
    }
}

TEST_CASE("OSC 110/111 reset dynamic colors to the default palette", "[screen]")
{
    // The mechanism esctest ResetSpecialColorTests.test_ResetSpecialColor_Dynamic exercises: OSC 110
    // (reset foreground) and OSC 111 (reset background) restore the dynamic color to the terminal's
    // default palette, undoing any OSC 10 / OSC 11 override.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("OSC 110 resets the foreground")
    {
        mock.writeToScreen("\033]10;?\033\\"); // query the default foreground
        mock.terminal.flushInput();
        auto const original = mock.replyData();
        REQUIRE(original.contains("]10;rgb:"));

        mock.resetReplyData();
        mock.writeToScreen("\033]10;#aaaabbbbcccc\033\\"); // override it
        mock.writeToScreen("\033]10;?\033\\");
        mock.terminal.flushInput();
        REQUIRE(mock.replyData() == "\033]10;rgb:aaaa/bbbb/cccc\033\\");

        mock.resetReplyData();
        mock.writeToScreen("\033]110\033\\");  // reset foreground to default
        mock.writeToScreen("\033]10;?\033\\"); // query again
        mock.terminal.flushInput();
        CHECK(mock.replyData() == original);
    }

    SECTION("OSC 111 resets the background")
    {
        mock.writeToScreen("\033]11;?\033\\");
        mock.terminal.flushInput();
        auto const original = mock.replyData();
        REQUIRE(original.contains("]11;rgb:"));

        mock.resetReplyData();
        mock.writeToScreen("\033]11;#112233445566\033\\");
        mock.writeToScreen("\033]111\033\\"); // reset background to default
        mock.writeToScreen("\033]11;?\033\\");
        mock.terminal.flushInput();
        CHECK(mock.replyData() == original);
    }
}

TEST_CASE("XTPUSHTITLE and XTPOPTITLE share one stack of optional pairs", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };

    SECTION("push both, pop only the icon's")
    {
        mock.writeToScreen("\033]0;first\033\\"); // both titles
        mock.writeToScreen("\033[22;0t");         // push both
        mock.writeToScreen("\033]0;x\033\\");     // both titles again

        mock.writeToScreen("\033[23;1t"); // pop the icon's alone
        CHECK(mock.iconTitle == "first");
        CHECK(mock.windowTitle == "x"); // the window's is left where the application put it

        // The pop took the one entry off the stack, whatever it held -- so there is nothing left for a
        // pop of the window's to restore. Two independent stacks would wrongly restore "first" here.
        mock.writeToScreen("\033[23;2t");
        CHECK(mock.windowTitle == "x");
    }

    SECTION("push both, pop only the window's")
    {
        mock.writeToScreen("\033]0;first\033\\");
        mock.writeToScreen("\033[22;0t");
        mock.writeToScreen("\033]0;x\033\\");

        mock.writeToScreen("\033[23;2t"); // pop the window's alone
        CHECK(mock.iconTitle == "x");
        CHECK(mock.windowTitle == "first");

        mock.writeToScreen("\033[23;1t"); // nothing left on the stack
        CHECK(mock.iconTitle == "x");
    }

    SECTION("push both, pop both")
    {
        mock.writeToScreen("\033]0;first\033\\");
        mock.writeToScreen("\033[22;0t");
        mock.writeToScreen("\033]0;x\033\\");

        mock.writeToScreen("\033[23;0t");
        CHECK(mock.iconTitle == "first");
        CHECK(mock.windowTitle == "first");
    }

    SECTION("push the icon's, push the window's, pop both")
    {
        // The popped entry carries only the window's title, so the icon's is looked for further down the
        // stack -- and found in the entry below. Both are restored.
        mock.writeToScreen("\033]2;win\033\\");
        mock.writeToScreen("\033]1;ico\033\\");
        mock.writeToScreen("\033[22;1t"); // push the icon's alone
        mock.writeToScreen("\033[22;2t"); // push the window's alone

        mock.writeToScreen("\033]2;y\033\\");
        mock.writeToScreen("\033]1;z\033\\");

        mock.writeToScreen("\033[23;0t"); // pop both
        CHECK(mock.iconTitle == "ico");
        CHECK(mock.windowTitle == "win");
    }

    SECTION("a stack, not a single slot")
    {
        mock.writeToScreen("\033]1;a\033\\");
        mock.writeToScreen("\033[22;1t");
        mock.writeToScreen("\033]1;b\033\\");
        mock.writeToScreen("\033[22;1t");
        mock.writeToScreen("\033]1;z\033\\");

        mock.writeToScreen("\033[23;1t");
        CHECK(mock.iconTitle == "b"); // last in, first out
        mock.writeToScreen("\033[23;1t");
        CHECK(mock.iconTitle == "a");
    }

    SECTION("popping an empty stack leaves the titles alone")
    {
        mock.writeToScreen("\033]0;kept\033\\");
        mock.writeToScreen("\033[23;0t");
        CHECK(mock.iconTitle == "kept");
        CHECK(mock.windowTitle == "kept");
    }

    SECTION("the stack is bounded")
    {
        // An unbounded stack is a memory-growth lever for anything that can write to the terminal.
        for (auto const i: std::views::iota(0u, 20u))
        {
            mock.writeToScreen(std::format("\033]1;t{}\033\\", i));
            mock.writeToScreen("\033[22;1t");
        }

        // Only the last MaxSavedTitles pushes survive; the oldest were discarded.
        mock.writeToScreen("\033[23;1t");
        CHECK(mock.iconTitle == "t19");
    }
}

TEST_CASE("XTWINOPS reads its operation from the first parameter", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    SECTION("a resize with two parameters is still a resize")
    {
        // `CSI 4 ; h ; w t` carries three parameters and `CSI 8 ; h t` two. Dispatching on the
        // parameter count -- as this used to -- read the latter as "resize to the display's size".
        mock.writeToScreen("\033[8;5;12t");
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(5), ColumnCount(12) });
    }

    SECTION("an omitted dimension keeps the current one")
    {
        // "Omitted parameters reuse the current height or width." -- xterm's ctlseqs.
        mock.writeToScreen("\033[8;;7t"); // set the columns, keep the lines
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(10), ColumnCount(7) });
    }

    SECTION("an omitted width keeps the current one")
    {
        mock.writeToScreen("\033[8;3t"); // set the lines, keep the columns
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(3), ColumnCount(20) });
    }
}

TEST_CASE("XTWINOPS reports the window's place on the screen", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(8), Height(16) });

    SECTION("the window's position is what the frontend last reported")
    {
        mock.terminal.setWindowState(
            WindowState { .position = WindowPosition { .x = 40, .y = 25 }, .iconified = false });
        mock.writeToScreen("\033[13t");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[3;40;25t"));
    }

    SECTION("a move asks the frontend, and the frontend reports back")
    {
        mock.writeToScreen("\033[3;12;34t");
        mock.discardPendingReplies();
        mock.writeToScreen("\033[13t");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[3;12;34t"));
    }

    SECTION("iconified or not")
    {
        mock.writeToScreen("\033[11t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[1t")); // not iconified
        mock.discardPendingReplies();

        mock.writeToScreen("\033[2t"); // iconify
        mock.writeToScreen("\033[11t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[2t"));
        mock.discardPendingReplies();

        mock.writeToScreen("\033[1t"); // de-iconify
        mock.writeToScreen("\033[11t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[1t"));
    }

    SECTION("the screen's size in pixels and in characters")
    {
        mock.terminal.setWindowState(
            WindowState { .screenPixelSize = ImageSize { Width(800), Height(480) } });

        mock.writeToScreen("\033[15t"); // the screen, in pixels
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[5;480;800t"));
        mock.discardPendingReplies();

        // 800/8 = 100 columns, 480/16 = 30 lines. Reporting the *page* size here -- as this used to --
        // tells every application that the window is already as large as the screen.
        mock.writeToScreen("\033[19t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[9;30;100t"));
        mock.discardPendingReplies();

        // The text area is still the page.
        mock.writeToScreen("\033[18t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[8;10;20t"));
    }

    SECTION("a terminal with no screen is exactly as large as the one it does not have")
    {
        // The frontend reported no screen, so the window's own size stands in for it -- an honest
        // answer, and one that keeps a resize-to-the-display meaningful.
        mock.writeToScreen("\033[15t");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033[5;160;160t")); // 10*16 by 20*8
    }
}

TEST_CASE("XTWINOPS resizes to the display when a dimension is zero", "[screen]")
{
    // "Omitted parameters reuse the current height or width. Zero parameters use the display's height
    // or width." -- xterm's ctlseqs. A dimension has three readings, not two.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(8), Height(16) });
    mock.terminal.setWindowState(WindowState { .screenPixelSize = ImageSize { Width(800), Height(480) } });

    SECTION("zero lines means the display's height")
    {
        mock.writeToScreen("\033[8;0;12t");
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(30), ColumnCount(12) });
    }

    SECTION("zero columns means the display's width")
    {
        mock.writeToScreen("\033[8;5;0t");
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(5), ColumnCount(100) });
    }

    SECTION("an omitted dimension still keeps the current one")
    {
        mock.writeToScreen("\033[8;;12t");
        REQUIRE(mock.requestedPageSize.has_value());
        CHECK(*mock.requestedPageSize == PageSize { LineCount(10), ColumnCount(12) });
    }
}

TEST_CASE("OSC.5 addresses the special colors", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };

    SECTION("set and query")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\"); // bold
        mock.writeToScreen("\033]5;4;rgb:1010/2020/3030\033\\"); // italic
        mock.discardPendingReplies();

        mock.writeToScreen("\033]5;0;?\033\\");
        mock.writeToScreen("\033]5;4;?\033\\");
        INFO(mock.terminal.peekInput());
        REQUIRE(e(mock.terminal.peekInput())
                == e("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\"
                     "\033]5;4;rgb:1010/2020/3030\033\\"));

        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == RGBColor { 0xF0, 0xF0, 0xF0 });
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Italic)
              == RGBColor { 0x10, 0x20, 0x30 });
    }

    SECTION("OSC 4 reaches the same colors, just past the indexed ones")
    {
        // An application may name a special color either way: `OSC 5 ; 0` and `OSC 4 ; 256` are the same
        // color. A report echoes the index it was given, in the form it was given.
        mock.writeToScreen("\033]4;256;rgb:aaaa/bbbb/cccc\033\\");
        mock.discardPendingReplies();

        mock.writeToScreen("\033]5;0;?\033\\");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]5;0;rgb:aaaa/bbbb/cccc\033\\"));
        mock.discardPendingReplies();

        mock.writeToScreen("\033]4;256;?\033\\");
        REQUIRE(e(mock.terminal.peekInput()) == e("\033]4;256;rgb:aaaa/bbbb/cccc\033\\"));
    }

    SECTION("an index past the last special color names nothing")
    {
        mock.writeToScreen("\033]5;5;rgb:0000/0000/0000\033\\");
        CHECK(mock.terminal.peekInput().empty());
    }

    SECTION("the dim colors are not reachable, and are not overwritten")
    {
        // Contour keeps its own dim colors where xterm keeps its special ones. Naming special color 0
        // must not land on a dim color.
        auto const dimBefore = mock.terminal.colorPalette().dimColor(0);
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        CHECK(mock.terminal.colorPalette().dimColor(0) == dimBefore);
    }
}

TEST_CASE("OSC.105 resets the special colors", "[screen]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    auto const original = mock.terminal.defaultColorPalette().specialColor(SpecialColor::Bold);

    SECTION("one color")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]105;0\033\\");
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
    }

    SECTION("all of them, when no index is given")
    {
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]5;4;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]105\033\\");
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Italic)
              == mock.terminal.defaultColorPalette().specialColor(SpecialColor::Italic));
    }

    SECTION("OSC 104 with no index resets every index it can address")
    {
        mock.writeToScreen("\033]4;3;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]5;0;rgb:f0f0/f0f0/f0f0\033\\");
        mock.writeToScreen("\033]104\033\\");
        CHECK(mock.terminal.colorPalette().palette[3] == mock.terminal.defaultColorPalette().palette[3]);
        // OSC 4 addresses the special colors too (256..260), so a bare OSC 104 reaches them -- as xterm
        // walks its whole Acolors.
        CHECK(mock.terminal.colorPalette().specialColor(SpecialColor::Bold) == original);
    }

    SECTION("OSC 104 does not reset the dynamic colors")
    {
        // The dynamic colors share the ColorPalette but are addressed by OSC 10..19 and reset by
        // OSC 110..119 -- xterm keeps them in a separate Tcolors for exactly this reason. Assigning the
        // whole palette here withdrew a background the application set with OSC 11 and nothing asked to
        // reset: a themed shell lost its background to any stray `tput oc`.
        mock.writeToScreen("\033]11;rgb:1e1e/1e1e/2e2e\033\\");
        auto const chosenBackground = mock.terminal.colorPalette().defaultBackground;
        REQUIRE(chosenBackground != mock.terminal.defaultColorPalette().defaultBackground);

        mock.writeToScreen("\033]104\033\\");

        CHECK(mock.terminal.colorPalette().defaultBackground == chosenBackground);
    }
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)

// {{{ OSC 533 -- screenshot

namespace
{
/// Decodes the base64 payloads of the `PM 533` data messages in @p reply and concatenates them.
///
/// @param reply Everything the terminal wrote back.
/// @return The screenshot the reply carries.
[[nodiscard]] std::string decodeScreenshot(std::string_view reply)
{
    auto content = std::string {};
    auto rest = reply;
    while (true)
    {
        auto const start = rest.find("\033^533;");
        if (start == std::string_view::npos)
            break;
        auto const end = rest.find("\033\\", start);
        REQUIRE(end != std::string_view::npos);
        // Skip the whole "ESC ^ 533 ;" introducer, leaving Pid;Ps;Pt;Pl;Pb;Pr;Pf;Pw;Ph;<base64>.
        auto const message = rest.substr(start + 6, end - start - 6);
        rest = rest.substr(end + 2);

        // Pid;Ps;... -- a data message is the only one with a payload after the pixel extent.
        auto const fields = crispy::split(message, ';');
        if (fields.size() < 10)
            continue;
        content += crispy::base64::decode(fields[9]);
    }
    return content;
}
} // namespace

TEST_CASE("screenshot.OSC533", "[screen][screenshot]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    mock.writeToScreen("ABCDE\r\nfghij\r\n12345");
    mock.discardPendingReplies();

    SECTION("no parameters captures the whole main page")
    {
        mock.writeToScreen("\033]533\033\\");
        auto const reply = mock.terminal.peekInput();
        // A PM reply, never an OSC -- replaying it into a terminal must not start a new screenshot.
        CHECK(reply.starts_with("\033^533;0;1;1;1;3;5;0;0;0;"));
        CHECK(!reply.contains("\033]533"));
        CHECK(decodeScreenshot(reply) == "ABCDE\nfghij\n12345\n");
        // ... and it ends with the end-of-data message.
        CHECK(reply.ends_with("\033^533;0;0\033\\"));
    }

    SECTION("an explicit region captures exactly that rectangle")
    {
        // Rows 1..2, columns 2..4 -- one-based and inclusive on every edge.
        mock.writeToScreen("\033]533;0;1;2;2;4\033\\");
        CHECK(decodeScreenshot(mock.terminal.peekInput()) == "BCD\nghi\n");
    }

    SECTION("the request id is echoed in every reply message")
    {
        mock.writeToScreen("\033]533;99;1;1;1;5\033\\");
        auto const reply = mock.terminal.peekInput();
        CHECK(reply.starts_with("\033^533;99;1;"));
        CHECK(reply.ends_with("\033^533;99;0\033\\"));
    }

    SECTION("the region is clamped to the page")
    {
        mock.writeToScreen("\033]533;0;1;1;999;999\033\\");
        auto const reply = mock.terminal.peekInput();
        CHECK(reply.starts_with("\033^533;0;1;1;1;3;5;0;0;0;"));
        CHECK(decodeScreenshot(reply) == "ABCDE\nfghij\n12345\n");
    }

    SECTION("the VT-sequence format carries SGR and CRLF")
    {
        // Repaint the top row in red, so there is a rendition for the format to carry. Plain text
        // with default attributes would legitimately produce no SGR at all.
        mock.writeToScreen("\033[H\033[31mABCDE\033[m");
        mock.discardPendingReplies();

        mock.writeToScreen("\033]533;0;1;1;1;5;1\033\\");
        auto const reply = mock.terminal.peekInput();
        CHECK(reply.starts_with("\033^533;0;1;1;1;1;5;1;0;0;"));
        auto const content = decodeScreenshot(reply);
        CHECK(content.ends_with("\r\n"));
        CHECK(content.contains("ABCDE"));
        // The ESC bytes live inside the base64 payload, never raw on the wire -- which is the whole
        // reason the payload is encoded.
        CHECK(content.contains('\033'));
        CHECK(!reply.contains("\033[31m"));
    }

    SECTION("an inverted region is refused rather than answered with nothing")
    {
        mock.writeToScreen("\033]533;5;3;1;1;5\033\\");
        CHECK(mock.terminal.peekInput() == "\033^533;5;5\033\\");
    }

    SECTION("a reserved format is refused")
    {
        // Raw RGBA: the number is spoken for so nothing else can take it, but the extension
        // deliberately does not carry pixels that way. @see screenshot::Format::Rgba.
        mock.writeToScreen("\033]533;6;1;1;3;5;4\033\\");
        CHECK(mock.terminal.peekInput() == "\033^533;6;4\033\\");
    }

    SECTION("a renderer format with no renderer behind it is unavailable, not denied")
    {
        // PNG is implemented, but only where there is something rasterizing glyphs. A MockTerm is a
        // headless session: nothing refused the read, there is simply no picture to take.
        mock.writeToScreen("\033]533;8;1;1;3;5;3\033\\");
        CHECK(mock.terminal.peekInput() == "\033^533;8;6\033\\");
    }

    SECTION("a malformed request is still answered")
    {
        mock.writeToScreen("\033]533;7;1;1;nope;5\033\\");
        CHECK(mock.terminal.peekInput() == "\033^533;7;3\033\\");
    }
}

TEST_CASE("screenshot.captureScreenshot reads the grid directly", "[screen][screenshot]")
{
    // The grid half of OSC 533 returns its bytes rather than replying with them, so it is checkable
    // as a function of the grid -- no sequence to write, no reply queue to decode, no PTY. The tests
    // above still drive the whole sequence; this one pins what the region actually reads.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    mock.writeToScreen("ABCDE\r\nfghij\r\n12345");

    auto const wholePage = screenshot::Request {
        .id = 0,
        .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(2), .right = Right(4) },
        .format = screenshot::Format::PlainText,
    };

    SECTION("plain text is LF-terminated, one line per row, and carries no pixel extent")
    {
        auto const capture = mock.terminal.primaryScreen().captureScreenshot(wholePage);
        CHECK(capture.content == "ABCDE\nfghij\n12345\n");
        CHECK(capture.pixelSize == ImageSize {});
    }

    SECTION("a sub-rectangle reads exactly those cells")
    {
        auto request = wholePage;
        request.area = Rect { .top = Top(0), .left = Left(1), .bottom = Bottom(1), .right = Right(3) };
        CHECK(mock.terminal.primaryScreen().captureScreenshot(request).content == "BCD\nghi\n");
    }

    SECTION("the VT-sequence format ends each row with CRLF")
    {
        // A bare LF would leave every row starting where the one above it ended, once written back.
        auto request = wholePage;
        request.format = screenshot::Format::VTSequences;
        auto const capture = mock.terminal.primaryScreen().captureScreenshot(request);
        CHECK(capture.content.ends_with("\r\n"));
        CHECK(capture.content.contains("ABCDE"));
        CHECK(capture.pixelSize == ImageSize {});
    }
}

TEST_CASE("screenshot.OSC533.chunking", "[screen][screenshot]")
{
    // A page whose screenshot comfortably exceeds one chunk: 60 rows of 80 columns plus a newline
    // each is 4860 bytes, against a 4095-byte chunk.
    auto mock = MockTerm { PageSize { LineCount(60), ColumnCount(80) } };
    for (auto const row: std::views::iota(0, 60))
    {
        mock.writeToScreen(std::string(80, static_cast<char>('a' + (row % 26))));
        if (row != 59)
            mock.writeToScreen("\r\n");
    }
    mock.discardPendingReplies();

    mock.writeToScreen("\033]533;1\033\\");
    auto const reply = mock.terminal.peekInput();

    // More than one data message, and an end-of-data message closing the sequence.
    auto const dataMessages = [&] {
        auto count = size_t { 0 };
        auto rest = std::string_view { reply };
        while (true)
        {
            auto const at = rest.find("\033^533;1;1;");
            if (at == std::string_view::npos)
                return count;
            ++count;
            rest = rest.substr(at + 1);
        }
    }();
    CHECK(dataMessages > 1);
    CHECK(reply.ends_with("\033^533;1;0\033\\"));

    // Every row survives reassembly, in order and unabridged.
    auto expected = std::string {};
    for (auto const row: std::views::iota(0, 60))
    {
        expected += std::string(80, static_cast<char>('a' + (row % 26)));
        expected += '\n';
    }
    CHECK(decodeScreenshot(reply) == expected);
}

TEST_CASE("screenshot.OSC533.denied", "[screen][screenshot]")
{
    // What the frontend does when the user says no, or the configuration already did. A refusal is
    // still a reply: an application that wrote a request and is reading must not be left hanging.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    mock.writeToScreen("abcd\r\nefgh");
    mock.discardPendingReplies();

    auto const request = screenshot::Request {
        .id = 8,
        .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(1), .right = Right(3) },
        .format = screenshot::Format::PlainText
    };
    mock.terminal.answerScreenshot(request, screenshot::Decision::Denied);
    CHECK(mock.terminal.peekInput() == "\033^533;8;2\033\\");

    // ... and the same request allowed does hand the screen over.
    mock.terminal.answerScreenshot(request, screenshot::Decision::Allowed);
    CHECK(decodeScreenshot(mock.terminal.peekInput()) == "abcd\nefgh\n");
}

// }}}
