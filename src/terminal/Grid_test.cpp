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
#include <terminal/Grid.h>
#include <terminal/Parser.h>
#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <iostream>

using namespace terminal;
using namespace std::string_view_literals;
using std::string;
using crispy::Size;

namespace // {{{ helper
{
    void logGridText(Grid const& _grid, string const& _headline = "")
    {
        UNSCOPED_INFO(fmt::format("Grid.dump({}, {}): {}\n", _grid.historyLineCount(), _grid.screenSize(), _headline));

        for (int row = 0; row < _grid.historyLineCount() + _grid.screenSize().height; ++row)
        {
            UNSCOPED_INFO(fmt::format(
                "{}: \"{}\" {}\n",
                row,
                _grid.renderTextLine(row - _grid.historyLineCount() + 1),
                _grid.absoluteLineAt(row).flags()
            ));
        }
    }

    Grid setupGrid5x2()
    {
        auto grid = Grid(Size{5, 2}, true, std::nullopt);
        grid.lineAt(1).setText("ABCDE");
        grid.lineAt(2).setText("abcde");
        logGridText(grid, "setup grid at 5x2");
        return grid;
    }

    Grid setupGrid8x2()
    {
        auto grid = Grid(Size{8, 2}, true, std::nullopt);
        grid.lineAt(1).setText("ABCDEFGH");
        grid.lineAt(2).setText("abcdefgh");
        logGridText(grid, "setup grid at 5x2");
        return grid;
    }
} // }}}

TEST_CASE("Line.reflow.unwrappable", "[grid]")
{
    auto line = Line(5, "ABCDE"sv, Line::Flags::None);
    REQUIRE(!line.wrappable());
    REQUIRE(!line.wrapped());

    auto const reflowed = line.reflow(3);
    CHECK(!line.wrapped());
    CHECK(line.size() == 3);
    CHECK(line.toUtf8() == "ABC");
    CHECK(reflowed.size() == 0);
}

TEST_CASE("Line.reflow.wrappable", "[grid]")
{
    auto line = Line(5, "ABCDE"sv, Line::Flags::Wrappable);
    REQUIRE(line.wrappable());
    REQUIRE(!line.wrapped());

    auto const reflowed = Line(line.reflow(3), line.inheritableFlags() | Line::Flags::Wrapped);
    CHECK(!line.wrapped());
    CHECK(line.size() == 3);
    CHECK(line.toUtf8() == "ABC");
    CHECK(reflowed.size() == 2);
    CHECK(reflowed.toUtf8() == "DE");
}

TEST_CASE("Line.reflow.empty", "[grid]")
{
    auto line = Line(5, Cell{}, Line::Flags::Wrappable);
    REQUIRE(!line.wrapped());
    REQUIRE(line.size() == 5);
    REQUIRE(line.toUtf8() == "     ");

    auto const reflowed = Line(line.reflow(3), line.flags());
    CHECK(!line.wrapped());
    CHECK(line.size() == 3);
    CHECK(line.toUtf8() == "   ");
    CHECK(reflowed.size() == 0);
    CHECK(reflowed.toUtf8() == "");
}

TEST_CASE("Grid.reflow.shrink.wrappable", "[grid]")
{
    auto grid = Grid(Size{4, 4}, true, std::nullopt);
    grid.lineAt(1).setText("ABCD");
    grid.lineAt(1).setWrappable(true);
    grid.lineAt(2).setWrappable(false);
    grid.lineAt(3).setWrappable(false);
    grid.lineAt(4).setWrappable(false);
    logGridText(grid, "initial");

    (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize");

    // TODO: test output
    //CHECK(false);
}

TEST_CASE("Grid.reflow.shrink", "[grid]")
{
    auto grid = setupGrid5x2();

    // Shrink slowly from 5x2 to 4x2 to 3x2 to 2x2.

    // 4x2
    (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize");

    CHECK(grid.historyLineCount() == 2);
    CHECK(grid.renderTextLine(-1) == "ABCD");
    CHECK(grid.renderTextLine(0)  == "E   ");

    CHECK(grid.screenSize() == Size{4, 2});
    CHECK(grid.renderTextLine(1) == "abcd");
    CHECK(grid.renderTextLine(2) == "e   ");

    // 3x2
    (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize 3x2");

    CHECK(grid.historyLineCount() == 2);
    CHECK(grid.renderTextLine(-1) == "ABC");
    CHECK(grid.renderTextLine(0)  == "DE ");

    CHECK(grid.screenSize() == Size{3, 2});
    CHECK(grid.renderTextLine(1) == "abc");
    CHECK(grid.renderTextLine(2) == "de ");

    // 2x2
    (void) grid.resize(Size{2, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.historyLineCount() == 4);
    CHECK(grid.renderTextLine(-3) == "AB");
    CHECK(grid.renderTextLine(-2) == "CD");
    CHECK(grid.renderTextLine(-1) == "E ");
    CHECK(grid.renderTextLine(0)  == "ab");

    CHECK(grid.screenSize() == Size{2, 2});
    CHECK(grid.renderTextLine(1) == "cd");
    CHECK(grid.renderTextLine(2) == "e ");
}

TEST_CASE("Grid.reflow", "[grid]")
{
    auto grid = setupGrid5x2();

    SECTION("resize 4x2") {
        (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize");

        CHECK(grid.historyLineCount() == 2);
        CHECK(grid.renderTextLine(-1) == "ABCD");
        CHECK(grid.renderTextLine(0)  == "E   ");

        CHECK(grid.screenSize() == Size{4, 2});
        CHECK(grid.renderTextLine(1) == "abcd");
        CHECK(grid.renderTextLine(2) == "e   ");
    }

    SECTION("resize 3x2") {
        (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == 2);
        CHECK(grid.renderTextLine(-1) == "ABC");
        CHECK(grid.renderTextLine(0)  == "DE ");

        CHECK(grid.screenSize() == Size{3, 2});
        CHECK(grid.renderTextLine(1) == "abc");
        CHECK(grid.renderTextLine(2) == "de ");
    }

    SECTION("resize 2x2") {
        (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(Size{2, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 2x2");

        CHECK(grid.historyLineCount() == 4);
        CHECK(grid.renderTextLine(-3) == "AB");
        CHECK(grid.renderTextLine(-2) == "CD");
        CHECK(grid.renderTextLine(-1) == "E ");
        CHECK(grid.renderTextLine(0)  == "ab");

        CHECK(grid.screenSize() == Size{2, 2});
        CHECK(grid.renderTextLine(1) == "cd");
        CHECK(grid.renderTextLine(2) == "e ");

        SECTION("regrow 3x2") {
            (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
            logGridText(grid, "after regrow 3x2");

            CHECK(grid.historyLineCount() == 2);
            CHECK(grid.renderTextLine(-1) == "ABC");
            CHECK(grid.renderTextLine(0)  == "DE ");

            CHECK(grid.screenSize() == Size{3, 2});
            CHECK(grid.renderTextLine(1) == "abc");
            CHECK(grid.renderTextLine(2) == "de ");

            SECTION("regrow 4x2") {
                (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
                logGridText(grid, "after regrow 4x2");

                CHECK(grid.historyLineCount() == 2);
                CHECK(grid.renderTextLine(-1) == "ABCD");
                CHECK(grid.renderTextLine(0)  == "E   ");

                CHECK(grid.screenSize() == Size{4, 2});
                CHECK(grid.renderTextLine(1) == "abcd");
                CHECK(grid.renderTextLine(2) == "e   ");
            }

            SECTION("regrow 5x2") {
                (void) grid.resize(Size{5, 2}, Coordinate{1, 1}, false);
                logGridText(grid, "after regrow 5x2");

                CHECK(grid.historyLineCount() == 0);
                CHECK(grid.screenSize() == Size{5, 2});
                CHECK(grid.renderTextLine(1) == "ABCDE");
                CHECK(grid.renderTextLine(2) == "abcde");
            }
        }
    }
}

TEST_CASE("Grid.reflow.shrink_many", "[grid]")
{
    auto grid = setupGrid5x2();
    REQUIRE(grid.screenSize() == Size{5, 2});
    REQUIRE(grid.renderTextLine(1) == "ABCDE"sv);
    REQUIRE(grid.renderTextLine(2) == "abcde"sv);

    (void) grid.resize(Size{2, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize 2x2");

    CHECK(grid.historyLineCount() == 4);
    CHECK(grid.renderTextLine(-3) == "AB");
    CHECK(grid.renderTextLine(-2) == "CD");
    CHECK(grid.renderTextLine(-1) == "E ");
    CHECK(grid.renderTextLine(0)  == "ab");

    CHECK(grid.screenSize() == Size{2, 2});
    CHECK(grid.renderTextLine(1) == "cd");
    CHECK(grid.renderTextLine(2) == "e ");
}

TEST_CASE("Grid.reflow.shrink_many_grow_many", "[grid]")
{
    auto grid = setupGrid5x2();

    (void) grid.resize(Size{2, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize 2x2");

    SECTION("smooth regrow 2->3->4->5") {
        (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 3x2");
        (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 4x2");
        (void) grid.resize(Size{5, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == 0);
        CHECK(grid.screenSize() == Size{5, 2});
        CHECK(grid.renderTextLine(1) == "ABCDE");
        CHECK(grid.renderTextLine(2) == "abcde");
    }

    SECTION("hard regrow 2->5") {
        (void) grid.resize(Size{5, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 5x2");

        CHECK(grid.historyLineCount() == 0);
        CHECK(grid.screenSize() == Size{5, 2});
        CHECK(grid.renderTextLine(1) == "ABCDE");
        CHECK(grid.renderTextLine(2) == "abcde");
    }
}

TEST_CASE("Grid.reflow.tripple", "[grid]")
{
    // Tests reflowing text upon shrink/grow across more than two (e.g. three) wrapped lines.
    auto grid = setupGrid8x2();

    (void) grid.resize(Size{2, 2}, Coordinate{1, 1}, false);
    logGridText(grid, "after resize 3x2");

    REQUIRE(grid.historyLineCount() == 6);
    REQUIRE(grid.screenSize() == Size{2, 2});

    REQUIRE(grid.renderTextLineAbsolute(0) == "AB");
    REQUIRE(grid.renderTextLineAbsolute(1) == "CD");
    REQUIRE(grid.renderTextLineAbsolute(2) == "EF");
    REQUIRE(grid.renderTextLineAbsolute(3) == "GH");

    REQUIRE(grid.renderTextLineAbsolute(4) == "ab");
    REQUIRE(grid.renderTextLineAbsolute(5) == "cd");
    REQUIRE(grid.renderTextLineAbsolute(6) == "ef");
    REQUIRE(grid.renderTextLineAbsolute(7) == "gh");

    REQUIRE(!grid.absoluteLineAt(0).wrapped());
    REQUIRE(grid.absoluteLineAt(1).wrapped());
    REQUIRE(grid.absoluteLineAt(2).wrapped());
    REQUIRE(grid.absoluteLineAt(3).wrapped());

    REQUIRE(!grid.absoluteLineAt(4).wrapped());
    REQUIRE(grid.absoluteLineAt(5).wrapped());
    REQUIRE(grid.absoluteLineAt(6).wrapped());
    REQUIRE(grid.absoluteLineAt(7).wrapped());

    SECTION("grow from 2x2 to 8x2") {
        (void) grid.resize(Size{8, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 3x2");

        CHECK(grid.historyLineCount() == 0);
        CHECK(grid.screenSize() == Size{8, 2});

        CHECK(!grid.lineAt(1).wrapped());
        CHECK(grid.renderTextLine(1) == "ABCDEFGH");

        CHECK(!grid.lineAt(2).wrapped());
        CHECK(grid.renderTextLine(2) == "abcdefgh");
    }

    SECTION("grow from 2x2 to 3x2 to ... to 8x2") {
        // {{{ 3x2
        (void) grid.resize(Size{3, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 3x2");

        REQUIRE(grid.historyLineCount() == 4);
        REQUIRE(grid.screenSize() == Size{3, 2});

        REQUIRE(grid.renderTextLineAbsolute(0) == "ABC");
        REQUIRE(grid.renderTextLineAbsolute(1) == "DEF");
        REQUIRE(grid.renderTextLineAbsolute(2) == "GH ");
        REQUIRE(grid.renderTextLineAbsolute(3) == "abc");
        REQUIRE(grid.renderTextLineAbsolute(4) == "def");
        REQUIRE(grid.renderTextLineAbsolute(5) == "gh ");

        REQUIRE(!grid.absoluteLineAt(0).wrapped());
        REQUIRE(grid.absoluteLineAt(1).wrapped());
        REQUIRE(grid.absoluteLineAt(2).wrapped());
        REQUIRE(!grid.absoluteLineAt(3).wrapped());
        REQUIRE(grid.absoluteLineAt(4).wrapped());
        REQUIRE(grid.absoluteLineAt(5).wrapped());
        // }}}

        // {{{ 4x2
        (void) grid.resize(Size{4, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 4x2");

        REQUIRE(grid.historyLineCount() == 2);
        REQUIRE(grid.screenSize() == Size{4, 2});

        REQUIRE(grid.renderTextLineAbsolute(0) == "ABCD");
        REQUIRE(grid.renderTextLineAbsolute(1) == "EFGH");
        REQUIRE(grid.renderTextLineAbsolute(2) == "abcd");
        REQUIRE(grid.renderTextLineAbsolute(3) == "efgh");

        REQUIRE(!grid.absoluteLineAt(0).wrapped());
        REQUIRE(grid.absoluteLineAt(1).wrapped());
        REQUIRE(!grid.absoluteLineAt(2).wrapped());
        REQUIRE(grid.absoluteLineAt(3).wrapped());
        // }}}

        // {{{ 5x2
        (void) grid.resize(Size{5, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 5x2");

        REQUIRE(grid.historyLineCount() == 2);
        REQUIRE(grid.screenSize() == Size{5, 2});

        REQUIRE(grid.renderTextLineAbsolute(0) == "ABCDE");
        REQUIRE(grid.renderTextLineAbsolute(1) == "FGH  ");
        REQUIRE(grid.renderTextLineAbsolute(2) == "abcde");
        REQUIRE(grid.renderTextLineAbsolute(3) == "fgh  ");

        REQUIRE(!grid.absoluteLineAt(0).wrapped());
        REQUIRE(grid.absoluteLineAt(1).wrapped());
        REQUIRE(!grid.absoluteLineAt(2).wrapped());
        REQUIRE(grid.absoluteLineAt(3).wrapped());
        // }}}

        // {{{ 7x2
        (void) grid.resize(Size{7, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 7x2");

        REQUIRE(grid.historyLineCount() == 2);
        REQUIRE(grid.screenSize() == Size{7, 2});

        REQUIRE(grid.renderTextLineAbsolute(0) == "ABCDEFG");
        REQUIRE(grid.renderTextLineAbsolute(1) == "H      ");
        REQUIRE(grid.renderTextLineAbsolute(2) == "abcdefg");
        REQUIRE(grid.renderTextLineAbsolute(3) == "h      ");

        REQUIRE(!grid.absoluteLineAt(0).wrapped());
        REQUIRE(grid.absoluteLineAt(1).wrapped());
        REQUIRE(!grid.absoluteLineAt(2).wrapped());
        REQUIRE(grid.absoluteLineAt(3).wrapped());
        // }}}

        // {{{ 8x2
        (void) grid.resize(Size{8, 2}, Coordinate{1, 1}, false);
        logGridText(grid, "after resize 8x2");

        REQUIRE(grid.historyLineCount() == 0);
        REQUIRE(grid.screenSize() == Size{8, 2});

        REQUIRE(grid.renderTextLineAbsolute(0) == "ABCDEFGH");
        REQUIRE(grid.renderTextLineAbsolute(1) == "abcdefgh");

        REQUIRE(!grid.absoluteLineAt(0).wrapped());
        REQUIRE(!grid.absoluteLineAt(1).wrapped());
        // }}}
    }
}
