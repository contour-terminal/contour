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
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/Selector.h>

#include <catch2/catch.hpp>

using crispy::Size;
using namespace std;
using namespace std::placeholders;
using namespace terminal;

namespace
{

template <typename T>
struct test_selection_helper: public terminal::selection_helper
{
    screen<T>* screen;
    explicit test_selection_helper(terminal::screen<T>& self): screen { &self } {}

    [[nodiscard]] PageSize pageSize() const noexcept override { return screen->pageSize(); }
    [[nodiscard]] bool wordDelimited(cell_location /*pos*/) const noexcept override { return true; } // TODO
    [[nodiscard]] bool wrappedLine(line_offset line) const noexcept override
    {
        return screen->isLineWrapped(line);
    }
    [[nodiscard]] bool cellEmpty(cell_location pos) const noexcept override
    {
        return screen->at(pos).empty();
    }
    [[nodiscard]] int cellWidth(cell_location pos) const noexcept override { return screen->at(pos).width(); }
};

template <typename T>
test_selection_helper(screen<T>&) -> test_selection_helper<T>;

} // namespace

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
[[maybe_unused]] void logScreenTextAlways(screen<T> const& screen, string const& headline = "")
{
    fmt::print("{}: ZI={} cursor={} HM={}..{}\n",
               headline.empty() ? "screen dump"s : headline,
               screen.grid().zero_index(),
               screen.realCursorPosition(),
               screen.margin().horizontal.from,
               screen.margin().horizontal.to);
    fmt::print("{}\n", dumpGrid(screen.grid()));
}

template <typename T>
struct text_selection
{
    screen<T> const* screen;
    string text;
    column_offset lastColumn = column_offset(0);

    explicit text_selection(terminal::screen<T> const& s): screen { &s } {}

    void operator()(cell_location const& pos)
    {
        auto const& cell = screen->at(pos);
        text += pos.column < lastColumn ? "\n" : "";
        text += cell.toUtf8();
        lastColumn = pos.column;
    }
};
template <typename T>
text_selection(screen<T> const&) -> text_selection<T>;
} // namespace

TEST_CASE("Selector.Linear", "[selector]")
{
    auto screenEvents = screen_events {};
    auto term = mock_term(PageSize { LineCount(3), ColumnCount(11) }, LineCount(5));
    auto& screen = term.terminal.primaryScreen();
    auto selectionHelper = test_selection_helper(screen);
    term.writeToScreen(
        //       0123456789A
        /* 0 */ "12345,67890"s +
        /* 1 */ "ab,cdefg,hi"s +
        /* 2 */ "12345,67890"s);

    REQUIRE(screen.getGrid().lineText(line_offset(0)) == "12345,67890");
    REQUIRE(screen.getGrid().lineText(line_offset(1)) == "ab,cdefg,hi");
    REQUIRE(screen.getGrid().lineText(line_offset(2)) == "12345,67890");

    SECTION("single-cell")
    { // "b"
        auto const pos = cell_location { line_offset(1), column_offset(1) };
        auto selector = linear_selection(selectionHelper, pos, []() {});
        (void) selector.extend(pos);
        selector.complete();

        vector<selection::range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        selection::range const& r1 = selection[0];
        CHECK(r1.line == pos.line);
        CHECK(r1.fromColumn == pos.column);
        CHECK(r1.toColumn == pos.column);
        CHECK(r1.length() == ColumnCount(1));

        auto selectedText = text_selection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b");
    }

    SECTION("forward single-line")
    { // "b,c"
        auto const pos = cell_location { line_offset(1), column_offset(1) };
        auto selector = linear_selection(selectionHelper, pos, []() {});
        (void) selector.extend(cell_location { line_offset(1), column_offset(3) });
        selector.complete();

        vector<selection::range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        selection::range const& r1 = selection[0];
        CHECK(r1.line == line_offset(1));
        CHECK(r1.fromColumn == column_offset(1));
        CHECK(r1.toColumn == column_offset(3));
        CHECK(r1.length() == ColumnCount(3));

        auto selectedText = text_selection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,c");
    }

    SECTION("forward multi-line")
    { // "b,cdefg,hi\n1234"
        auto const pos = cell_location { line_offset(1), column_offset(1) };
        auto selector = linear_selection(selectionHelper, pos, []() {});
        (void) selector.extend(cell_location { line_offset(2), column_offset(3) });
        selector.complete();

        vector<selection::range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        selection::range const& r1 = selection[0];
        CHECK(r1.line == line_offset(1));
        CHECK(r1.fromColumn == column_offset(1));
        CHECK(r1.toColumn == column_offset(10));
        CHECK(r1.length() == ColumnCount(10));

        selection::range const& r2 = selection[1];
        CHECK(r2.line == line_offset(2));
        CHECK(r2.fromColumn == column_offset(0));
        CHECK(r2.toColumn == column_offset(3));
        CHECK(r2.length() == ColumnCount(4));

        auto selectedText = text_selection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,cdefg,hi\n1234");
    }

    SECTION("multiple lines fully in history")
    {
        term.writeToScreen("foo\r\nbar\r\n"); // move first two lines into history.
        /*
         * |  0123456789A
        -2 | "12345,67890"
        -1 | "ab,cdefg,hi"       [fg,hi]
         0 | "12345,67890"       [123]
         1 | "foo"
         2 | "bar"
        */

        auto selector =
            linear_selection(selectionHelper, cell_location { line_offset(-2), column_offset(6) }, []() {});
        (void) selector.extend(cell_location { line_offset(-1), column_offset(2) });
        selector.complete();

        vector<selection::range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        selection::range const& r1 = selection[0];
        CHECK(r1.line == line_offset(-2));
        CHECK(r1.fromColumn == column_offset(6));
        CHECK(r1.toColumn == column_offset(10));
        CHECK(r1.length() == ColumnCount(5));

        selection::range const& r2 = selection[1];
        CHECK(r2.line == line_offset(-1));
        CHECK(r2.fromColumn == column_offset(0));
        CHECK(r2.toColumn == column_offset(2));
        CHECK(r2.length() == ColumnCount(3));

        auto selectedText = text_selection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "fg,hi\n123");
    }

    SECTION("multiple lines from history into main buffer")
    {
        term.writeToScreen("foo\r\nbar\r\n"); // move first two lines into history.
        /*
        -3 | "12345,67890"
        -2 | "ab,cdefg,hi"         (--
        -1 | "12345,67890" -----------
         0 | "foo"         --)
         1 | "bar"
         2 | ""
        */

        auto selector =
            linear_selection(selectionHelper, cell_location { line_offset(-2), column_offset(8) }, []() {});
        (void) selector.extend(cell_location { line_offset(0), column_offset(1) });
        selector.complete();

        vector<selection::range> const selection = selector.ranges();
        REQUIRE(selection.size() == 3);

        selection::range const& r1 = selection[0];
        CHECK(r1.line == line_offset(-2));
        CHECK(r1.fromColumn == column_offset(8));
        CHECK(r1.toColumn == column_offset(10));
        CHECK(r1.length() == ColumnCount(3));

        selection::range const& r2 = selection[1];
        CHECK(r2.line == line_offset(-1));
        CHECK(r2.fromColumn == column_offset(0));
        CHECK(r2.toColumn == column_offset(10));
        CHECK(r2.length() == ColumnCount(11));

        selection::range const& r3 = selection[2];
        CHECK(r3.line == line_offset(0));
        CHECK(r3.fromColumn == column_offset(0));
        CHECK(r3.toColumn == column_offset(1));
        CHECK(r3.length() == ColumnCount(2));

        auto selectedText = text_selection { screen };
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
