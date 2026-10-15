// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the render buffer's own helpers: laying out host text (the sticky header's chip and
// placeholder, a collapsed fold's label) as render cells, and clear() forgetting the sticky header with
// everything else -- but not the storage it took.

#include <vtbackend/render/RenderBuffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;

namespace
{
/// @p text laid out by layoutRenderText() into a fresh vector.
[[nodiscard]] std::vector<RenderCell> laidOut(std::u32string_view text,
                                              CellLocation origin,
                                              ColumnOffset end,
                                              RenderAttributes const& attributes = {})
{
    auto cells = std::vector<RenderCell> {};
    layoutRenderText(cells, text, attributes, origin, end);
    return cells;
}
} // namespace

TEST_CASE("RenderBuffer.layoutRenderText.placesOneCellPerGlyph", "[renderbuffer][stickyheader]")
{
    auto const attributes = RenderAttributes { .foregroundColor = 0x112233_rgb,
                                               .backgroundColor = 0x445566_rgb,
                                               .decorationColor = 0x112233_rgb,
                                               .flags = {},
                                               .lineFlags = LineFlag::None };
    auto cells = std::vector<RenderCell> {};
    auto const next = layoutRenderText(cells,
                                       U"✓ 3s",
                                       attributes,
                                       CellLocation { .line = LineOffset(2), .column = ColumnOffset(10) },
                                       ColumnOffset(20));

    REQUIRE(cells.size() == 4);
    CHECK(cells[0].codepoints == U"✓");
    CHECK(cells[0].position == CellLocation { .line = LineOffset(2), .column = ColumnOffset(10) });
    CHECK(cells[3].codepoints == U"s");
    CHECK(cells[3].position.column == ColumnOffset(13));
    CHECK(cells[0].attributes.foregroundColor == 0x112233_rgb);
    CHECK(cells[0].attributes.backgroundColor == 0x445566_rgb);
    // One shaping group, so the text renderer places it at its own pen position.
    CHECK(cells.front().groupStart);
    CHECK(cells.back().groupEnd);
    // The column after the last cell: what tells a caller how wide the text came out.
    CHECK(next == ColumnOffset(14));
}

TEST_CASE("RenderBuffer.layoutRenderText.wideGlyphsTakeTwoColumnsAndStopAtTheEnd",
          "[renderbuffer][stickyheader]")
{
    // 'a' takes column 0, '中' columns 1-2; 'b' would start at column 3, which is the end.
    auto const cells = laidOut(U"a中b", CellLocation {}, ColumnOffset(3));
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].codepoints == U"中");
    CHECK(cells[1].width == 2);

    // A glyph that would straddle the end is dropped whole, never cut in half.
    CHECK(laidOut(U"a中", CellLocation {}, ColumnOffset(2)).size() == 1);
}

TEST_CASE("RenderBuffer.layoutRenderText.aCombiningMarkJoinsItsBase", "[renderbuffer][stickyheader]")
{
    // 'e' followed by U+0301 COMBINING ACUTE ACCENT: one glyph, one column.
    auto cells = std::vector<RenderCell> {};
    auto const next = layoutRenderText(cells, U"éx", RenderAttributes {}, CellLocation {}, ColumnOffset(10));
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].codepoints == U"é");
    CHECK(cells[1].position.column == ColumnOffset(1));
    CHECK(next == ColumnOffset(2));
}

TEST_CASE("RenderBuffer.layoutRenderText.appendsARunOfItsOwn", "[renderbuffer][gutter]")
{
    // Two labels in one vector: each its own shaping group, and a combining mark that opens the second
    // text has no glyph of ITS text to join, so it never reaches back into the first.
    auto cells = laidOut(U"ab", CellLocation {}, ColumnOffset(10));
    layoutRenderText(cells,
                     U"́c",
                     RenderAttributes {},
                     CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) },
                     ColumnOffset(10));
    REQUIRE(cells.size() == 4);
    CHECK(cells[1].codepoints == U"b");
    CHECK(cells[1].groupEnd);
    CHECK(cells[2].groupStart);
    CHECK(cells[2].position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) });
    CHECK(cells.back().groupEnd);
}

TEST_CASE("RenderBuffer.layoutRenderText.nothingThatFitsLeavesTheOutputAlone", "[renderbuffer][gutter]")
{
    auto cells = laidOut(U"x", CellLocation {}, ColumnOffset(10));
    auto const next = layoutRenderText(cells,
                                       U"中",
                                       RenderAttributes {},
                                       CellLocation { .line = {}, .column = ColumnOffset(9) },
                                       ColumnOffset(10));
    CHECK(next == ColumnOffset(9));
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].groupEnd);
}

TEST_CASE("RenderBuffer.clear.forgetsTheStickyHeader", "[renderbuffer][stickyheader]")
{
    auto buffer = RenderBuffer {};
    auto& header = buffer.showStickyHeader();
    header.kind = StickyHeaderKind::Command;
    header.block = CommandBlockId(1);
    layoutRenderText(header.chip, U"✓", RenderAttributes {}, CellLocation {}, ColumnOffset(10));
    buffer.clear();
    CHECK_FALSE(buffer.stickyHeader.has_value());
}

TEST_CASE("RenderBuffer.clear.keepsTheStickyHeadersStorage", "[renderbuffer][stickyheader]")
{
    // A header shown frame after frame lays itself out into the storage the last frame's left behind.
    auto buffer = RenderBuffer {};
    layoutRenderText(buffer.showStickyHeader().cells,
                     U"make --jobs=8",
                     RenderAttributes {},
                     CellLocation {},
                     ColumnOffset(80));
    auto const* const storage = buffer.stickyHeader->cells.data();
    auto const capacity = buffer.stickyHeader->cells.capacity();

    buffer.clear();
    auto const& next = buffer.showStickyHeader();
    CHECK(next.cells.empty());
    CHECK(next.chip.empty());
    CHECK(next.cells.data() == storage);
    CHECK(next.cells.capacity() == capacity);
}
