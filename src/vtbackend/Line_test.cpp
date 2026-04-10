// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Line.h>

#include <crispy/escape.h>

#include <catch2/catch_test_macros.hpp>

using namespace std;

using namespace vtbackend;
using namespace crispy;

TEST_CASE("Line.wrappedFlag", "[Line]")
{
    auto line = Line(ColumnCount(10), LineFlag::Wrapped, GraphicsAttributes {});
    CHECK(line.wrapped());
    CHECK(line.wrappedFlag() == LineFlag::Wrapped);
}

TEST_CASE("Line.resize", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);

    auto const sgr = GraphicsAttributes {};
    auto lineSoA = Line(DisplayWidth, LineFlag::None, sgr);
    CHECK(lineSoA.size() == DisplayWidth);

    lineSoA.resize(ColumnCount(10));
    CHECK(lineSoA.size() == ColumnCount(10));

    lineSoA.resize(ColumnCount(5));
    CHECK(lineSoA.size() == ColumnCount(5));

    lineSoA.resize(ColumnCount(3));
    CHECK(lineSoA.size() == ColumnCount(3));
}

TEST_CASE("Line.reflow", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);

    auto const sgr = GraphicsAttributes {};
    auto lineSoA = Line(DisplayWidth, LineFlag::Wrappable, sgr);

    // Write "abcd" into the line via SoA
    auto& storage = lineSoA.storage();
    storage.codepoints[0] = 'a';
    storage.codepoints[1] = 'b';
    storage.codepoints[2] = 'c';
    storage.codepoints[3] = 'd';
    for (size_t i = 0; i < 4; ++i)
        storage.clusterSize[i] = 1;

    CHECK(lineSoA.toUtf8() == "abcd");

    auto overflow = lineSoA.reflow(ColumnCount(5));
    CHECK(overflow.codepoints.empty()); // no overflow
    CHECK(lineSoA.size() == ColumnCount(5));

    // Reset for reflow-shrink test
    lineSoA = Line(DisplayWidth, LineFlag::Wrappable, sgr);
    auto& s2 = lineSoA.storage();
    s2.codepoints[0] = 'a';
    s2.codepoints[1] = 'b';
    s2.codepoints[2] = 'c';
    s2.codepoints[3] = 'd';
    for (size_t i = 0; i < 4; ++i)
        s2.clusterSize[i] = 1;

    overflow = lineSoA.reflow(ColumnCount(3));
    CHECK(lineSoA.size() == ColumnCount(3));
    CHECK(overflow.codepoints.size() == 1); // 'd' overflowed
}

TEST_CASE("Line.SoA.basic", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(10);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.flags |= CellFlag::CurlyUnderlined;

    auto line = Line(DisplayWidth, LineFlag::None, sgr);

    // Write some text via CellProxy
    for (size_t i = 0; i < 10; ++i)
    {
        auto cell = line.useCellAt(ColumnOffset::cast_from(i));
        cell.write(sgr, static_cast<char32_t>('0' + i), 1);
    }

    // Verify via CellProxy
    for (size_t i = 0; i < 10; ++i)
    {
        auto cell = line.useCellAt(ColumnOffset::cast_from(i));
        INFO(std::format("column {} codepoint {}", i, (char) cell.codepoint(0)));
        CHECK(cell.foregroundColor() == sgr.foregroundColor);
        CHECK(cell.backgroundColor() == sgr.backgroundColor);
        CHECK(cell.underlineColor() == sgr.underlineColor);
        CHECK(cell.codepointCount() == 1);
        CHECK(char(cell.codepoint(0)) == char('0' + i));
    }
}

TEST_CASE("Line.toUtf8", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(5);

    auto line = Line(DisplayWidth, LineFlag::None, GraphicsAttributes {});
    CHECK(line.toUtf8() == "     "); // 5 empty cells -> 5 spaces

    auto cell0 = line.useCellAt(ColumnOffset(0));
    cell0.write(GraphicsAttributes {}, U'H', 1);
    auto cell1 = line.useCellAt(ColumnOffset(1));
    cell1.write(GraphicsAttributes {}, U'i', 1);

    auto const text = line.toUtf8();
    CHECK(text == "Hi   ");

    auto const trimmed = line.toUtf8Trimmed();
    CHECK(trimmed == "Hi");
}
