// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>

#include <vtrasterizer/CursorRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RendererTestHelpers.h>

#include <libunicode/bidi.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

constexpr auto CellWidth = 10;
constexpr auto Baseline = 15;

/// The bar's stem width, which is also the side of the direction-hint square.
///
/// Spelled out rather than imported so the test pins the number the renderer must produce: with a
/// baseline of 15 this is `max(15 / 3, 1)`.
constexpr auto Thickness = Baseline / 3;

/// Drives one cursor draw and returns the X coordinate of every tile it scheduled, in draw order.
struct CursorFixture
{
    GridMetrics gridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                              .cellSize = ImageSize { Width(CellWidth), Height(20) },
                              .baseline = Baseline,
                              .underline = { .position = 17, .thickness = 1 } };

    MockRenderTarget renderTarget {};
    atlas::DirectMappingAllocator<RenderTileAttributes> allocator {};
    CursorRenderer renderer { gridMetrics, CursorShape::Bar };
    std::optional<RenderTarget::TextureAtlas> textureAtlas {};

    CursorFixture()
    {
        renderer.setRenderTarget(renderTarget, allocator);
        textureAtlas.emplace(renderTarget.getMockBackend(),
                             atlas::AtlasProperties { .format = atlas::Format::Red,
                                                      .tileSize = gridMetrics.cellSize,
                                                      .hashCount = { 1024 },
                                                      .tileCount = { 4096 },
                                                      .directMappingCount = 128 });
        renderer.setTextureAtlas(*textureAtlas);
    }

    [[nodiscard]] std::vector<int> draw(CursorShape shape,
                                        unicode::Bidi_Direction direction,
                                        bool mixedDirection,
                                        int columnWidth = 1)
    {
        renderTarget.getMockBackend().renderCommands.clear();
        renderer.setShape(shape);
        renderer.render(crispy::point { .x = 0, .y = 0 },
                        columnWidth,
                        RGBColor { 0xFF, 0xFF, 0xFF },
                        direction,
                        mixedDirection);

        auto positions = std::vector<int> {};
        for (auto const& command: renderTarget.getMockBackend().renderCommands)
            positions.emplace_back(command.x.value);
        return positions;
    }
};

} // namespace

// The hint exists to answer "which way is the character under me written?", and that is only a
// question worth answering where a paragraph actually mixes directions. Drawing it on every cursor
// would be noise on the overwhelming majority of terminal lines.
TEST_CASE("CursorRenderer.a uniform paragraph gets no direction hint", "[renderer][cursor]")
{
    auto fixture = CursorFixture {};

    auto const positions = fixture.draw(CursorShape::Bar, unicode::Bidi_Direction::Left_To_Right, false);

    REQUIRE(positions.size() == 1); // the stem, and nothing else
    CHECK(positions[0] == 0);
}

TEST_CASE("CursorRenderer.a mixed paragraph hints toward the writing direction", "[renderer][cursor]")
{
    auto fixture = CursorFixture {};

    SECTION("left-to-right puts the square right of the stem")
    {
        auto const positions = fixture.draw(CursorShape::Bar, unicode::Bidi_Direction::Left_To_Right, true);

        REQUIRE(positions.size() == 2);
        CHECK(positions[0] == 0);         // stem against the cell's leading edge
        CHECK(positions[1] == Thickness); // square immediately right of it, reading as an upper-left
                                          // bracket
    }

    SECTION("right-to-left puts the square left of the stem")
    {
        // The stem itself has already moved to the cell's trailing edge, so the square hangs off its
        // other side -- the mirror image, reading as an upper-right bracket.
        auto const positions = fixture.draw(CursorShape::Bar, unicode::Bidi_Direction::Right_To_Left, true);

        REQUIRE(positions.size() == 2);
        CHECK(positions[0] == CellWidth - Thickness);
        CHECK(positions[1] == CellWidth - (2 * Thickness));
    }

    SECTION("a double-width cell keeps the square on the stem")
    {
        // Two cells means two stem tiles plus the square; the stem still sits at the trailing edge of
        // the pair, so the square is one thickness further left again.
        auto const positions =
            fixture.draw(CursorShape::Bar, unicode::Bidi_Direction::Right_To_Left, true, 2);

        REQUIRE(positions.size() == 3);
        CHECK(positions.back() == (2 * CellWidth) - (2 * Thickness));
    }
}

// VTE draws the hint for its I-beam only, and its own note says the other shapes want a visual design
// nobody has settled on. A block or a rectangle has no stem to hang a square off, and an underscore
// already spans the cell, so a square at its end would read as part of the glyph beneath.
TEST_CASE("CursorRenderer.only a bar carries the direction hint", "[renderer][cursor]")
{
    auto fixture = CursorFixture {};

    for (auto const shape: { CursorShape::Block, CursorShape::Underscore, CursorShape::Rectangle })
    {
        auto const positions = fixture.draw(shape, unicode::Bidi_Direction::Right_To_Left, true);
        CHECK(positions.size() == 1);
    }
}
