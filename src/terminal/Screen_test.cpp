/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

void logScreenText(Screen const& screen, string const& headline = "")
{
    if (headline.empty())
        UNSCOPED_INFO("dump:");
    else
        UNSCOPED_INFO(headline + ":");

    for (cursor_pos_t row = 1; row <= screen.size().rows; ++row)
        UNSCOPED_INFO(fmt::format("[{}] \"{}\"", row, screen.renderTextLine(row)));
}

TEST_CASE("resize", "[screen]")
{
    auto screen = Screen{{2, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
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

        // 2.) fill
        screen.write("Y\033[1;3HX");
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

TEST_CASE("AppendChar", "[screen]")
{
    auto screen = Screen{{3, 1}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    REQUIRE("   " == screen.renderTextLine(1));

    screen(SetMode{ Mode::AutoWrap, false });

    screen.write("A");
    REQUIRE("A  " == screen.renderTextLine(1));

    screen.write("B");
    REQUIRE("AB " == screen.renderTextLine(1));

    screen.write("C");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen.write("D");
    REQUIRE("ABD" == screen.renderTextLine(1));

    screen(SetMode{ Mode::AutoWrap, true });
    screen.write("EF");
    REQUIRE("F  " == screen.renderTextLine(1));
}

TEST_CASE("AppendChar_AutoWrap", "[screen]")
{
    auto screen = Screen{{3, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(SetMode{Mode::AutoWrap, true});

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
    auto screen = Screen{{3, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(SetMode{Mode::AutoWrap, true});

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
    auto screen = Screen{{3, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    screen.write("12");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    screen.write("\b");
    CHECK("12 " == screen.renderTextLine(1));;
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
}

TEST_CASE("Linefeed", "[screen]")
{
    auto screen = Screen{{2, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
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
    Screen screen{{3, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));
    REQUIRE("GHI" == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

    screen(MoveCursorTo{2, 2});
    screen(ClearToEndOfScreen{});

    CHECK("ABC" == screen.renderTextLine(1));
    CHECK("D  " == screen.renderTextLine(2));
    CHECK("   " == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
}

TEST_CASE("ClearToBeginOfScreen", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write("ABC\r\nDEF\r\nGHI");

    REQUIRE("ABC" == screen.renderTextLine(1));
    REQUIRE("DEF" == screen.renderTextLine(2));
    REQUIRE("GHI" == screen.renderTextLine(3));
    REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

    screen(MoveCursorTo{2, 2});
    screen(ClearToBeginOfScreen{});

    CHECK("   " == screen.renderTextLine(1));
    CHECK("  F" == screen.renderTextLine(2));
    CHECK("GHI" == screen.renderTextLine(3));
    CHECK(screen.cursorPosition() == Coordinate{2, 2});
}

TEST_CASE("ClearScreen", "[screen]")
{
    Screen screen{{2, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write("AB\r\nC");
    screen(ClearScreen{});
    CHECK("  " == screen.renderTextLine(1));
    CHECK("  " == screen.renderTextLine(2));
}

TEST_CASE("ClearToEndOfLine", "[screen]")
{
    Screen screen{{3, 1}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToEndOfLine{});
    CHECK("A  " == screen.renderTextLine(1));
}

TEST_CASE("ClearToBeginOfLine", "[screen]")
{
    Screen screen{{3, 1}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(MoveCursorToColumn{2});
    screen(ClearToBeginOfLine{});
    CHECK("  C" == screen.renderTextLine(1));
}

TEST_CASE("ClearLine", "[screen]")
{
    Screen screen{{3, 1}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(SetMode{Mode::AutoWrap, false});
    screen.write("ABC");
    REQUIRE("ABC" == screen.renderTextLine(1));

    screen(ClearLine{});
    CHECK("   " == screen.renderTextLine(1));
}

TEST_CASE("InsertColumns", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(SetMode{ Mode::LeftRightMargin, true });
    screen(SetLeftRightMargin{2, 4});
    screen(SetTopBottomMargin{2, 4});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("outside margins: top left") {
        screen(MoveCursorTo{1, 1});
        screen(InsertColumns{ 1 });
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("outside margins: bottom right") {
        screen(MoveCursorTo{5, 5});
        screen(InsertColumns{ 1 });
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("inside margins") {
        screen(MoveCursorTo{2, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

        SECTION("DECIC-0") {
            screen(InsertColumns{0});
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-1") {
            screen(InsertColumns{1});
            REQUIRE("12345\n67 80\nAB CE\nFG HJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-2") {
            screen(InsertColumns{2});
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }

        SECTION("DECIC-3-clamped") {
            screen(InsertColumns{3});
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
    }
}

TEST_CASE("InsertCharacters", "[screen]")
{
    Screen screen{{5, 2}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890");
    screen(SetMode{ Mode::LeftRightMargin, true });
    screen(SetLeftRightMargin{2, 4});
    REQUIRE("12345\n67890\n" == screen.renderText());

    SECTION("outside margins: left") {
        screen(MoveCursorTo{1, 1});
        screen(InsertCharacters{ 1 });
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("outside margins: right") {
        screen(MoveCursorTo{1, 5});
        screen(InsertCharacters{ 1 });
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("inside margins") {
        screen(MoveCursorTo{1, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

        SECTION("no-op") {
            screen(InsertCharacters{0});
            REQUIRE(screen.renderText() == "12345\n67890\n");
        }

        SECTION("ICH-1") {
            screen(InsertCharacters{1});
            REQUIRE(screen.renderText() == "12 35\n67890\n");
        }

        SECTION("ICH-2") {
            screen(InsertCharacters{2});
            REQUIRE(screen.renderText() == "12  5\n67890\n");
        }

        SECTION("ICH-3-clamped") {
            screen(InsertCharacters{3});
            REQUIRE(screen.renderText() == "12  5\n67890\n");
        }
    }
}

TEST_CASE("InsertLines", "[screen]")
{
    Screen screen{{4, 6}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());

    SECTION("old") {
        Screen screen{{2, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};

        screen.write("AB\r\nCD");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("CD" == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));

        screen(InsertLines{1});
        CHECK("AB" == screen.renderTextLine(1));
        CHECK("  " == screen.renderTextLine(2));
        CHECK("CD" == screen.renderTextLine(3));

        screen(MoveCursorTo{1, 1});
        screen(InsertLines{1});
        CHECK("  " == screen.renderTextLine(1));
        CHECK("AB" == screen.renderTextLine(2));
        CHECK("  " == screen.renderTextLine(3));
    }
    // TODO: test with (top/bottom and left/right) margins enabled
}

TEST_CASE("DeleteLines", "[screen]")
{
    Screen screen{{2, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};

    screen.write("AB\r\nCD\r\nEF");
    logScreenText(screen, "initial");
    REQUIRE("AB" == screen.renderTextLine(1));
    REQUIRE("CD" == screen.renderTextLine(2));
    REQUIRE("EF" == screen.renderTextLine(3));

    screen(MoveCursorTo{2, 1});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});

    SECTION("no-op") {
        screen(DeleteLines{0});
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("CD" == screen.renderTextLine(2));
        REQUIRE("EF" == screen.renderTextLine(3));
    }

    SECTION("in-range") {
        screen(DeleteLines{1});
        logScreenText(screen, "After EL(1)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("EF" == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }

    SECTION("clamped") {
        screen(MoveCursorTo{2, 2});
        screen(DeleteLines{5});
        logScreenText(screen, "After clamped EL(5)");
        REQUIRE("AB" == screen.renderTextLine(1));
        REQUIRE("  " == screen.renderTextLine(2));
        REQUIRE("  " == screen.renderTextLine(3));
    }
}

TEST_CASE("DeleteColumns", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(SetMode{ Mode::LeftRightMargin, true });
    screen(SetLeftRightMargin{2, 4});
    screen(SetTopBottomMargin{2, 4});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("outside margin") {
        screen(DeleteColumns{ 1 });
        REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("inside margin") {
        screen(MoveCursorTo{ 2, 3 });
        REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

        SECTION("DECDC-0") {
            screen(DeleteColumns{ 0 });
            REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-1") {
            screen(DeleteColumns{ 1 });
            REQUIRE("12345\n679 0\nABD E\nFGI J\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-2") {
            screen(DeleteColumns{ 2 });
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
        SECTION("DECDC-3-clamped") {
            screen(DeleteColumns{ 4 });
            REQUIRE("12345\n67  0\nAB  E\nFG  J\nKLMNO\n" == screen.renderText());
        }
    }
}

TEST_CASE("DeleteCharacters", "[screen]")
{
    Screen screen{{5, 2}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\033[1;2H");
    REQUIRE("12345\n67890\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    SECTION("outside margin") {
        screen(SetMode{ Mode::LeftRightMargin, true });
        screen(SetLeftRightMargin{ 2, 4 });
        screen(MoveCursorTo{ 1, 1 });
        screen(DeleteCharacters{ 1 });
        REQUIRE("12345\n67890\n" == screen.renderText());
    }

    SECTION("without horizontal margin") {
        SECTION("no-op") {
            screen(DeleteCharacters{ 0 });
            REQUIRE("12345\n67890\n" == screen.renderText());
        }
        SECTION("in-range-1") {
            screen(DeleteCharacters{ 1 });
            REQUIRE("1345 \n67890\n" == screen.renderText());
        }
        SECTION("in-range-2") {
            screen(DeleteCharacters{ 2 });
            REQUIRE("145  \n67890\n" == screen.renderText());
        }
        SECTION("in-range-4") {
            screen(DeleteCharacters{ 4 });
            REQUIRE("1    \n67890\n" == screen.renderText());
        }
        SECTION("clamped") {
            screen(DeleteCharacters{ 5 });
            REQUIRE("1    \n67890\n" == screen.renderText());
        }
    }
    SECTION("with horizontal margin") {
        screen(SetMode{ Mode::LeftRightMargin, true });
        screen(SetLeftRightMargin{ 1, 4 });
        screen(MoveCursorTo{ 1, 2 });
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

        SECTION("no-op") {
            screen(DeleteCharacters{ 0 });
            REQUIRE("12345\n67890\n" == screen.renderText());
        }
        SECTION("in-range-1") {
            REQUIRE("12345\n67890\n" == screen.renderText());
            screen(DeleteCharacters{ 1 });
            REQUIRE("134 5\n67890\n" == screen.renderText());
        }
        SECTION("in-range-2") {
            screen(DeleteCharacters{ 2 });
            REQUIRE("14  5\n67890\n" == screen.renderText());
        }
        SECTION("clamped") {
            screen(DeleteCharacters{ 4 });
            REQUIRE("1   5\n67890\n" == screen.renderText());
        }
    }
}

TEST_CASE("ClearScrollbackBuffer", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\r\nPQRST\033[H");
    REQUIRE("67890\nABCDE\nFGHIJ\nKLMNO\nPQRST\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    REQUIRE(1 == screen.scrollbackLines().size());
    REQUIRE("12345" == screen.renderHistoryTextLine(1));
}

TEST_CASE("EraseCharacters", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\033[H");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("ECH-0 equals ECH-1") {
        screen(EraseCharacters{0});
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-1") {
        screen(EraseCharacters{1});
        REQUIRE(" 2345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-5") {
        screen(EraseCharacters{5});
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("ECH-6-clamped") {
        screen(EraseCharacters{6});
        REQUIRE("     \n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    }
}

TEST_CASE("ScrollUp", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write("ABC\r\n");
    screen.write("DEF\r\n");
    screen.write("GHI");
    REQUIRE("ABC\nDEF\nGHI\n" == screen.renderText());

    SECTION("no-op") {
        INFO("begin:");
        screen(ScrollUp{0});
        INFO("end:");
        REQUIRE("ABC\nDEF\nGHI\n" == screen.renderText());
    }

    SECTION("by-1") {
        screen(ScrollUp{1});
        REQUIRE("DEF\nGHI\n   \n" == screen.renderText());
    }

    SECTION("by-2") {
        screen(ScrollUp{2});
        REQUIRE("GHI\n   \n   \n" == screen.renderText());
    }

    SECTION("by-3") {
        screen(ScrollUp{3});
        REQUIRE("   \n   \n   \n" == screen.renderText());
    }

    SECTION("clamped") {
        screen(ScrollUp{4});
        REQUIRE("   \n   \n   \n" == screen.renderText());
    }
}

TEST_CASE("ScrollDown", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    SECTION("scroll fully inside margins") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetLeftRightMargin{2, 4});
        screen(SetTopBottomMargin{2, 4});
        screen(SetMode{Mode::Origin, true});

        SECTION("SD 1") {
            screen(ScrollDown{1});
            CHECK("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderText());
        }

        SECTION("SD 2") {
            screen(ScrollDown{2});
            CHECK(
                "12345\n"
                "6   0\n"
                "A   E\n"
                "F789J\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 3") {
            screen(ScrollDown{3});
            CHECK(
                "12345\n"
                "6   0\n"
                "A   E\n"
                "F   J\n"
                "KLMNO\n" == screen.renderText());
        }
    }

    SECTION("vertical margins") {
        screen(SetTopBottomMargin{2, 4});
        SECTION("SD 0") {
            screen(ScrollDown{0});
            REQUIRE(
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 1") {
            screen(ScrollDown{1});
            REQUIRE(
                "12345\n"
                "     \n"
                "67890\n"
                "ABCDE\n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 3") {
            screen(ScrollDown{5});
            REQUIRE(
                "12345\n"
                "     \n"
                "     \n"
                "     \n"
                "KLMNO\n" == screen.renderText());
        }

        SECTION("SD 4 clamped") {
            screen(ScrollDown{4});
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
            screen(ScrollDown{0});
            REQUIRE(
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                "KLMNO\n" == screen.renderText());
        }
        SECTION("SD 1") {
            screen(ScrollDown{1});
            REQUIRE(
                "     \n"
                "12345\n"
                "67890\n"
                "ABCDE\n"
                "FGHIJ\n"
                == screen.renderText());
        }
        SECTION("SD 5") {
            screen(ScrollDown{5});
            REQUIRE(
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                "     \n"
                == screen.renderText());
        }
        SECTION("SD 6 clamped") {
            screen(ScrollDown{6});
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
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO\033[3;2H");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    SECTION("no-op") {
        screen(MoveCursorUp{0});
        REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
    }

    SECTION("in-range") {
        screen(MoveCursorUp{1});
        REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
    }

    SECTION("overflow") {
        screen(MoveCursorUp{5});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("with margins") {
        screen(SetTopBottomMargin{2, 4});
        screen(MoveCursorTo{3, 2});
        REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

        SECTION("in-range") {
            screen(MoveCursorUp{1});
            REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
        }

        SECTION("overflow") {
            screen(MoveCursorUp{5});
            REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
        }
    }

    SECTION("cursor already above margins") {
        screen(SetTopBottomMargin{3, 4});
        screen(MoveCursorTo{2, 3});
        screen(MoveCursorUp{1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 3});
    }
}

TEST_CASE("MoveCursorDown", "[screen]")
{
    Screen screen{{2, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("A");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // no-op
    screen(MoveCursorDown{0});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // in-range
    screen(MoveCursorDown{1});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // overflow
    screen(MoveCursorDown{5});
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
}

TEST_CASE("MoveCursorForward", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("no-op") {
        screen(MoveCursorForward{0});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    }

    SECTION("CUF-1") {
        screen(MoveCursorForward{1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("CUF-3 (to right border)") {
        screen(MoveCursorForward{screen.size().columns});
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().columns});
    }

    SECTION("CUF-overflow") {
        screen(MoveCursorForward{screen.size().columns + 1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().columns});
    }
}

TEST_CASE("MoveCursorBackward", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("ABC");
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    // no-op
    screen(MoveCursorBackward{0});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    // in-range
    screen(MoveCursorBackward{1});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    // overflow
    screen(MoveCursorBackward{5});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
}

TEST_CASE("HorizontalPositionAbsolute", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen(HorizontalPositionAbsolute{1});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen(HorizontalPositionAbsolute{3});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen(HorizontalPositionAbsolute{2});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // overflow
    screen(HorizontalPositionAbsolute{5});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3 /*clamped*/});
}

TEST_CASE("HorizontalPositionRelative", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    SECTION("no-op") {
        screen(HorizontalPositionRelative{0});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
    }

    SECTION("HPR-1") {
        screen(HorizontalPositionRelative{1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
    }

    SECTION("HPR-3 (to right border)") {
        screen(HorizontalPositionRelative{screen.size().columns});
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().columns});
    }

    SECTION("HPR-overflow") {
        screen(HorizontalPositionRelative{screen.size().columns + 1});
        REQUIRE(screen.cursorPosition() == Coordinate{1, screen.size().columns});
    }
}


TEST_CASE("MoveCursorToColumn", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen(MoveCursorToColumn{1});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen(MoveCursorToColumn{3});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen(MoveCursorToColumn{2});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // overflow
    screen(MoveCursorToColumn{5});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3 /*clamped*/});
}

TEST_CASE("MoveCursorToLine", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // no-op
    screen(MoveCursorToLine{});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    // in-range
    screen(MoveCursorToLine{3});
    REQUIRE(screen.cursorPosition() == Coordinate{3, 1});

    screen(MoveCursorToLine{2});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});

    // overflow
    screen(MoveCursorToLine{5});
    REQUIRE(screen.cursorPosition() == Coordinate{3, 1/*clamped*/});
}

TEST_CASE("MoveCursorToBeginOfLine", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};

    screen.write("\r\nAB");
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    screen(MoveCursorToBeginOfLine{});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
}

TEST_CASE("MoveCursorTo", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    SECTION("origin mode disabled") {
        SECTION("in range") {
            screen(MoveCursorTo{3, 2});
            REQUIRE(screen.cursorPosition() == Coordinate{3, 2});
        }

        SECTION("origin") {
            screen(MoveCursorTo{1, 1});
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }

        SECTION("clamped") {
            screen(MoveCursorTo{6, 7});
            REQUIRE(screen.cursorPosition() == Coordinate{5, 5});
        }
    }

    SECTION("origin-mode enabled") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetLeftRightMargin{2, 4});
        screen(SetTopBottomMargin{2, 4});
        screen(SetMode{Mode::Origin, true});

        SECTION("move to origin") {
            screen(MoveCursorTo{1, 1});
            CHECK(Coordinate{1, 1} == screen.cursorPosition());
            CHECK(Coordinate{2, 2} == screen.realCursorPosition());
            CHECK('7' == (char)screen.withOriginAt(1, 1).character);
            CHECK('I' == (char)screen.withOriginAt(3, 3).character);
        }
    }
}

TEST_CASE("MoveCursorToNextTab", "[screen]")
{
    auto constexpr TabWidth = 8;
    Screen screen{{20, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen(MoveCursorToNextTab{});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 1 * TabWidth + 1});

    screen(MoveCursorToNextTab{});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2 * TabWidth + 1});

    screen(MoveCursorToNextTab{});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 20});

    screen(SetMode{Mode::AutoWrap, true});
    screen.write("A"); // 'A' is being written at the right margin
    screen.write("B"); // force wrap to next line, writing 'B' at the beginning of the line

    screen(MoveCursorToNextTab{});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 9});
}

// TODO: HideCursor
// TODO: ShowCursor

TEST_CASE("SaveCursor and RestoreCursor", "[screen]")
{
    Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen(SetMode{Mode::AutoWrap, false});
    screen(SaveCursor{});

    screen(MoveCursorTo{3, 3});
    screen(SetMode{Mode::AutoWrap, true});
    screen(SetMode{Mode::Origin, true});

    screen(RestoreCursor{});
    CHECK(screen.cursorPosition() == Coordinate{1, 1});
    CHECK(screen.isModeEnabled(Mode::AutoWrap) == false);
    CHECK(screen.isModeEnabled(Mode::Origin) == false);
}

TEST_CASE("Index_outside_margin", "[screen]")
{
    Screen screen{{4, 6}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("1234\r\n5678\r\nABCD\r\nEFGH\r\nIJKL\r\nMNOP");
    logScreenText(screen, "initial");
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    screen(SetTopBottomMargin{2, 4});

    // with cursor above top margin
    screen(MoveCursorTo{1, 3});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 3});

    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    // with cursor below bottom margin and above bottom screen (=> only moves cursor one down)
    screen(MoveCursorTo{5, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{6, 3});

    // with cursor below bottom margin and at bottom screen (=> no-op)
    screen(MoveCursorTo{6, 3});
    screen(Index{});
    REQUIRE("1234\n5678\nABCD\nEFGH\nIJKL\nMNOP\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{6, 3});
}

TEST_CASE("Index_inside_margin", "[screen]")
{
    Screen screen{{2, 6}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("11\r\n22\r\n33\r\n44\r\n55\r\n66");
    logScreenText(screen, "initial setup");

    // test IND when cursor is within margin range (=> move cursor down)
    screen(SetTopBottomMargin{2, 4});
    screen(MoveCursorTo{3, 2});
    screen(Index{});
    logScreenText(screen, "IND while cursor at line 3");
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
    REQUIRE("11\n22\n33\n44\n55\n66\n" == screen.renderText());
}

TEST_CASE("Index_at_bottom_margin", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial setup");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    SECTION("cursor at bottom margin and full horizontal margins") {
        screen(MoveCursorTo{4, 2});
        screen(Index{});
        logScreenText(screen, "IND while cursor at bottom margin");
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
        REQUIRE("12345\nABCDE\nFGHIJ\n     \nKLMNO\n" == screen.renderText());
    }

    SECTION("cursor at bottom margin and NOT full horizontal margins") {
        screen(MoveCursorTo{1, 1});
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetLeftRightMargin{2, 4});
        screen(SetTopBottomMargin{2, 4});
        screen(MoveCursorTo{4, 2}); // cursor at bottom margin
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

        screen(Index{});
        CHECK("12345\n6BCD0\nAGHIE\nF   J\nKLMNO\n" == screen.renderText());
        REQUIRE(screen.cursorPosition() == Coordinate{4, 2});
    }
}

TEST_CASE("ReverseIndex_without_custom_margins", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    // at bottom screen
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    screen(ReverseIndex{});
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen(ReverseIndex{});
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    screen(ReverseIndex{});
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen(ReverseIndex{});
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n12345\n67890\nABCDE\nFGHIJ\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    screen(ReverseIndex{});
    logScreenText(screen, "RI at top screen");
    REQUIRE("     \n     \n12345\n67890\nABCDE\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ReverseIndex_with_vertical_margin", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetTopBottomMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    logScreenText(screen, "RI below bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    // at bottom margin
    screen(ReverseIndex{});
    logScreenText(screen, "RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen(ReverseIndex{});
    logScreenText(screen, "RI middle margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin
    screen(ReverseIndex{});
    logScreenText(screen, "RI at top margin #1");
    REQUIRE("12345\n     \n67890\nABCDE\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin (again)
    screen(ReverseIndex{});
    logScreenText(screen, "RI at top margin #2");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    logScreenText(screen, "RI above top margin");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});

    // above top margin (top screen) => no-op
    screen(ReverseIndex{});
    logScreenText(screen, "RI above top margin (top-screen)");
    REQUIRE("12345\n     \n     \n67890\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ReverseIndex_with_vertical_and_horizontal_margin", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    logScreenText(screen, "initial");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    screen(SetMode{Mode::LeftRightMargin, true});
    screen(SetLeftRightMargin{2, 4});
    screen(SetTopBottomMargin{2, 4});

    // below bottom margin
    screen(MoveCursorTo{5, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{4, 2});

    // at bottom margin
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at bottom margin");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{3, 2});

    screen(ReverseIndex{});
    logScreenText(screen, "after RI at bottom margin (again)");
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // at top margin
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at top margin");
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});
    REQUIRE("12345\n6   0\nA789E\nFBCDJ\nKLMNO\n" == screen.renderText());

    // at top margin (again)
    screen(ReverseIndex{});
    logScreenText(screen, "after RI at top margin (again)");
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 2});

    // above top margin
    screen(MoveCursorTo{1, 2});
    screen(ReverseIndex{});
    REQUIRE("12345\n6   0\nA   E\nF789J\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{1, 2});
}

TEST_CASE("ScreenAlignmentPattern", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(SetTopBottomMargin{2, 4});
    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());

    REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

    REQUIRE(2 == screen.margin().vertical.from);
    REQUIRE(4 == screen.margin().vertical.to);

    SECTION("test") {
        screen(ScreenAlignmentPattern{});
        REQUIRE("XXXXX\nXXXXX\nXXXXX\nXXXXX\nXXXXX\n" == screen.renderText());

        REQUIRE(screen.cursorPosition() == Coordinate{1, 1});

        REQUIRE(1 == screen.margin().horizontal.from);
        REQUIRE(5 == screen.margin().horizontal.to);
        REQUIRE(1 == screen.margin().vertical.from);
        REQUIRE(5 == screen.margin().vertical.to);
    }
}

TEST_CASE("CursorNextLine", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(MoveCursorTo{2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("without margins") {
        SECTION("normal") {
            screen(CursorNextLine{1});
            REQUIRE(screen.cursorPosition() == Coordinate{3, 1});
        }

        SECTION("clamped") {
            screen(CursorNextLine{5});
            REQUIRE(screen.cursorPosition() == Coordinate{5, 1});
        }
    }

    SECTION("with margins") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetLeftRightMargin{2, 4});
        screen(SetTopBottomMargin{2, 4});
        screen(SetMode{Mode::Origin, true});
        screen(MoveCursorTo{1, 2});

        SECTION("normal-1") {
            screen(CursorNextLine{1});
            REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
        }

        SECTION("normal-2") {
            screen(CursorNextLine{2});
            REQUIRE(screen.cursorPosition() == Coordinate{3, 1});
        }

        SECTION("clamped") {
            screen(CursorNextLine{3});
            REQUIRE(screen.cursorPosition() == Coordinate{3, 1});
        }
    }
}

TEST_CASE("CursorPreviousLine", "[screen]")
{
    Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{5, 5});

    SECTION("without margins") {
        SECTION("normal") {
            screen(CursorPreviousLine{1});
            REQUIRE(screen.cursorPosition() == Coordinate{4, 1});
        }

        SECTION("clamped") {
            screen(CursorPreviousLine{5});
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }
    }

    SECTION("with margins") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetLeftRightMargin{2, 4});
        screen(SetTopBottomMargin{2, 4});
        screen(SetMode{Mode::Origin, true});
        screen(MoveCursorTo{3, 3});
        REQUIRE(screen.cursorPosition() == Coordinate{3, 3});

        SECTION("normal-1") {
            screen(CursorPreviousLine{1});
            REQUIRE(screen.cursorPosition() == Coordinate{2, 1});
        }

        SECTION("normal-2") {
            screen(CursorPreviousLine{2});
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }

        SECTION("clamped") {
            screen(CursorPreviousLine{3});
            REQUIRE(screen.cursorPosition() == Coordinate{1, 1});
        }
    }
}

TEST_CASE("ReportCursorPosition", "[screen]")
{
    string reply;
    Screen screen{
        {5, 5},
        nullopt, // history line count (infinite)
        {}, // useAppCursorKeys
        {}, // onWindowTitleChanged
        {}, // resizeWindow
        {}, // setAppKeypadMode,
        {}, // setBracketedPaste
        {}, // setMouseProtocol
        {}, // setMouseTransport
        {}, // setMouseAlternateScroll
        {}, // setCursorStyle
        [&](auto const& _reply) { reply += _reply; },
        [&](auto const& _msg) { UNSCOPED_INFO(fmt::format("{}", _msg)); }
    };
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(MoveCursorTo{2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE("" == reply);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("with Origin mode disabled") {
        screen(ReportCursorPosition{});
        CHECK("\033[2;3R" == reply);
    }

    SECTION("with margins and origin mode enabled") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetTopBottomMargin{2, 4});
        screen(SetLeftRightMargin{2, 4});
        screen(SetMode{Mode::Origin, true});
        screen(MoveCursorTo{3, 2});

        screen(ReportCursorPosition{});
        CHECK("\033[3;2R" == reply);
    }
}

TEST_CASE("ReportExtendedCursorPosition", "[screen]")
{
    string reply;
    Screen screen{
        {5, 5},
        nullopt, // history line count (infinite)
        {}, // useAppCursorKeys
        {}, // onWindowTitleChanged
        {}, // resizeWindow
        {}, // setAppKeypadMode,
        {}, // setBracketedPaste
        {}, // setMouseProtocol
        {}, // setMouseTransport
        {}, // setMouseAlternateScroll
        {}, // setCursorStyle
        [&](auto const& _reply) { reply += _reply; },
        [&](auto const& _msg) { UNSCOPED_INFO(fmt::format("{}", _msg)); }
    };
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");
    screen(MoveCursorTo{2, 3});

    REQUIRE("12345\n67890\nABCDE\nFGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE("" == reply);
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    SECTION("with Origin mode disabled") {
        screen(ReportExtendedCursorPosition{});
        CHECK("\033[2;3;1R" == reply);
    }

    SECTION("with margins and origin mode enabled") {
        screen(SetMode{Mode::LeftRightMargin, true});
        screen(SetTopBottomMargin{2, 4});
        screen(SetLeftRightMargin{2, 4});
        screen(SetMode{Mode::Origin, true});
        screen(MoveCursorTo{3, 2});

        screen(ReportExtendedCursorPosition{});
        CHECK("\033[3;2;1R" == reply);
    }
}

TEST_CASE("SetMode", "[screen]") {
    SECTION("Auto NewLine Mode: Enabled") {
        Screen screen{{5, 5}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
        screen(SetMode{Mode::AutomaticNewLine, true});
        screen.write("12345\n67890\nABCDE\nFGHIJ\nKLMNO");
        REQUIRE(screen.renderText() == "12345\n67890\nABCDE\nFGHIJ\nKLMNO\n");
    }

    SECTION("Auto NewLine Mode: Disabled") {
        Screen screen{{3, 3}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
        screen.write("A\nB\nC");
        REQUIRE(screen.renderText() == "A  \n B \n  C\n");
    }
}

TEST_CASE("RequestMode", "[screen]")
{
    string reply;
    Screen screen{
        {5, 5},
        nullopt, // history line count (infinite)
        {}, // useAppCursorKeys
        {}, // onWindowTitleChanged
        {}, // resizeWindow
        {}, // setAppKeypadMode,
        {}, // setBracketedPaste
        {}, // setMouseProtocol
        {}, // setMouseTransport
        {}, // setMouseAlternateScroll
        {}, // setCursorStyle
        [&](auto const& _reply) { reply += _reply; },
        [&](auto const& _msg) { UNSCOPED_INFO(fmt::format("{}", _msg)); }
    };

    SECTION("ANSI modes") {
        screen(SetMode{Mode::Insert, true}); // IRM
        screen(RequestMode{Mode::Insert});
        REQUIRE(reply == fmt::format("\033[{};1$y", to_code(Mode::Insert)));
    }

    SECTION("DEC modes") {
        screen(SetMode{Mode::Origin, true}); // DECOM
        screen(RequestMode{Mode::Origin});
        REQUIRE(reply == fmt::format("\033[?{};1$y", to_code(Mode::Origin)));
    }
}

TEST_CASE("peek into history", "[screen]")
{
    Screen screen{{3, 2}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("123\r\n456\r\nABC\r\nDEF");

    REQUIRE("ABC\nDEF\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 3});

    // first line in history
    CHECK(screen.absoluteAt({1, 1}).character == '1');
    CHECK(screen.absoluteAt({1, 2}).character == '2');
    CHECK(screen.absoluteAt({1, 3}).character == '3');

    // second line in history
    CHECK(screen.absoluteAt({2, 1}).character == '4');
    CHECK(screen.absoluteAt({2, 2}).character == '5');
    CHECK(screen.absoluteAt({2, 3}).character == '6');

    // first line on screen buffer
    CHECK(screen.absoluteAt({3, 1}).character == 'A');
    CHECK(screen.absoluteAt({3, 2}).character == 'B');
    CHECK(screen.absoluteAt({3, 3}).character == 'C');

    // second line on screen buffer
    CHECK(screen.absoluteAt({4, 1}).character == 'D');
    CHECK(screen.absoluteAt({4, 2}).character == 'E');
    CHECK(screen.absoluteAt({4, 3}).character == 'F');

    // too big row number
    CHECK_THROWS(screen.absoluteAt({5, 1}));
}

TEST_CASE("render into history", "[screen]")
{
    Screen screen{{5, 2}, [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};
    screen.write("12345\r\n67890\r\nABCDE\r\nFGHIJ\r\nKLMNO");

    REQUIRE("FGHIJ\nKLMNO\n" == screen.renderText());
    REQUIRE(screen.cursorPosition() == Coordinate{2, 5});

    string renderedText;
    renderedText.resize(2 * 6);
    auto const renderer = [&](auto rowNumber, auto columnNumber, Screen::Cell const& cell) {
        renderedText[(rowNumber - 1) * 6 + (columnNumber - 1)] = static_cast<char>(cell.character);
        if (columnNumber == 5)
            renderedText[(rowNumber - 1) * 6 + (columnNumber)] = '\n';
    };

    SECTION("main area") {
        screen.render(renderer, 0);
        REQUIRE("FGHIJ\nKLMNO\n" == screen.renderText());
    }

    SECTION("1 line into history") {
        screen.render(renderer, 1);
        REQUIRE("ABCDE\nFGHIJ\n" == renderedText);
    }

    SECTION("2 lines into history") {
        screen.render(renderer, 2);
        REQUIRE("67890\nABCDE\n" == renderedText);
    }

    SECTION("3 lines into history") {
        screen.render(renderer, 3);
        REQUIRE("12345\n67890\n" == renderedText);
    }

    SECTION("4 lines into history (1 clamped)") {
        screen.render(renderer, 4);
        REQUIRE("12345\n67890\n" == renderedText);
    }
}

TEST_CASE("HorizontalTabClear.AllTabs", "[screen]")
{
    Screen screen{{5, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(HorizontalTabClear{HorizontalTabClear::AllTabs});

    screen(AppendChar{'X'});
    screen(MoveCursorToNextTab{});
    screen(AppendChar{'Y'});
    REQUIRE("X   Y" == screen.renderTextLine(1));

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'Z'});
    REQUIRE("X   Y" == screen.renderTextLine(1));
    REQUIRE("Z    " == screen.renderTextLine(2));

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'A'});
    REQUIRE("X   Y" == screen.renderTextLine(1));
    REQUIRE("Z   A" == screen.renderTextLine(2));
}

TEST_CASE("HorizontalTabClear.UnderCursor", "[screen]")
{
    Screen screen{{10, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.setTabWidth(4);

    // clear tab at column 4
    screen(MoveCursorTo{1, 4});
    screen(HorizontalTabClear{HorizontalTabClear::UnderCursor});

    screen(MoveCursorTo{1, 1});
    screen(AppendChar{'A'});
    screen(MoveCursorToNextTab{});
    screen(AppendChar{'B'});

    //       1234567890
    REQUIRE("A      B  " == screen.renderTextLine(1));
    REQUIRE("          " == screen.renderTextLine(2));

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'C'});
    CHECK("A      B C" == screen.renderTextLine(1));
    CHECK("          " == screen.renderTextLine(2));
}

TEST_CASE("HorizontalTabSet", "[screen]")
{
    Screen screen{{10, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen(HorizontalTabClear{HorizontalTabClear::AllTabs});

    screen(MoveCursorToColumn{3});
    screen(HorizontalTabSet{});

    screen(MoveCursorToColumn{5});
    screen(HorizontalTabSet{});

    screen(MoveCursorToColumn{8});
    screen(HorizontalTabSet{});

    screen(MoveCursorToBeginOfLine{});

    screen(AppendChar{'1'});

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'3'});

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'5'});

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'8'});

    screen(MoveCursorToNextTab{}); // capped
    screen(AppendChar{'A'});       // writes B at right margin, flags for autowrap

    REQUIRE("1 3 5  8 A" == screen.renderTextLine(1));

    screen(MoveCursorToNextTab{});  // wrapped
    screen(AppendChar{'B'});        // writes B at left margin

    //       1234567890
    REQUIRE("1 3 5  8 A" == screen.renderTextLine(1));
    screen(MoveCursorToNextTab{});  // 1 -> 3 (overflow)
    screen(MoveCursorToNextTab{});  // 3 -> 5
    screen(MoveCursorToNextTab{});  // 5 -> 8
    screen(AppendChar{'C'});

    //     1234567890
    CHECK("1 3 5  8 A" == screen.renderTextLine(1));
    CHECK("B      C  " == screen.renderTextLine(2));
}

TEST_CASE("CursorBackwardTab.fixedTabWidth", "[screen]")
{
    Screen screen{{10, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.setTabWidth(4); // 5, 9

    screen(AppendChar{'a'});

    screen(MoveCursorToNextTab{}); // -> 5
    screen(AppendChar{'b'});

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'c'});       // -> 9

    //      "1234567890"
    REQUIRE("a   b   c " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition().column == 10);

    SECTION("oveflow") {
        screen(CursorBackwardTab{4});
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen(AppendChar{'X'});
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("exact") {
        screen(CursorBackwardTab{3});
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("inside 2") {
        screen(CursorBackwardTab{2});
        CHECK(screen.cursorPosition() == Coordinate{1, 5});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("a   X   c " == screen.renderTextLine(1));
    }

    SECTION("inside 1") {
        screen(CursorBackwardTab{1});
        CHECK(screen.cursorPosition() == Coordinate{1, 9});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("a   b   X " == screen.renderTextLine(1));
    }

    SECTION("no op") {
        screen(CursorBackwardTab{0});
        CHECK(screen.cursorPosition() == Coordinate{1, 10});
    }
}

TEST_CASE("CursorBackwardTab.manualTabs", "[screen]")
{
    Screen screen{{10, 3}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};

    screen(MoveCursorToColumn{5});
    screen(HorizontalTabSet{});
    screen(MoveCursorToColumn{9});
    screen(HorizontalTabSet{});
    screen(MoveCursorToBeginOfLine{});

    screen(AppendChar{'a'});

    screen(MoveCursorToNextTab{}); // -> 5
    screen(AppendChar{'b'});

    screen(MoveCursorToNextTab{});
    screen(AppendChar{'c'});       // -> 9

    //      "1234567890"
    REQUIRE("a   b   c " == screen.renderTextLine(1));
    REQUIRE(screen.cursorPosition().column == 10);

    SECTION("oveflow") {
        screen(CursorBackwardTab{4});
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen(AppendChar{'X'});
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("exact") {
        screen(CursorBackwardTab{3});
        CHECK(screen.cursorPosition() == Coordinate{1, 1});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("X   b   c " == screen.renderTextLine(1));
    }

    SECTION("inside 2") {
        screen(CursorBackwardTab{2});
        CHECK(screen.cursorPosition() == Coordinate{1, 5});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("a   X   c " == screen.renderTextLine(1));
    }

    SECTION("inside 1") {
        screen(CursorBackwardTab{1});
        CHECK(screen.cursorPosition() == Coordinate{1, 9});
        screen(AppendChar{'X'});
        //    "1234567890"
        CHECK("a   b   X " == screen.renderTextLine(1));
    }

    SECTION("no op") {
        screen(CursorBackwardTab{0});
        CHECK(screen.cursorPosition() == Coordinate{1, 10});
    }
}

TEST_CASE("findNextMarker", "[screen]")
{
    auto screen = Screen{{4, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};

    REQUIRE_FALSE(screen.findPrevMarker(0).has_value());

    SECTION("no marks") {
        screen.write("1abc\r\n"s);
        screen.write("2def\r\n"s);
        screen.write("3ghi\r\n"s);
        screen.write("4jkl\r\n"s);
        screen.write("5mno\r\n"s);

        CHECK(screen.findNextMarker(0).value() == 0);
        CHECK(screen.findNextMarker(1).value() == 0);
        CHECK(screen.findNextMarker(2).value() == 0);
        CHECK(screen.findNextMarker(3).value() == 0);
        CHECK(screen.findNextMarker(4).value() == 0);
        CHECK(screen.findNextMarker(5).value() == 0);
    }

    SECTION("with marks") {
        screen.write(SetMark{});
        screen.write("1abc\r\n"s);
        screen.write("2def\r\n"s);
        screen.write(SetMark{});
        screen.write(SetMark{});
        screen.write("3ghi\r\n"s);
        screen.write(SetMark{});
        screen.write("4jkl\r\n"s);
        screen.write("5mno\r\n"s);

        REQUIRE(screen.renderTextLine(1) == "5mno");
        REQUIRE(screen.renderTextLine(2) == "    ");

        CHECK(screen.findNextMarker(0).value() == 0);

        CHECK(screen.findNextMarker(1).has_value());
        CHECK(screen.findNextMarker(1).value() == 0); // 3ghi

        CHECK(screen.findNextMarker(2).has_value());
        CHECK(screen.findNextMarker(2).value() == 1); // 2def

        CHECK(screen.findNextMarker(4).has_value());
        CHECK(screen.findNextMarker(4).value() == 2); // 2def
    }
}

TEST_CASE("findPrevMarker", "[screen]")
{
    auto screen = Screen{{4, 2}, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};

    REQUIRE_FALSE(screen.findPrevMarker(0).has_value());

    SECTION("no marks") {
        screen.write("1abc\r\n"s);
        screen.write("2def\r\n"s);
        screen.write("3ghi\r\n"s);
        screen.write("4jkl\r\n"s);
        screen.write("5mno\r\n"s);

        REQUIRE_FALSE(screen.findPrevMarker(0).has_value());

        // being a little beyond history line count
        REQUIRE_FALSE(screen.findPrevMarker(1).has_value());
    }

    SECTION("with marks") {
        screen.write(SetMark{});
        screen.write("1abc\r\n"s);
        screen.write("2def\r\n"s);
        screen.write(SetMark{});
        screen.write(SetMark{});
        screen.write("3ghi\r\n"s);
        screen.write(SetMark{});
        screen.write("4jkl\r\n"s);
        screen.write("5mno\r\n"s);

        REQUIRE(screen.renderTextLine(1) == "5mno");
        REQUIRE(screen.renderTextLine(2) == "    ");

        REQUIRE(screen.findPrevMarker(0).has_value());
        REQUIRE(screen.findPrevMarker(0).value() == 1); // 4jkl

        REQUIRE(screen.findPrevMarker(1).has_value());
        REQUIRE(screen.findPrevMarker(1).value() == 2); // 3ghi

        REQUIRE(screen.findPrevMarker(2).has_value());
        REQUIRE(screen.findPrevMarker(2).value() == 4); // 2def

        REQUIRE_FALSE(screen.findPrevMarker(4).has_value());
    }
}

TEST_CASE("DECTABSR", "[screen]")
{
    string reply;
    auto screen = Screen{
        {35, 2},
        nullopt, // history line count (infinite)
        {}, // useAppCursorKeys
        {}, // onWindowTitleChanged
        {}, // resizeWindow
        {}, // setAppKeypadMode,
        {}, // setBracketedPaste
        {}, // setMouseProtocol
        {}, // setMouseTransport
        {}, // setMouseAlternateScroll
        {}, // setCursorStyle
        [&](auto const& _reply) { reply += _reply; },
        [&](auto const& _msg) { UNSCOPED_INFO(fmt::format("{}", _msg)); }
    };

    SECTION("default tabstops") {
        screen.write(RequestTabStops{});
        CHECK(reply == "\033P2$u9/17/25/33\x5c");
    }

    SECTION("cleared tabs") {
        screen.write(HorizontalTabClear{HorizontalTabClear::AllTabs});
        screen.write(RequestTabStops{});
        CHECK(reply == "\033P2$u\x5c");
    }

    SECTION("custom tabstops") {
        screen.write(HorizontalTabClear{HorizontalTabClear::AllTabs});

        screen.write(MoveCursorToColumn{2});
        screen.write(HorizontalTabSet{});

        screen.write(MoveCursorToColumn{4});
        screen.write(HorizontalTabSet{});

        screen.write(MoveCursorToColumn{8});
        screen.write(HorizontalTabSet{});

        screen.write(MoveCursorToColumn{16});
        screen.write(HorizontalTabSet{});

        screen.write(RequestTabStops{});
        CHECK(reply == "\033P2$u2/4/8/16\x5c");
    }
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
