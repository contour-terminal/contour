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
#include <catch2/catch_all.hpp>

using crispy::Size;
using namespace std;
using namespace std::placeholders;
using namespace terminal;

namespace
{

template <typename T>
struct TestSelectionHelper: public terminal::SelectionHelper
{
    Screen<T>* screen;
    explicit TestSelectionHelper(Screen<T>& self): screen{&self} {}

    PageSize pageSize() const noexcept { return screen->pageSize(); }
    bool wordDelimited(Coordinate _pos) const noexcept { return true; } // TODO
    bool wrappedLine(LineOffset _line) const noexcept { return screen->isLineWrapped(_line); }
    bool cellEmpty(Coordinate _pos) const noexcept { return screen->at(_pos).empty(); }
    int cellWidth(Coordinate _pos) const noexcept { return screen->at(_pos).width(); }
};

template <typename T>
TestSelectionHelper(Screen<T>&) -> TestSelectionHelper<T>;

}

// Different cases to test
// - single cell
// - inside single line
// - multiple lines
// - multiple lines fully in history
// - multiple lines from history into main buffer
// all of the above with and without scrollback != 0.

namespace
{
    template <typename T>
    [[maybe_unused]]
    void logScreenTextAlways(Screen<T> const& screen, string const& headline = "")
    {
        fmt::print("{}: ZI={} cursor={} HM={}..{}\n",
                headline.empty() ? "screen dump"s : headline,
                screen.grid().zero_index(),
                screen.realCursorPosition(),
                screen.margin().horizontal.from,
                screen.margin().horizontal.to
        );
        fmt::print("{}\n", dumpGrid(screen.grid()));
    }

    template <typename T>
    struct TextSelection
    {
        Screen<T> const* screen;
        string text;
        ColumnOffset lastColumn_ = ColumnOffset(0);

        explicit TextSelection(Screen<T> const& s): screen{&s} {}

        void operator()(Coordinate const& _pos)
        {
            auto const& cell = screen->at(_pos);
            text += _pos.column < lastColumn_ ? "\n" : "";
            text += cell.toUtf8();
            lastColumn_ = _pos.column;
        }
    };
    template <typename T>
    TextSelection(Screen<T> const&) -> TextSelection<T>;
}

TEST_CASE("Selector.Linear", "[selector]")
{
    auto screenEvents = ScreenEvents{};
    auto term = MockTerm(PageSize{LineCount(3), ColumnCount(11)}, LineCount(5));
    auto& screen = term.screen;
    auto selectionHelper = TestSelectionHelper(screen);
    screen.write(
        //       0123456789A
        /* 0 */ "12345,67890"s +
        /* 1 */ "ab,cdefg,hi"s +
        /* 2 */ "12345,67890"s
    );

    logScreenTextAlways(screen, "init");
    REQUIRE(screen.grid().lineText(LineOffset(0)) == "12345,67890");
    REQUIRE(screen.grid().lineText(LineOffset(1)) == "ab,cdefg,hi");
    REQUIRE(screen.grid().lineText(LineOffset(2)) == "12345,67890");

    SECTION("single-cell") { // "b"
        auto const pos = Coordinate{LineOffset(1), ColumnOffset(1)};
        auto selector = LinearSelection(selectionHelper, pos);
        selector.extend(pos);
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == pos.line);
        CHECK(r1.fromColumn == pos.column);
        CHECK(r1.toColumn == pos.column);
        CHECK(r1.length() == ColumnCount(1));

        auto selectedText = TextSelection{screen};
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b");
    }

    SECTION("forward single-line") { // "b,c"
        auto const pos = Coordinate{LineOffset(1), ColumnOffset(1)};
        auto selector = LinearSelection(selectionHelper, pos);
        selector.extend(Coordinate{LineOffset(1), ColumnOffset(3)});
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(1));
        CHECK(r1.fromColumn == ColumnOffset(1));
        CHECK(r1.toColumn == ColumnOffset(3));
        CHECK(r1.length() == ColumnCount(3));

        auto selectedText = TextSelection{screen};
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,c");
    }

    SECTION("forward multi-line") { // "b,cdefg,hi\n1234"
        auto const pos = Coordinate{LineOffset(1), ColumnOffset(1)};
        auto selector = LinearSelection(selectionHelper, pos);
        selector.extend(Coordinate{LineOffset(2), ColumnOffset(3)});
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(1));
        CHECK(r1.fromColumn == ColumnOffset(1));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(10));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(2));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(3));
        CHECK(r2.length() == ColumnCount(4));

        auto selectedText = TextSelection{screen};
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,cdefg,hi\n1234");
    }

    SECTION("multiple lines fully in history") {
        screen.write("foo\r\nbar\r\n"); // move first two lines into history.
        /*
         * |  0123456789A
        -2 | "12345,67890"
        -1 | "ab,cdefg,hi"       [fg,hi]
         0 | "12345,67890"       [123]
         1 | "foo"
         2 | "bar"
        */

        logScreenTextAlways(screen);
        auto selector = LinearSelection(selectionHelper, Coordinate{LineOffset(-2), ColumnOffset(6)});
        selector.extend(Coordinate{LineOffset(-1), ColumnOffset(2)});
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(-2));
        CHECK(r1.fromColumn == ColumnOffset(6));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(5));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(-1));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(2));
        CHECK(r2.length() == ColumnCount(3));

        auto selectedText = TextSelection{screen};
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "fg,hi\n123");
    }

    SECTION("multiple lines from history into main buffer") {
        logScreenTextAlways(screen, "just before next test-write");
        screen.write("foo\r\nbar\r\n"); // move first two lines into history.
        logScreenTextAlways(screen, "just after next test-write");
        /*
        -3 | "12345,67890"
        -2 | "ab,cdefg,hi"         (--
        -1 | "12345,67890" -----------
         0 | "foo"         --)
         1 | "bar"
         2 | ""
        */

        auto selector = LinearSelection(selectionHelper, Coordinate{LineOffset(-2), ColumnOffset(8)});
        selector.extend(Coordinate{LineOffset(0), ColumnOffset(1)});
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 3);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(-2));
        CHECK(r1.fromColumn == ColumnOffset(8));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(3));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(-1));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(10));
        CHECK(r2.length() == ColumnCount(11));

        Selection::Range const& r3 = selection[2];
        CHECK(r3.line == LineOffset(0));
        CHECK(r3.fromColumn == ColumnOffset(0));
        CHECK(r3.toColumn == ColumnOffset(1));
        CHECK(r3.length() == ColumnCount(2));

        auto selectedText = TextSelection{screen};
        renderSelection(selector, selectedText);
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
