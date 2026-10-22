// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/BackgroundRenderer.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtrasterizer;

TEST_CASE("BackgroundRenderer.owns_default_background", "[background]")
{
    auto const metrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                       .cellSize = ImageSize { Width(10), Height(20) } };
    auto paletteColor = RGBColor { 10, 20, 30 };
    auto renderer = BackgroundRenderer { metrics, paletteColor };
    auto target = MockRenderTarget {};
    auto allocator = Renderable::DirectMappingAllocator { 0 };
    renderer.setRenderTarget(target, allocator);

    auto cell = RenderCell {};
    cell.width = 1;
    cell.attributes.backgroundColor = paletteColor;
    paletteColor = RGBColor { 40, 50, 60 };
    renderer.renderCell(cell);
    CHECK(target.rectangles.empty());

    renderer.setDefaultColor(paletteColor);
    renderer.renderCell(cell);
    REQUIRE(target.rectangles.size() == 1);
    CHECK(target.rectangles.back().color == RGBAColor(10, 20, 30, 255));

    target.rectangles.clear();
    cell.attributes.backgroundColor = paletteColor;
    renderer.renderCell(cell);
    CHECK(target.rectangles.empty());
}

TEST_CASE("BackgroundRenderer.refreshes_line_background", "[background]")
{
    auto const metrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                       .cellSize = ImageSize { Width(10), Height(20) } };
    auto renderer = BackgroundRenderer { metrics, RGBColor { 10, 20, 30 } };
    auto target = MockRenderTarget {};
    auto allocator = Renderable::DirectMappingAllocator { 0 };
    renderer.setRenderTarget(target, allocator);

    auto line = RenderLine {};
    line.usedColumns = ColumnCount(2);
    line.displayWidth = ColumnCount(5);
    line.textAttributes.backgroundColor = RGBColor { 40, 50, 60 };
    line.fillAttributes.backgroundColor = RGBColor { 40, 50, 60 };
    renderer.setDefaultColor(RGBColor { 40, 50, 60 });
    renderer.renderLine(line);
    CHECK(target.rectangles.empty());

    renderer.setDefaultColor(RGBColor { 10, 20, 30 });
    renderer.renderLine(line);
    REQUIRE(target.rectangles.size() == 2);
    CHECK(target.rectangles[0].width == Width(20));
    CHECK(target.rectangles[1].width == Width(30));
    CHECK(target.rectangles[0].color == RGBAColor(40, 50, 60, 255));
    CHECK(target.rectangles[1].color == RGBAColor(40, 50, 60, 255));
}
