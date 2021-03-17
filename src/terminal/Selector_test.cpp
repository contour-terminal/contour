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
#include <terminal/Selector.h>
#include <catch2/catch.hpp>

using crispy::Size;
using namespace std;
using namespace std::placeholders;
using namespace terminal;

// Different cases to test
// - single cell
// - inside single line
// - multiple lines
// - multiple lines fully in history
// - multiple lines from history into main buffer
// all of the above with and without scrollback != 0.

namespace
{
    struct TextSelection {
        string text;

        void operator()(Coordinate const& _pos, Cell const& _cell)
        {
            text += _pos.column < lastColumn_ ? "\n" : "";
            text += _cell.toUtf8();
            lastColumn_ = _pos.column;
        }

      private:
        int lastColumn_ = 0;
    };
}

TEST_CASE("Selector.Linear", "[selector]")
{
    auto screenEvents = ScreenEvents{};
    auto screen = Screen{Size{11, 3}, screenEvents};
    screen.write(
        //       123456789AB
        /* 0 */ "12345,67890"s +
        /* 1 */ "ab,cdefg,hi"s +
        /* 2 */ "12345,67890"s
    );

    SECTION("single-cell") { // "b"
        auto const pos = screen.toAbsolute({2, 2});
        auto selector = Selector{Selector::Mode::Linear, U",", screen, pos};
        selector.extend(pos);
        selector.stop();

        vector<Selector::Range> const selection = selector.selection();
        REQUIRE(selection.size() == 1);
        Selector::Range const& r1 = selection[0];
        CHECK(r1.line == pos.row);
        CHECK(r1.fromColumn == pos.column);
        CHECK(r1.toColumn == pos.column);
        CHECK(r1.length() == 1);

        auto selectedText = TextSelection{};
        selector.render(selectedText);
        CHECK(selectedText.text == "b");
    }

    SECTION("forward single-line") { // "b,c"
        auto selector = Selector{Selector::Mode::Linear, U",", screen, Coordinate{1, 2}};
        selector.extend(Coordinate{1, 4});
        selector.stop();

        vector<Selector::Range> const selection = selector.selection();
        REQUIRE(selection.size() == 1);
        Selector::Range const& r1 = selection[0];
        CHECK(r1.line == 1);
        CHECK(r1.fromColumn == 2);
        CHECK(r1.toColumn == 4);
        CHECK(r1.length() == 3);

        auto selectedText = TextSelection{};
        selector.render(selectedText);
        CHECK(selectedText.text == "b,c");
    }

    SECTION("forward multi-line") { // "b,cdefg,hi\n1234"
        auto selector = Selector{Selector::Mode::Linear, U",", screen, Coordinate{1, 2}};
        selector.extend(Coordinate{2, 4});
        selector.stop();

        vector<Selector::Range> const selection = selector.selection();
        REQUIRE(selection.size() == 2);

        Selector::Range const& r1 = selection[0];
        CHECK(r1.line == 1);
        CHECK(r1.fromColumn == 2);
        CHECK(r1.toColumn == 11);
        CHECK(r1.length() == 10);

        Selector::Range const& r2 = selection[1];
        CHECK(r2.line == 2);
        CHECK(r2.fromColumn == 1);
        CHECK(r2.toColumn == 4);
        CHECK(r2.length() == 4);

        auto selectedText = TextSelection{};
        selector.render(selectedText);
        CHECK(selectedText.text == "b,cdefg,hi\n1234");
    }

    SECTION("multiple lines fully in history") {
        screen.write("foo\nbar\n"); // move first two lines into history.
        /*
        "12345,67890"
        "ab,cdefg,hi"
        "12345,67890"
        "foo"
        "bar"
        */

        auto selector = Selector{Selector::Mode::Linear, U",", screen, Coordinate{1, 7}};
        selector.extend(Coordinate{2, 3});
        selector.stop();

        vector<Selector::Range> const selection = selector.selection();
        REQUIRE(selection.size() == 2);

        Selector::Range const& r1 = selection[0];
        CHECK(r1.line == 1);
        CHECK(r1.fromColumn == 7);
        CHECK(r1.toColumn == 11);
        CHECK(r1.length() == 5);

        Selector::Range const& r2 = selection[1];
        CHECK(r2.line == 2);
        CHECK(r2.fromColumn == 1);
        CHECK(r2.toColumn == 3);
        CHECK(r2.length() == 3);

        auto selectedText = TextSelection{};
        selector.render(selectedText);
        CHECK(selectedText.text == "fg,hi\n123");
    }

    SECTION("multiple lines from history into main buffer") {
        screen.write("foo\nbar\n"); // move first two lines into history.
        /*
        "12345,67890"
        "ab,cdefg,hi"         (--
        "12345,67890" -----------
        "foo"         --)
        "bar"
        */

        auto selector = Selector{Selector::Mode::Linear, U",", screen, Coordinate{1, 9}};
        selector.extend(Coordinate{3, 2});
        selector.stop();

        vector<Selector::Range> const selection = selector.selection();
        REQUIRE(selection.size() == 3);

        Selector::Range const& r1 = selection[0];
        CHECK(r1.line == 1);
        CHECK(r1.fromColumn == 9);
        CHECK(r1.toColumn == 11);
        CHECK(r1.length() == 3);

        Selector::Range const& r2 = selection[1];
        CHECK(r2.line == 2);
        CHECK(r2.fromColumn == 1);
        CHECK(r2.toColumn == 11);
        CHECK(r2.length() == 11);

        Selector::Range const& r3 = selection[2];
        CHECK(r3.line == 3);
        CHECK(r3.fromColumn == 1);
        CHECK(r3.toColumn == 2);
        CHECK(r3.length() == 2);

        auto selectedText = TextSelection{};
        selector.render(selectedText);
        CHECK(selectedText.text == ",hi\n12345,67890\nfo");
    }
}

TEST_CASE("Selector.LinearWordWise", "[selector]")
{
    // TODO
}

TEST_CASE("Selector.FullLine", "[selector]")
{
    // TODO
}

TEST_CASE("Selector.Rectangular", "[selector]")
{
    // TODO
}
