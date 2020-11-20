/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Screen.h>
#include <catch2/catch.hpp>
#include <string_view>

using namespace terminal;
using namespace std;

namespace
{
    void logScreenText(Screen const& screen, string const& headline = "")
    {
        if (headline.empty())
            UNSCOPED_INFO("dump:");
        else
            UNSCOPED_INFO(headline + ":");

        for (int row = 1; row <= screen.size().height; ++row)
            UNSCOPED_INFO(fmt::format("[{}] \"{}\"", row, screen.renderTextLine(row)));
    }

    class MockScreen : public MockScreenEvents,
                       public Screen {
      public:
        explicit MockScreen(Size const& _size) :
            Screen{
                _size,
                *this,
                [this](LogEvent const& _logEvent) { log(_logEvent); }
            }
        {
        }

        void log(LogEvent _logEvent)
        {
            INFO(fmt::format("{}", _logEvent));
        }
    };
}

TEST_CASE("Screen.isLineVisible", "[screen]")
{
    auto screen = MockScreen{Size{2, 1}};
    auto viewport = terminal::Viewport{screen};

    screen.write("10203040");
    REQUIRE("40" == screen.renderTextLine(1));
    REQUIRE("30" == screen.renderTextLine(0));
    REQUIRE("20" == screen.renderTextLine(-1));
    REQUIRE("10" == screen.renderTextLine(-2));

    CHECK(viewport.isLineVisible(1));
    CHECK_FALSE(viewport.isLineVisible(0));
    CHECK_FALSE(viewport.isLineVisible(-1));
    CHECK_FALSE(viewport.isLineVisible(-2));
    CHECK_FALSE(viewport.isLineVisible(-3)); // minimal out-of-bounds

    viewport.scrollUp(1);
    CHECK_FALSE(viewport.isLineVisible(1));
    CHECK(viewport.isLineVisible(0));
    CHECK_FALSE(viewport.isLineVisible(-1));
    CHECK_FALSE(viewport.isLineVisible(-2));

    viewport.scrollUp(1);
    CHECK_FALSE(viewport.isLineVisible(1));
    CHECK_FALSE(viewport.isLineVisible(0));
    CHECK(viewport.isLineVisible(-1));
    CHECK_FALSE(viewport.isLineVisible(-2));

    viewport.scrollUp(1);
    CHECK_FALSE(viewport.isLineVisible(1));
    CHECK_FALSE(viewport.isLineVisible(0));
    CHECK_FALSE(viewport.isLineVisible(-1));
    CHECK(viewport.isLineVisible(-2));
}

TEST_CASE("AppendChar", "[screen]")
{
    auto screen = MockScreen{{3, 1}};
    REQUIRE("   " == screen.renderTextLine(1));

    screen.setMode(Mode::AutoWrap, false);

    screen.write("A");
    REQUIRE("A  " == screen.renderTextLine(1));

    screen.write("B");
    REQUIRE("AB " == screen.renderTextLine(1));

    screen.write("C");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.write("D");
    REQUIRE("ABD" == screen.renderTextLine(1));

    screen.setMode(Mode::AutoWrap, true);
    screen.write("EF");
    REQUIRE("F  " == screen.renderTextLine(1));
}

TEST_CASE("AppendChar_CR_LF", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    REQUIRE("   " == screen.renderTextLine(1));

    screen.setMode(Mode::AutoWrap, false);

    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.write("\r");
    REQUIRE("ABC\n   \n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    screen.write("\n");
    REQUIRE("ABC\n   \n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
}

TEST_CASE("AppendChar.emoji_exclamationmark", "[screen]")
{
    auto screen = MockScreen{{5, 1}};

    screen.setBackgroundColor(IndexedColor::Blue);

    screen.write(U"\u2757"); // ❗
    // screen.write(U"\uFE0F");
    CHECK(screen.at({1, 1}).attributes().backgroundColor == IndexedColor::Blue);
    CHECK(screen.at({1, 1}).width() == 2);
    CHECK(screen.at({1, 2}).attributes().backgroundColor == IndexedColor::Blue);
    CHECK(screen.at({1, 2}).width() == 1);

    screen.write(U"M");
    CHECK(screen.at({1, 3}).attributes().backgroundColor == IndexedColor::Blue);
}

TEST_CASE("AppendChar.emoji_VS16_fixed_width", "[screen]")
{
    auto screen = MockScreen{{5, 1}};

    // print letter-like symbol `i` with forced emoji presentation style.
    screen.write(U"\u2139");
    screen.write(U"\uFE0F");
    screen.write(U"X");

    // double-width emoji with VS16
    auto const& c1 = screen.at({1, 1});
    CHECK(c1.codepoints() == U"\u2139\uFE0F");
    CHECK(c1.width() == 1); // XXX by default: do not change width (TODO: create test for optionally changing width by configuration)

    // character after the emoji
    auto const& c2 = screen.at({1, 2});
    CHECK(c2.codepoints() == U"X");
    CHECK(c2.width() == 1);

    // character after the emoji
    auto const& c3 = screen.at({1, 3});
    CHECK(c3.codepointCount() == 0);
}

#if 0
TEST_CASE("AppendChar.emoji_VS16_with_changing_width", "[screen]") // TODO
{
    auto screen = MockScreen{{5, 1}};

    // print letter-like symbol `i` with forced emoji presentation style.
    screen.write(U"\u2139");
    screen.write(U"\uFE0F");
    screen.write(U"X");

    // double-width emoji with VS16
    auto const& c1 = screen.write(1, 1);
    CHECK(c1.codepoints() == U"\u2139\uFE0F");
    CHECK(c1.width() == 2);

    // unused cell
    auto const& c2 = screen.write(1, 2);
    CHECK(c2.codepointCount() == 0);
    CHECK(c2.width() == 1);

    // character after the emoji
    auto const& c3 = screen.write(1, 3);
    CHECK(c3.codepoints() == U"X");
    CHECK(c3.width() == 1);
}
#endif

TEST_CASE("AppendChar.emoji_family", "[screen]")
{
    auto screen = MockScreen{{5, 1}};

    // print letter-like symbol `i` with forced emoji presentation style.
    screen.write(U"\U0001F468");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    screen.write(U"\u200D");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    screen.write(U"\U0001F468");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    screen.write(U"\u200D");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    screen.write(U"\U0001F467");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    screen.write(U"X");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 4});

    // double-width emoji with VS16
    auto const& c1 = screen.at({1, 1});
    CHECK(c1.codepoints() == U"\U0001F468\u200D\U0001F468\u200D\U0001F467");
    CHECK(c1.width() == 2);

    // unused cell
    auto const& c2 = screen.at({1, 2});
    CHECK(c2.codepointCount() == 0);
    CHECK(c2.width() == 1);

    // character after the emoji
    auto const& c3 = screen.at({1, 3});
    CHECK(c3.codepoints() == U"X");
    CHECK(c3.width() == 1);
}

TEST_CASE("AppendChar.emoji_zwj1", "[screen]")
{
    auto screen = MockScreen{{5, 1}};

    screen.setMode(Mode::AutoWrap, false);

    // https://emojipedia.org/man-facepalming-medium-light-skin-tone/
    auto const emoji = u32string_view{U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F"};
    screen.write(emoji);
    // TODO: provide native UTF-32 write function (not emulated through UTF-8 -> UTF-32...)

    auto const& c1 = screen.at({1, 1});
    CHECK(c1.codepoints() == emoji);
    CHECK(c1.width() == 2);

    // other columns remain untouched
    CHECK(screen.at({1, 2}).codepointCount() == 0);
    CHECK(screen.at({1, 3}).codepointCount() == 0);
    CHECK(screen.at({1, 4}).codepointCount() == 0);
    CHECK(screen.at({1, 5}).codepointCount() == 0);

    CHECK(U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F    " == unicode::from_utf8(screen.renderTextLine(1)));
}

TEST_CASE("AppendChar.emoji_1", "[screen]")
{
    auto screen = MockScreen{{3, 1}};

    screen.write(U"\U0001F600");

    auto const& c1 = screen.at({1, 1});
    CHECK(c1.codepoints() == U"\U0001F600");
    CHECK(c1.width() == 2);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    CHECK(screen.at({1, 2}).codepointCount() == 0);
    CHECK(screen.at({1, 3}).codepointCount() == 0);

    screen.write("B");
    auto const& c2 = screen.at({1, 2});
    CHECK(c2.codepointCount() == 0);
    CHECK(c2.codepoint(0) == 0);
    CHECK(c2.width() == 1);

    auto const& c3 = screen.at({1, 3});
    CHECK(c3.codepointCount() == 1);
    CHECK(c3.codepoint(0) == 'B');
    CHECK(c3.codepoint(1) == 0);
    CHECK(c3.width() == 1);
}

TEST_CASE("AppendChar_WideChar", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    screen.setMode(Mode::AutoWrap, true);
    screen.write(U"\U0001F600");
    CHECK(screen.cursorPosition() == Coordinate{1, 3});
}

TEST_CASE("AppendChar_AutoWrap", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    screen.setMode(Mode::AutoWrap, true);

    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("   " == screen.renderTextLine(2));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.write("D");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("D  " == screen.renderTextLine(2));

    screen.write("EF");
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));

    screen.write("G");
    REQUIRE("DEF" == screen.renderTextLine(1));
    REQUIRE("G  " == screen.renderTextLine(2));
}

TEST_CASE("AppendChar_AutoWrap_LF", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    screen.setMode(Mode::AutoWrap, true);

    INFO("write ABC");
    screen.write("ABC");
    logScreenText(screen);
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("   " == screen.renderTextLine(2));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    INFO("write CRLF");
    screen.write("\r\n");
    logScreenText(screen, "after writing LF");
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});

    INFO("write 'D'");
    screen.write("D");
    logScreenText(screen);
    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("D  " == screen.renderTextLine(2));
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
}

TEST_CASE("Backspace", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    screen.write("12");
    CHECK("12 " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
}

TEST_CASE("Linefeed", "[screen]")
{
    auto screen = MockScreen{{2, 2}};
    SECTION("with scroll-up") {
        INFO("init:");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        screen.write("1\r\n2");

        INFO("after writing '1\\n2':");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        REQUIRE("1 " == screen.renderTextLine(1));
        REQUIRE("2 " == screen.renderTextLine(2));

        screen.write("\r\n3"); // line 3

        INFO("After writing '\\n3':");
        INFO(fmt::format("  line 1: '{}'", screen.renderTextLine(1)));
        INFO(fmt::format("  line 2: '{}'", screen.renderTextLine(2)));

        REQUIRE("2 " == screen.renderTextLine(1));
        REQUIRE("3 " == screen.renderTextLine(2));
    }
}

TEST_CASE("ClearToEndOfScreen", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    screen.write("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));
    REQUIRE("GHI" == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

    screen.moveCursorTo({2, 2});
    screen.clearToEndOfScreen();

    CHECK("ABC" == screen.renderTextLine(1));
    CHECK("D  " == screen.renderTextLine(2));
    CHECK("   " == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
}

TEST_CASE("ClearToBeginOfScreen", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    screen.write("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));
    REQUIRE("GHI" == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

    screen.moveCursorTo({2, 2});
    screen.clearToBeginOfScreen();

    CHECK("   " == screen.renderTextLine(1));
    CHECK("  F" == screen.renderTextLine(2));
    CHECK("GHI" == screen.renderTextLine(3));
    CHECK(screen.cursorPosition() == Coordinate{2, 2});
}

TEST_CASE("ClearScreen", "[screen]")
{
    auto screen = MockScreen{{2, 2}};
    screen.write("AB\r\nC");
    screen.clearScreen();
    CHECK("  " == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
}

TEST_CASE("ClearToEndOfLine", "[screen]")
{
    auto screen = MockScreen{{3, 1}};
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.moveCursorToColumn(2);
    screen.clearToEndOfLine();
    CHECK("A  " == screen.renderTextLine(1));
}

TEST_CASE("ClearToBeginOfLine", "[screen]")
{
    auto screen = MockScreen{{3, 1}};
    screen.setMode(Mode::AutoWrap, false);
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.moveCursorToColumn(2);
    screen.clearToBeginOfLine();
    CHECK("  C" == screen.renderTextLine(1));
}

TEST_CASE("ClearLine", "[screen]")
{
    auto screen = MockScreen{{3, 1}};
    screen.setMode(Mode::AutoWrap, false);
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.clearLine();
    CHECK("   " == screen.renderTextLine(1));
}

TEST_CASE("InsertColumns", "[screen]")
{
    // "DECIC has no effect outside the scrolling margins."
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    screen.setMode(Mode::LeftRightMargin, true);
    screen.setLeftRightMargin(2, 4);
    screen.setTopBottomMargin(2, 4);

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("outside margins: top left") {
        screen.moveCursorTo({1, 1});
        screen.insertColumns(1);
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("outside margins: bottom right") {
        screen.moveCursorTo({5, 5});
        screen.insertColumns(1);
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("inside margins") {
        screen.moveCursorTo({2, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

        SECTION("DECIC-0") {
            screen.insertColumns(0);
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-1") {
            screen.insertColumns(1);
            REQUIRE("12345\n67 80\nAB CE\nFG HJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-2") {
            screen.insertColumns(2);
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-3-clamped") {
            screen.insertColumns(3);
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
    }

    SECTION("inside margins - repeative") {
        screen.moveCursorTo({2, 2});
        screen.insertColumns(1);
        REQUIRE("12345\n6 780\nA BCE\nF GHJ\nKLMNO\n" == screen.renderText());
        screen.insertColumns(1);
        REQUIRE("12345\n6  70\nA  BE\nF  GJ\nKLMNO\n" == screen.renderText());
    }
}

TEST_CASE("InsertCharacters", "[screen]")
{
    auto screen = MockScreen{{5, 2}};
    screen.write("12345\r\n67890");
    screen.setMode(Mode::LeftRightMargin, true);
    screen.setLeftRightMargin(2, 4);
    REQUIRE("12345\n67890\n" == screen.renderText());

    SECTION("outside margins: left") {
        screen.moveCursorTo({1, 1});
        screen.insertCharacters(1);
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("outside margins: right") {
        screen.moveCursorTo({1, 5});
        screen.insertCharacters( 1 );
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("inside margins") {
        screen.moveCursorTo({1, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

        SECTION("no-op") {
            screen.insertCharacters(0);
            REQUIRE(screen.renderText() == "12345\n67890\n");
        }

        SECTION("ICH-1") {
            screen.insertCharacters(1);
            REQUIRE(screen.renderText() == "12 35\n67890\n");
        }

        SECTION("ICH-2") {
            screen.insertCharacters(2);
            REQUIRE(screen.renderText() == "12  5\n67890\n");
        }

        SECTION("ICH-3-clamped") {
            screen.insertCharacters(3);
            REQUIRE(screen.renderText() == "12  5\n67890\n");
        }
    }
}

TEST_CASE("InsertLines", "[screen]")
{
    auto screen = MockScreen{{4, 6}};
    screen.write("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());

    SECTION("old") {
        auto screen = MockScreen{{2, 3}};

        screen.write("AB\r\nCD");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("CD" == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));

        screen.insertLines(1);
        CHECK("AB" == screen.renderTextLine(1));
        CHECK("  " == screen.renderTextLine(2));
        CHECK("CD" == screen.renderTextLine(3));

        screen.moveCursorTo({1, 1});
        screen.insertLines(1);
        CHECK("  " == screen.renderTextLine(1));
        CHECK("AB" == screen.renderTextLine(2));
        CHECK("  " == screen.renderTextLine(3));
    }
    // TODO: test with (top/bottom and left/right) margins enabled
}

TEST_CASE("DeleteLines", "[screen]")
{
    auto screen = MockScreen{{2, 3}};

    screen.write("AB\r\nCD\r\nEF");
    logScreenText(screen, "initial");
    REQUIRE("AB" == screen.renderTextLine(1));
    REQUIRE("CD" == screen.renderTextLine(2));
    REQUIRE("EF" == screen.renderTextLine(3));

    screen.moveCursorTo({2, 1});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});

    SECTION("no-op") {
        screen.deleteLines(0);
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("CD" == screen.renderTextLine(2));
        REQUIRE("EF" == screen.renderTextLine(3));
    }

    SECTION("in-range") {
        screen.deleteLines(1);
        logScreenText(screen, "After EL(1)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("EF" == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }

    SECTION("clamped") {
        screen.moveCursorTo({2, 2});
        screen.deleteLines(5);
        logScreenText(screen, "After clamped EL(5)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("  " == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }
}

TEST_CASE("DeleteColumns", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.setMode(Mode::LeftRightMargin, true);
    screen.setLeftRightMargin(2, 4);
    screen.setTopBottomMargin(2, 4);

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("outside margin") {
        screen.deleteColumns(1);
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("inside margin") {
        screen.moveCursorTo({ 2, 3 });
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

        SECTION("DECDC-0") {
            screen.deleteColumns(0);
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-1") {
            screen.deleteColumns(1);
            REQUIRE("12345\n679 0\nABD E\nFGI J\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-2") {
            screen.deleteColumns(2);
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-3-clamped") {
            screen.deleteColumns(4);
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
    }
}

TEST_CASE("DeleteCharacters", "[screen]")
{
    auto screen = MockScreen{{5, 2}};
    screen.write("12345\r\n67890\033[1;2H");
    REQUIRE("12345\n67890\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    SECTION("outside margin") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(2, 4);
        screen.moveCursorTo({ 1, 1 });
        screen.deleteCharacters(1);
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("without horizontal margin") {
        SECTION("no-op") {
            screen.deleteCharacters(0);
            REQUIRE("12345\n67890\n" == screen.renderText());
        }
        SECTION("in-range-1") {
            screen.deleteCharacters(1);
            REQUIRE("1345 \n67890\n" == screen.renderText());
        }
        SECTION("in-range-2") {
            screen.deleteCharacters(2);
            REQUIRE("145  \n67890\n" == screen.renderText());
        }
        SECTION("in-range-4") {
            screen.deleteCharacters(4);
            REQUIRE("1    \n67890\n" == screen.renderText());
        }
        SECTION("clamped") {
            screen.deleteCharacters(5);
            REQUIRE("1    \n67890\n" == screen.renderText());
        }
    }
    SECTION("with horizontal margin") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(1, 4 );
        screen.moveCursorTo({ 1, 2 });
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

        SECTION("no-op") {
            screen.deleteCharacters(0);
            REQUIRE("12345\n67890\n" == screen.renderText());
        }
        SECTION("in-range-1") {
            REQUIRE("12345\n67890\n" == screen.renderText());
            screen.deleteCharacters(1);
            REQUIRE("134 5\n67890\n" == screen.renderText());
        }
        SECTION("in-range-2") {
            screen.deleteCharacters(2);
            REQUIRE("14  5\n67890\n" == screen.renderText());
        }
        SECTION("clamped") {
            screen.deleteCharacters(4);
            REQUIRE("1   5\n67890\n" == screen.renderText());
        }
    }
}

TEST_CASE("ClearScrollbackBuffer", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\r\nPQRST\033[H");
    REQUIRE("67890\nABCDE\nFGHIJ\nKLMNO\nPQRST\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    REQUIRE(1 == screen.scrollbackLines().size());
    REQUIRE("12345" == screen.renderHistoryTextLine(1));
}

TEST_CASE("EraseCharacters", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\033[H");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("ECH-0 equals ECH-1") {
        screen.eraseCharacters(0);
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-1") {
        screen.eraseCharacters(1);
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-5") {
        screen.eraseCharacters(5);
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-6-clamped") {
        screen.eraseCharacters(6);
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }
}

TEST_CASE("ScrollUp", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    screen.write("ABC\r\n");
    screen.write("DEF\r\n");
    screen.write("GHI");
    REQUIRE("ABC\nDEF\nGHI\n" == screen.renderText());

    SECTION("no-op") {
        INFO("begin:");
        screen.currentBuffer().scrollUp(0);
        INFO("end:");
        REQUIRE("ABC\nDEF\nGHI\n" == screen.renderText());
    }

    SECTION("by-1") {
        screen.currentBuffer().scrollUp(1);
        REQUIRE("DEF\nGHI\n   \n" == screen.renderText());
    }

    SECTION("by-2") {
        screen.currentBuffer().scrollUp(2);
        REQUIRE("GHI\n   \n   \n" == screen.renderText());
    }

    SECTION("by-3") {
        screen.currentBuffer().scrollUp(3);
        REQUIRE("   \n   \n   \n" == screen.renderText());
    }

    SECTION("clamped") {
        screen.currentBuffer().scrollUp(4);
        REQUIRE("   \n   \n   \n" == screen.renderText());
    }
}

TEST_CASE("ScrollDown", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    SECTION("scroll fully inside margins") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(2, 4);
        screen.setTopBottomMargin(2, 4);
        screen.setMode(Mode::Origin, true);

        SECTION("SD 1") {
            screen.currentBuffer().scrollDown(1);
            CHECK("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("SD 2") {
            screen.currentBuffer().scrollDown(2);
            CHECK(
                "12345\n"
                "6   0\n"
                "A   E\n"
                "F789J\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 3") {
            screen.currentBuffer().scrollDown(3);
            CHECK(
                "12345\n"
                "6   0\n"
                "A   E\n"
                "F   J\n"
                "KLMNO\n" == screen.renderText());
        }
    }

    SECTION("vertical margins") {
        screen.setTopBottomMargin(2, 4);
        SECTION("SD 0") {
            screen.currentBuffer().scrollDown(0);
            REQUIRE(
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 1") {
            screen.currentBuffer().scrollDown(1);
            REQUIRE(
                "12345\n"
                "     \n"
                "67890\n"
                "ABCDE\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 3") {
            screen.currentBuffer().scrollDown(5);
            REQUIRE(
                "12345\n"
                "     \n"
                "     \n"
                "     \n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 4 clamped") {
            screen.currentBuffer().scrollDown(4);
            REQUIRE(
                "12345\n"
                "     \n"
                "     \n"
                "     \n"
                "KLMNO\n" == screen.renderText());
        }
    }

    SECTION("no custom margins") {
        SECTION("SD 0") {
            screen.currentBuffer().scrollDown(0);
            REQUIRE(
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                "KLMNO\n" == screen.renderText());
        }
        SECTION("SD 1") {
            screen.currentBuffer().scrollDown(1);
            REQUIRE(
                "     \n"
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                == screen.renderText());
        }
        SECTION("SD 5") {
            screen.currentBuffer().scrollDown(5);
            REQUIRE(
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                == screen.renderText());
        }
        SECTION("SD 6 clamped") {
            screen.currentBuffer().scrollDown(6);
            REQUIRE(
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                == screen.renderText());
        }
    }
}

TEST_CASE("MoveCursorUp", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    screen.moveCursorTo({3, 2});
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    SECTION("no-op") {
        screen.moveCursorUp(0);
        REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
    }

    SECTION("in-range") {
        screen.moveCursorUp(1);
        REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
    }

    SECTION("overflow") {
        screen.moveCursorUp(5);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("with margins") {
        screen.setTopBottomMargin(2, 4);
        screen.moveCursorTo({3, 2});
        REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

        SECTION("in-range") {
            screen.moveCursorUp(1);
            REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
        }

        SECTION("overflow") {
            screen.moveCursorUp(5);
            REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
        }
    }

    SECTION("cursor already above margins") {
        screen.setTopBottomMargin(3, 4);
        screen.moveCursorTo({2, 3});
        screen.moveCursorUp(1);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    }
}

TEST_CASE("MoveCursorDown", "[screen]")
{
    auto screen = MockScreen{{2, 3}};
    screen.write("A");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // no-op
    screen.moveCursorDown(0);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // in-range
    screen.moveCursorDown(1);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // overflow
    screen.moveCursorDown(5);
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
}

TEST_CASE("MoveCursorForward", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("no-op") {
        screen.moveCursorForward(0);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    }

    SECTION("CUF-1") {
        screen.moveCursorForward(1);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("CUF-3 (to right border)") {
        screen.moveCursorForward(screen.size().width);
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().width});
    }

    SECTION("CUF-overflow") {
        screen.moveCursorForward(screen.size().width + 1);
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().width});
    }
}

TEST_CASE("MoveCursorBackward", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    screen.write("ABC");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    // no-op
    screen.moveCursorBackward(0);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    // in-range
    screen.moveCursorBackward(1);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    // overflow
    screen.moveCursorBackward(5);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
}

TEST_CASE("HorizontalPositionAbsolute", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen.moveCursorToColumn(1);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen.moveCursorToColumn(3);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.moveCursorToColumn(2);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // overflow
    screen.moveCursorToColumn(5);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3 /*clamped*/});
}

TEST_CASE("HorizontalPositionRelative", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("no-op") {
        screen.moveCursorForward(0);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    }

    SECTION("HPR-1") {
        screen.moveCursorForward(1);
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("HPR-3 (to right border)") {
        screen.moveCursorForward(screen.size().width);
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().width});
    }

    SECTION("HPR-overflow") {
        screen.moveCursorForward(screen.size().width + 1);
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().width});
    }
}


TEST_CASE("MoveCursorToColumn", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen.moveCursorToColumn(1);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen.moveCursorToColumn(3);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.moveCursorToColumn(2);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // overflow
    screen.moveCursorToColumn(5);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3 /*clamped*/});

    SECTION("with wide character")
    {
        screen.moveCursorTo({1, 1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        screen.write(U"\u26A1"); // ⚡ :flash: (double width)
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    }
}

TEST_CASE("MoveCursorToLine", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen.moveCursorToLine(0);
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen.moveCursorToLine(3);
    REQUIRE(screen.cursorPosition() == Coordinate{3, 1});

    screen.moveCursorToLine(2);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});

    // overflow
    screen.moveCursorToLine(5);
    REQUIRE(screen.cursorPosition() == Coordinate{3, 1/*clamped*/});
}

TEST_CASE("MoveCursorToBeginOfLine", "[screen]")
{
    auto screen = MockScreen{{3, 3}};

    screen.write("\r\nAB");
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    screen.moveCursorToBeginOfLine();
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
}

TEST_CASE("MoveCursorTo", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    SECTION("origin mode disabled") {
        SECTION("in range") {
            screen.moveCursorTo({3, 2});
            REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
        }

        SECTION("origin") {
            screen.moveCursorTo({1, 1});
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }

        SECTION("clamped") {
            screen.moveCursorTo({6, 7});
            REQUIRE(screen.cursorPosition() == Coordinate{5, 5});
        }
    }

    SECTION("origin-mode enabled") {
        constexpr auto TopMargin = 2;
        constexpr auto BottomMargin = 4;
        constexpr auto LeftMargin = 2;
        constexpr auto RightMargin = 4;
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(LeftMargin, RightMargin);
        screen.setTopBottomMargin(TopMargin, BottomMargin);
        screen.setMode(Mode::Origin, true);

        SECTION("move to origin") {
            screen.moveCursorTo({1, 1});
            CHECK(Coordinate{1, 1} == screen.cursorPosition());
            CHECK(Coordinate{2, 2} == screen.realCursorPosition());
            CHECK('7' == (char)screen.at({1 + (TopMargin - 1), 1 + (LeftMargin - 1)}).codepoint(0));
            CHECK('I' == (char)screen.at({3 + (TopMargin - 1), 3 + (LeftMargin - 1)}).codepoint(0));
        }
    }
}

TEST_CASE("MoveCursorToNextTab", "[screen]")
{
    auto constexpr TabWidth = 8;
    auto screen = MockScreen{{20, 3}};
    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1 * TabWidth + 1});

    screen.moveCursorToColumn(TabWidth - 1);
    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1 * TabWidth + 1});

    screen.moveCursorToColumn(TabWidth);
    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1 * TabWidth + 1});

    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2 * TabWidth + 1});

    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 20});

    screen.setMode(Mode::AutoWrap, true);
    screen.write("A"); // 'A' is being written at the right margin
    screen.write("B"); // force wrap to next line, writing 'B' at the beginning of the line

    screen.moveCursorToNextTab();
    REQUIRE(screen.cursorPosition() == Coordinate{2, 9});
}

// TODO: HideCursor
// TODO: ShowCursor

TEST_CASE("SaveCursor and RestoreCursor", "[screen]")
{
    auto screen = MockScreen{{3, 3}};
    screen.setMode(Mode::AutoWrap, false);
    screen.saveCursor();

    // mutate the cursor's position, autowrap and origin flags
    screen.moveCursorTo({3, 3});
    screen.setMode(Mode::AutoWrap, true);
    screen.setMode(Mode::Origin, true);

    // restore cursor and see if the changes have been reverted
    screen.restoreCursor();
    CHECK(screen.cursorPosition() == Coordinate{1, 1});
    CHECK_FALSE(screen.isModeEnabled(Mode::AutoWrap));
    CHECK_FALSE(screen.isModeEnabled(Mode::Origin));
}

TEST_CASE("Index_outside_margin", "[screen]")
{
    auto screen = MockScreen{{4, 6}};
    screen.write("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    logScreenText(screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    screen.setTopBottomMargin(2, 4);

    // with cursor above top margin
    screen.moveCursorTo({1, 3});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen.moveCursorTo({5, 3});
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{6, 3});

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen.moveCursorTo({6, 3});
    screen.index();
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{6, 3});
}

TEST_CASE("Index_inside_margin", "[screen]")
{
    auto screen = MockScreen{{2, 6}};
    screen.write("11\r\n22\r\n33\r\n44\r\n55\r\n66");
    logScreenText(screen, "initial setup");

    // test IND when cursor is within margin range (=> move cursor down)
    screen.setTopBottomMargin(2, 4);
    screen.moveCursorTo({3, 2});
    screen.index();
    logScreenText(screen, "IND while cursor at line 3");
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
    REQUIRE("11\n22\n33\n44\n55\n66\n" == screen.renderText());
}

TEST_CASE("Index_at_bottom_margin", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial setup");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen.setTopBottomMargin(2, 4);

    SECTION("cursor at bottom margin and full horizontal margins") {
        screen.moveCursorTo({4, 2});
        screen.index();
        logScreenText(screen, "IND while cursor at bottom margin");
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
        REQUIRE("12345\nABCDE\nFGHIJ\n     \nKLMNO\n" == screen.renderText());
    }

    SECTION("cursor at bottom margin and NOT full horizontal margins") {
        screen.moveCursorTo({1, 1});
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(2, 4);
        screen.setTopBottomMargin(2, 4);
        screen.moveCursorTo({4, 2}); // cursor at bottom margin
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

        screen.index();
        CHECK("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
    }
}

TEST_CASE("ReverseIndex_without_custom_margins", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    // at bottom screen
    screen.moveCursorTo({5, 2});
    screen.reverseIndex();
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    screen.reverseIndex();
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen.reverseIndex();
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    screen.reverseIndex();
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen.reverseIndex();
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n12345\n67890\nABCDE\nFGHIJ\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen.reverseIndex();
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n     \n12345\n67890\nABCDE\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ReverseIndex_with_vertical_margin", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen.setTopBottomMargin(2, 4);

    // below bottom margin
    screen.moveCursorTo({5, 2});
    screen.reverseIndex();
    logScreenText(screen, "RI below bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    // at bottom margin
    screen.reverseIndex();
    logScreenText(screen, "RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen.reverseIndex();
    logScreenText(screen, "RI middle margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin
    screen.reverseIndex();
    logScreenText(screen, "RI at top margin #1");
    REQUIRE("12345\n     \n67890\nABCDE\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin (again)
    screen.reverseIndex();
    logScreenText(screen, "RI at top margin #2");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // above top margin
    screen.moveCursorTo({1, 2});
    screen.reverseIndex();
    logScreenText(screen, "RI above top margin");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // above top margin (top screen) => no-op
    screen.reverseIndex();
    logScreenText(screen, "RI above top margin (top-screen)");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ReverseIndex_with_vertical_and_horizontal_margin", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen.setMode(Mode::LeftRightMargin, true);
    screen.setLeftRightMargin(2, 4);
    screen.setTopBottomMargin(2, 4);

    // below bottom margin
    screen.moveCursorTo({5, 2});
    screen.reverseIndex();
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    // at bottom margin
    screen.reverseIndex();
    logScreenText(screen, "after RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen.reverseIndex();
    logScreenText(screen, "after RI at bottom margin (again)");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin
    screen.reverseIndex();
    logScreenText(screen, "after RI at top margin");
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
    REQUIRE("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderText());

    // at top margin (again)
    screen.reverseIndex();
    logScreenText(screen, "after RI at top margin (again)");
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // above top margin
    screen.moveCursorTo({1, 2});
    screen.reverseIndex();
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ScreenAlignmentPattern", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.setTopBottomMargin(2, 4);
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    REQUIRE(2 == screen.margin().vertical.from);
    REQUIRE(4 == screen.margin().vertical.to);

    SECTION("test") {
        screen.screenAlignmentPattern();
        REQUIRE("EEEEE\nEEEEE\nEEEEE\nEEEEE\nEEEEE\n" == screen.renderText());

        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

        REQUIRE(1 == screen.margin().horizontal.from);
        REQUIRE(5 == screen.margin().horizontal.to);
        REQUIRE(1 == screen.margin().vertical.from);
        REQUIRE(5 == screen.margin().vertical.to);
    }
}

TEST_CASE("CursorNextLine", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo({2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("without margins") {
        SECTION("normal") {
            screen.moveCursorToNextLine(1);
            REQUIRE(screen.cursorPosition() == Coordinate{3, 1});
        }

        SECTION("clamped") {
            screen.moveCursorToNextLine(5);
            REQUIRE(screen.cursorPosition() == Coordinate{5, 1});
        }
    }

    SECTION("with margins") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(2, 4);
        screen.setTopBottomMargin(2, 4);
        screen.setMode(Mode::Origin, true);
        screen.moveCursorTo({1, 2});
        REQUIRE(screen.currentCell().toUtf8() == "8");

        SECTION("normal-1") {
            screen.moveCursorToNextLine(1);
            REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
        }

        SECTION("normal-2") {
            screen.moveCursorToNextLine(2);
            REQUIRE(screen.cursorPosition() == Coordinate{3, 1});
        }

        SECTION("normal-3") {
            screen.moveCursorToNextLine(3);
            REQUIRE(screen.cursorPosition() == Coordinate{4, 1});
        }

        SECTION("clamped-1") {
            screen.moveCursorToNextLine(4);
            REQUIRE(screen.cursorPosition() == Coordinate{4, 1});
        }
    }
}

TEST_CASE("CursorPreviousLine", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{5, 5});

    SECTION("without margins") {
        SECTION("normal") {
            screen.moveCursorToPrevLine(1);
            REQUIRE(screen.cursorPosition() == Coordinate{4, 1});
        }

        SECTION("clamped") {
            screen.moveCursorToPrevLine(5);
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }
    }

    SECTION("with margins") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setLeftRightMargin(2, 4);
        screen.setTopBottomMargin(2, 4);
        screen.setMode(Mode::Origin, true);
        screen.moveCursorTo({3, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

        SECTION("normal-1") {
            screen.moveCursorToPrevLine(1);
            REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
        }

        SECTION("normal-2") {
            screen.moveCursorToPrevLine(2);
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }

        SECTION("clamped") {
            screen.moveCursorToPrevLine(3);
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }
    }
}

TEST_CASE("ReportCursorPosition", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo({2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE("" == screen.replyData);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("with Origin mode disabled") {
        screen.reportCursorPosition();
        CHECK("\033[2;3R" == screen.replyData);
    }

    SECTION("with margins and origin mode enabled") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setTopBottomMargin(2, 4);
        screen.setLeftRightMargin(2, 4);
        screen.setMode(Mode::Origin, true);
        screen.moveCursorTo({3, 2});

        screen.reportCursorPosition();
        CHECK("\033[3;2R" == screen.replyData);
    }
}

TEST_CASE("ReportExtendedCursorPosition", "[screen]")
{
    auto screen = MockScreen{{5, 5}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen.moveCursorTo({2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE("" == screen.replyData);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("with Origin mode disabled") {
        screen.reportExtendedCursorPosition();
        CHECK("\033[2;3;1R" == screen.replyData);
    }

    SECTION("with margins and origin mode enabled") {
        screen.setMode(Mode::LeftRightMargin, true);
        screen.setTopBottomMargin(2, 4);
        screen.setLeftRightMargin(2, 4);
        screen.setMode(Mode::Origin, true);
        screen.moveCursorTo({3, 2});

        screen.reportExtendedCursorPosition();
        CHECK("\033[3;2;1R" == screen.replyData);
    }
}

TEST_CASE("SetMode", "[screen]") {
    SECTION("Auto NewLine Mode: Enabled") {
        auto screen = MockScreen{{5, 5}};
        screen.setMode(Mode::AutomaticNewLine, true);
        screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
        REQUIRE(screen.renderText() == "12345\n67890\nABCDE\nFGHIJ\nKLMNO\n");
    }

    SECTION("Auto NewLine Mode: Disabled") {
        auto screen = MockScreen{{3, 3}};
        screen.write("A\nB\nC");
        REQUIRE(screen.renderText() == "A  \n B \n  C\n");
    }
}

TEST_CASE("RequestMode", "[screen]")
{
    auto screen = MockScreen{{5, 5}};

    SECTION("ANSI modes") {
        screen.setMode(Mode::Insert, true); // IRM
        screen.requestMode(Mode::Insert);
        REQUIRE(screen.replyData == fmt::format("\033[{};1$y", to_code(Mode::Insert)));
    }

    SECTION("DEC modes") {
        screen.setMode(Mode::Origin, true); // DECOM
        screen.requestMode(Mode::Origin);
        REQUIRE(screen.replyData == fmt::format("\033[?{};1$y", to_code(Mode::Origin)));
    }
}

TEST_CASE("peek into history", "[screen]")
{
    auto screen = MockScreen{{3, 2}};
    screen.write("123\r\n456\r\nABC\r\nDEF");

    REQUIRE("ABC\nDEF\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    // first line in history
    CHECK(screen.at({-1, 1}).codepoint(0) == '1');
    CHECK(screen.at({-1, 2}).codepoint(0) == '2');
    CHECK(screen.at({-1, 3}).codepoint(0) == '3');

    // second line in history
    CHECK(screen.at({0, 1}).codepoint(0) == '4');
    CHECK(screen.at({0, 2}).codepoint(0) == '5');
    CHECK(screen.at({0, 3}).codepoint(0) == '6');

    // first line on screen buffer
    CHECK(screen.at({1, 1}).codepoint(0) == 'A');
    CHECK(screen.at({1, 2}).codepoint(0) == 'B');
    CHECK(screen.at({1, 3}).codepoint(0) == 'C');

    // second line on screen buffer
    CHECK(screen.at({2, 1}).codepoint(0) == 'D');
    CHECK(screen.at({2, 2}).codepoint(0) == 'E');
    CHECK(screen.at({2, 3}).codepoint(0) == 'F');

    // out-of-range corner cases
    // CHECK_THROWS(screen.at({3, 1}));
    // CHECK_THROWS(screen.at({2, 4}));
    // CHECK_THROWS(screen.at({2, 0}));
    // XXX currently not checked, as they're intentionally using assert() instead.
}

TEST_CASE("render into history", "[screen]")
{
    auto screen = MockScreen{{5, 2}};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("FGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 5});
    REQUIRE(screen.historyLineCount() == 3);

    string renderedText;
    renderedText.resize(2 * 6);
    auto const renderer = [&](Coordinate const& pos, Cell const& cell) {
        renderedText[(pos.row - 1) * 6 + (pos.column - 1)] = static_cast<char>(cell.codepoint(0));
        if (pos.column == 5)
            renderedText[(pos.row - 1) * 6 + (pos.column)] = '\n';
    };

    SECTION("main area") {
        screen.render(renderer);
        REQUIRE("FGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("1 line into history") {
        screen.render(renderer, 2);
        REQUIRE("ABCDE\nFGHIJ\n" == renderedText);
    }

    SECTION("2 lines into history") {
        screen.render(renderer, 1);
        REQUIRE("67890\nABCDE\n" == renderedText);
    }

    SECTION("3 lines into history") {
        screen.render(renderer, 0);
        REQUIRE("12345\n67890\n" == renderedText);
    }

    SECTION("4 lines into history (1 clamped)") {
        screen.render(renderer, -1);
        REQUIRE("12345\n67890\n" == renderedText);
    }
}

TEST_CASE("HorizontalTabClear.AllTabs", "[screen]")
{
    auto screen = MockScreen{{5, 3}};
    screen.horizontalTabClear(HorizontalTabClear::AllTabs);

    screen.writeText('X');
    screen.moveCursorToNextTab();
    screen.writeText('Y');
    REQUIRE("X   Y" == screen.renderTextLine(1));

    screen.moveCursorToNextTab();
    screen.writeText('Z');
    REQUIRE("X   Y" == screen.renderTextLine(1));
    REQUIRE("Z    " == screen.renderTextLine(2));

    screen.moveCursorToNextTab();
    screen.writeText('A');
    REQUIRE("X   Y" == screen.renderTextLine(1));
    REQUIRE("Z   A" == screen.renderTextLine(2));
}

TEST_CASE("HorizontalTabClear.UnderCursor", "[screen]")
{
    auto screen = MockScreen{{10, 3}};
    screen.setTabWidth(4);

    // clear tab at column 4
    screen.moveCursorTo({1, 4});
    screen.horizontalTabClear(HorizontalTabClear::UnderCursor);

    screen.moveCursorTo({1, 1});
    screen.writeText('A');
    screen.moveCursorToNextTab();
    screen.writeText('B');

    //       1234567890
    REQUIRE("A      B  " == screen.renderTextLine(1));
    REQUIRE("          " == screen.renderTextLine(2));

    screen.moveCursorToNextTab();
    screen.writeText('C');
    CHECK("A      B C" == screen.renderTextLine(1));
    CHECK("          " == screen.renderTextLine(2));
}

TEST_CASE("HorizontalTabSet", "[screen]")
{
    auto screen = MockScreen{{10, 3}};
    screen.horizontalTabClear(HorizontalTabClear::AllTabs);

    screen.moveCursorToColumn(3);
    screen.horizontalTabSet();

    screen.moveCursorToColumn(5);
    screen.horizontalTabSet();

    screen.moveCursorToColumn(8);
    screen.horizontalTabSet();

    screen.moveCursorToBeginOfLine();

    screen.writeText('1');

    screen.moveCursorToNextTab();
    screen.writeText('3');

    screen.moveCursorToNextTab();
    screen.writeText('5');

    screen.moveCursorToNextTab();
    screen.writeText('8');

    screen.moveCursorToNextTab(); // capped
    screen.writeText('A');       // writes B at right margin, flags for autowrap

    REQUIRE("1 3 5  8 A" == screen.renderTextLine(1));

    screen.moveCursorToNextTab();  // wrapped
    screen.writeText('B');        // writes B at left margin

    //       1234567890
    REQUIRE("1 3 5  8 A" == screen.renderTextLine(1));
    screen.moveCursorToNextTab();  // 1 -> 3 (overflow)
    screen.moveCursorToNextTab();  // 3 -> 5
    screen.moveCursorToNextTab();  // 5 -> 8
    screen.writeText('C');

    //     1234567890
    CHECK("1 3 5  8 A" == screen.renderTextLine(1));
    CHECK("B      C  " == screen.renderTextLine(2));
}

TEST_CASE("CursorBackwardTab.fixedTabWidth", "[screen]")
{
    auto screen = MockScreen{{10, 3}};
    screen.setTabWidth(4); // 5, 9

    screen.writeText('a');

    screen.moveCursorToNextTab(); // -> 5
    screen.writeText('b');

    screen.moveCursorToNextTab();
    screen.writeText('c');       // -> 9

    //      "1234567890"
    REQUIRE("a   b   c " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition() == Coordinate{1, 10});

    SECTION("oveflow") {
        screen.cursorBackwardTab(4);
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen.writeText('X');
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("exact") {
        screen.cursorBackwardTab(3);
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen.writeText('X');
        //    "1234567890"
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("inside 2") {
        screen.cursorBackwardTab(2);
        CHECK(screen.cursorPosition() == Coordinate{1, 5});
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   X   c " == screen.renderTextLine(1));
    }

    SECTION("inside 1") {
        screen.cursorBackwardTab(1);
        CHECK(screen.cursorPosition() == Coordinate{1, 9});
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   b   X " == screen.renderTextLine(1));
    }

    SECTION("no op") {
        screen.cursorBackwardTab(0);
        CHECK(screen.cursorPosition() == Coordinate{1, 10});
    }
}

TEST_CASE("CursorBackwardTab.manualTabs", "[screen]")
{
    auto screen = MockScreen{{10, 3}};

    screen.moveCursorToColumn(5);
    screen.horizontalTabSet();
    screen.moveCursorToColumn(9);
    screen.horizontalTabSet();
    screen.moveCursorToBeginOfLine();

    screen.writeText('a');

    screen.moveCursorToNextTab(); // -> 5
    screen.writeText('b');

    screen.moveCursorToNextTab();
    screen.writeText('c');       // -> 9

    //      "1234567890"
    REQUIRE("a   b   c " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition().column == 10);

    SECTION("oveflow") {
        screen.cursorBackwardTab(4);
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen.writeText('X');
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("exact") {
        screen.cursorBackwardTab(3);
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen.writeText('X');
        //    "1234567890"
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("inside 2") {
        screen.cursorBackwardTab(2);
        CHECK(screen.cursorPosition() == Coordinate{1, 5});
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   X   c " == screen.renderTextLine(1));
    }

    SECTION("inside 1") {
        screen.cursorBackwardTab(1);
        CHECK(screen.cursorPosition() == Coordinate{1, 9});
        screen.writeText('X');
        //    "1234567890"
        CHECK("a   b   X " == screen.renderTextLine(1));
    }

    SECTION("no op") {
        screen.cursorBackwardTab(0);
        CHECK(screen.cursorPosition() == Coordinate{1, 10});
    }
}

// TEST_CASE("findNextMarker", "[screen]")
// {
//     auto screen = MockScreen{{4, 2}};
//
//     //REQUIRE_FALSE(screen.findNextMarker(0).has_value());
//
//     SECTION("no marks") {
//         screen.write("1abc\r\n"s);
//         screen.write("2def\r\n"s);
//         screen.write("3ghi\r\n"s);
//         screen.write("4jkl\r\n"s);
//         screen.write("5mno\r\n"s);
//
//         auto marker = screen.findNextMarker(0);
//         REQUIRE(marker.has_value());
//         CHECK(marker.value() == 0);
//
//         // CHECK(screen.findNextMarker(0).value() == 0);
//         // CHECK(screen.findNextMarker(1).value() == 0);
//         // CHECK(screen.findNextMarker(2).value() == 0);
//         // CHECK(screen.findNextMarker(3).value() == 0);
//         // CHECK(screen.findNextMarker(4).value() == 0);
//         // CHECK(screen.findNextMarker(5).value() == 0);
//     }
//
//     SECTION("with marks") {
//         // history area
//         screen.setMark();
//         screen.write("1abc\r\n"s);  // 2
//         screen.setMark();
//         screen.write("2def\r\n"s);  // 1
//         screen.setMark();
//         screen.write("3ghi\r\n"s);  // 0
//
//         // screen area
//         screen.setMark();
//         screen.write("4jkl\r\n"s);
//         screen.write("5mno\r\n"s);
//
//         REQUIRE(screen.renderTextLine(1) == "5mno");
//         REQUIRE(screen.renderTextLine(2) == "    ");
//
//         auto marker = screen.findNextMarker(0);
//         CHECK(marker.value() == 0);
//
//         marker = screen.findNextMarker(1);
//         CHECK(marker.has_value());
//         CHECK(marker.value() == 0); // 3ghi
//
//         marker = screen.findNextMarker(2);
//         CHECK(marker.has_value());
//         CHECK(marker.value() == 1); // 2def
//     }
// }

TEST_CASE("findMarkerForward", "[screen]")
{
    auto screen = MockScreen{{4, 3}};
    REQUIRE_FALSE(screen.findMarkerForward(0).has_value()); // peak into history
    REQUIRE_FALSE(screen.findMarkerForward(1).has_value());
    REQUIRE_FALSE(screen.findMarkerForward(2).has_value());
    REQUIRE_FALSE(screen.findMarkerForward(3).has_value());
    REQUIRE_FALSE(screen.findMarkerForward(4).has_value()); // overflow

    SECTION("no marks") {
        screen.write("1abc"sv); // 0: +
        screen.write("2def"sv); // 1: | history
        screen.write("3ghi"sv); // 2: +
        screen.write("4jkl"sv); // 3: +
        screen.write("5mno"sv); // 4: | main screen
        screen.write("6pqr"sv); // 5: +

        REQUIRE(screen.historyLineCount() == 3);

        // test bottom line
        auto mark = screen.findMarkerForward(5);
        REQUIRE_FALSE(mark.has_value());

        // test one line beyond history line count
        mark = screen.findMarkerForward(-1);
        REQUIRE_FALSE(mark.has_value());

        // test last history line
        mark = screen.findMarkerForward(0);
        REQUIRE_FALSE(mark.has_value());

        // test second-last history line
        mark = screen.findMarkerForward(1);
        REQUIRE_FALSE(mark.has_value());

        // test first history line
        mark = screen.findMarkerForward(2);
        REQUIRE_FALSE(mark.has_value());
    }

    SECTION("with marks") {
        // saved lines
        screen.setMark();    // 0
        screen.write("1abc\r\n"sv);
        screen.write("2def\r\n"sv); // 1
        screen.setMark();
        screen.write("3ghi\r\n"sv); // 2

        // visibile screen
        screen.setMark();    // 3
        screen.write("4jkl\r\n"sv);
        screen.write("5mno\r\n"sv); // 4
        screen.setMark();    // 5
        screen.write("6pqr"sv);

        REQUIRE(screen.renderTextLine(-2) == "1abc");
        REQUIRE(screen.renderTextLine(-1) == "2def");
        REQUIRE(screen.renderTextLine(0) == "3ghi");

        REQUIRE(screen.renderTextLine(1) == "4jkl");
        REQUIRE(screen.renderTextLine(2) == "5mno");
        REQUIRE(screen.renderTextLine(3) == "6pqr");

        // ======================================================

        // 0: -> 2
        auto marker = screen.findMarkerForward(0);
        CHECK(marker.has_value());
        if (marker.has_value())
            CHECK(marker.value() == 2); // 3ghi

        // 1: -> 2
        marker = screen.findMarkerForward(1);
        CHECK(marker.has_value());
        if (marker.has_value())
            CHECK(marker.value() == 2); // 3ghi

        // 2: -> 3
        marker = screen.findMarkerForward(2);
        CHECK(marker.has_value());
        if (marker.has_value())
            CHECK(marker.value() == 3); // 4jkl

        // 3: -> 5
        marker = screen.findMarkerForward(3);
        CHECK(marker.has_value());
        if (marker.has_value())
            CHECK(marker.value() == 5); // 6pqn

        // 4: -> 5
        marker = screen.findMarkerForward(4);
        CHECK(marker.has_value());
        if (marker.has_value())
            CHECK(marker.value() == 5); // 6pqn

        // 5: -> NONE (bottom of screen already)
        marker = screen.findMarkerForward(5);
        CHECK_FALSE(marker.has_value());
    }
}

TEST_CASE("findMarkerBackward", "[screen]")
{
    auto screen = MockScreen{{4, 3}};
    REQUIRE_FALSE(screen.findMarkerBackward(0).has_value()); // peak into history
    REQUIRE_FALSE(screen.findMarkerBackward(1).has_value());
    REQUIRE_FALSE(screen.findMarkerBackward(2).has_value());
    REQUIRE_FALSE(screen.findMarkerBackward(3).has_value());
    REQUIRE_FALSE(screen.findMarkerBackward(4).has_value()); // overflow

    SECTION("no marks") {
        screen.write("1abc"sv);
        screen.write("2def"sv);
        screen.write("3ghi"sv);
        screen.write("4jkl"sv);
        screen.write("5mno"sv);
        screen.write("6pqr"sv);

        REQUIRE(screen.historyLineCount() == 3);

        auto mark = screen.findMarkerBackward(screen.size().height);
        REQUIRE_FALSE(mark.has_value());

        // test one line beyond history line count
        mark = screen.findMarkerBackward(-1);
        REQUIRE_FALSE(mark.has_value());

        // test last history line
        mark = screen.findMarkerBackward(0);
        REQUIRE_FALSE(mark.has_value());

        // test second-last history line
        mark = screen.findMarkerBackward(1);
        REQUIRE_FALSE(mark.has_value());

        // test first history line
        mark = screen.findMarkerBackward(2);
        REQUIRE_FALSE(mark.has_value());
    }

    SECTION("with marks") {
        // saved lines
        screen.setMark();    // 0
        screen.write("1abc\r\n"sv);
        screen.write("2def\r\n"sv); // 1
        screen.setMark();
        screen.write("3ghi\r\n"sv); // 2

        // visibile screen
        screen.setMark();    // 3
        screen.write("4jkl\r\n"sv);
        screen.write("5mno\r\n"sv); // 4
        screen.setMark();    // 5
        screen.write("6pqr"sv);

        REQUIRE(screen.renderTextLine(-2) == "1abc");
        REQUIRE(screen.renderTextLine(-1) == "2def");
        REQUIRE(screen.renderTextLine(0) == "3ghi");

        REQUIRE(screen.renderTextLine(1) == "4jkl");
        REQUIRE(screen.renderTextLine(2) == "5mno");
        REQUIRE(screen.renderTextLine(3) == "6pqr");

        // ======================================================

        // 5: -> 3
        auto marker = screen.findMarkerBackward(5);
        REQUIRE(marker.has_value());
        CHECK(marker.value() == 3); // 4jkl

        // 4: -> 3
        marker = screen.findMarkerBackward(4);
        REQUIRE(marker.has_value());
        CHECK(marker.value() == 3); // 4jkl

        // 3: -> 2
        marker = screen.findMarkerBackward(3);
        REQUIRE(marker.has_value());
        CHECK(marker.value() == 2); // 3gh

        // 2: -> 0
        marker = screen.findMarkerBackward(2);
        REQUIRE(marker.has_value());
        CHECK(marker.value() == 0); // 1abc

        // 1: -> 0
        marker = screen.findMarkerBackward(1);
        REQUIRE(marker.has_value());
        CHECK(marker.value() == 0); // 1abc

        // 0: -> NONE
        marker = screen.findMarkerBackward(0);
        CHECK_FALSE(marker.has_value());

        // -1: -> NONE (one off edge case)
        marker = screen.findMarkerBackward(-1);
        CHECK_FALSE(marker.has_value());
    }
}

TEST_CASE("DECTABSR", "[screen]")
{
    auto screen = MockScreen{{35, 2}};

    SECTION("default tabstops") {
        screen.requestTabStops();
        CHECK(screen.replyData == "\033P2$u9/17/25/33\x5c");
    }

    SECTION("cleared tabs") {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);
        screen.requestTabStops();
        CHECK(screen.replyData == "\033P2$u\x5c");
    }

    SECTION("custom tabstops") {
        screen.horizontalTabClear(HorizontalTabClear::AllTabs);

        screen.moveCursorToColumn(2);
        screen.horizontalTabSet();

        screen.moveCursorToColumn(4);
        screen.horizontalTabSet();

        screen.moveCursorToColumn(8);
        screen.horizontalTabSet();

        screen.moveCursorToColumn(16);
        screen.horizontalTabSet();

        screen.requestTabStops();
        CHECK(screen.replyData == "\033P2$u2/4/8/16\x5c");
    }
}

TEST_CASE("save_restore_DEC_modes", "[screen]")
{
    auto screen = MockScreen{{2, 2}};

    screen.setMode(Mode::MouseProtocolHighlightTracking, false);
    screen.saveModes(vector{Mode::MouseProtocolHighlightTracking});

    screen.setMode(Mode::MouseProtocolHighlightTracking, true);
    CHECK(screen.isModeEnabled(Mode::MouseProtocolHighlightTracking));

    screen.restoreModes(vector{Mode::MouseProtocolHighlightTracking});
    CHECK_FALSE(screen.isModeEnabled(Mode::MouseProtocolHighlightTracking));
}

TEST_CASE("resize", "[screen]")
{
    auto screen = MockScreen{{2, 2}};
    screen.write("AB\r\nCD");
    REQUIRE("AB\nCD\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    SECTION("no-op") {
        screen.resize({2, 2});
        REQUIRE("AB\nCD\n" == screen.renderText());
    }

    SECTION("grow lines") {
        screen.resize({2, 3});
        REQUIRE("AB\nCD\n  \n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

        screen.write("EF");
        REQUIRE("AB\nCD\nEF\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
    }

    SECTION("shrink lines") {
        screen.resize({2, 1});
        REQUIRE("CD\n" == screen.renderText());
        REQUIRE("AB" == screen.renderHistoryTextLine(1));
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("grow columns") {
        screen.resize({3, 2});
        REQUIRE("AB \nCD \n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});
    }

    SECTION("shrink columns") {
        screen.resize({1, 2});
        REQUIRE("A\nC\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
    }

    SECTION("regrow columns") {
        // 1.) grow
        screen.resize({3, 2});
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

        // 2.) fill
        screen.writeText('Y');
        REQUIRE("AB \nCDY\n" == screen.renderText());
        screen.moveCursorTo({1, 3});
        screen.writeText('X');
        REQUIRE("ABX\nCDY\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

        // 3.) shrink
        screen.resize({2, 2});
        REQUIRE("AB\nCD\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

        // 4.) regrow (and see if pre-filled data were retained)
        screen.resize({3, 2});
        REQUIRE("ABX\nCDY\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    }

    SECTION("grow rows, grow columns") {
        screen.resize({3, 3});
        REQUIRE("AB \nCD \n   \n" == screen.renderText());
        screen.write("1\r\n234");
        REQUIRE("AB \nCD1\n234\n" == screen.renderText());
    }

    SECTION("grow rows, shrink columns") {
        screen.resize({1, 3});
        REQUIRE("A\nC\n \n" == screen.renderText());
    }

    SECTION("shrink rows, grow columns") {
        screen.resize({3, 1});
        REQUIRE("CD \n" == screen.renderText());
    }

    SECTION("shrink rows, shrink columns") {
        screen.resize({1, 1});
        REQUIRE("C\n" == screen.renderText());
    }

    // TODO: what do we want to do when re resize to {0, y}, {x, 0}, {0, 0}?
}

// TODO: SetForegroundColor
// TODO: SetBackgroundColor
// TODO: SetGraphicsRendition
// TODO: SetScrollingRegion

// TODO: SendMouseEvents
// TODO: AlternateKeypadMode

// TODO: DesignateCharset
// TODO: SingleShiftSelect

// TODO: ChangeWindowTitle

// TODO: Bell
// TODO: FullReset

// TODO: DeviceStatusReport
// TODO: SendDeviceAttributes
// TODO: SendTerminalId
