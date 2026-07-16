// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>

#include <vtrasterizer/ImageRenderer.h>
#include <vtrasterizer/RendererTestHelpers.h>

#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

auto constexpr CellSize = ImageSize { Width(10), Height(20) };

auto const testGridMetrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                           .cellSize = CellSize,
                                           .baseline = 15,
                                           .underline = { .position = 17, .thickness = 1 } };

/// A rasterized image spanning @p cellSpan cells at exactly one image pixel per target pixel.
std::shared_ptr<RasterizedImage> makeImage(GridSize cellSpan, ImageLayer layer = ImageLayer::Replace)
{
    auto const imageSize = ImageSize { Width::cast_from(unbox(cellSpan.columns) * unbox(CellSize.width)),
                                       Height::cast_from(unbox(cellSpan.lines) * unbox(CellSize.height)) };
    auto data = Image::Data(imageSize.area() * 4, 0x7F);
    auto image =
        std::make_shared<Image>(ImageId(1), ImageFormat::RGBA, std::move(data), imageSize, [](auto) {});

    return std::make_shared<RasterizedImage>(std::move(image),
                                             ImageAlignment::TopStart,
                                             ImageResize::NoResize,
                                             RGBAColor { 0, 0, 0, 0xFF },
                                             cellSpan,
                                             CellSize,
                                             layer);
}

/// Drives one frame's worth of cells through the renderer, left to right along a single line.
void renderRow(ImageRenderer& renderer, std::shared_ptr<RasterizedImage> const& image, unsigned cellCount)
{
    renderer.beginFrame();
    for (auto const column: std::views::iota(0u, cellCount))
    {
        auto const fragment = ImageFragment {
            image, CellLocation { .line = LineOffset(0), .column = ColumnOffset::cast_from(column) }
        };
        renderer.renderImage(
            crispy::point { .x = static_cast<int>(column * unbox<unsigned>(CellSize.width)), .y = 0 },
            fragment);
    }
    renderer.endFrame();
}

} // namespace

TEST_CASE("ImageRenderer.uploads an image once, however many cells it spans", "[image][renderer]")
{
    // The whole point of the whole-image texture: the cost of showing an image is its pixels, not
    // its cell count. Slicing it per cell made a full-screen image thousands of uploads AND made it
    // evict its own tiles mid-frame, so most cells sampled another cell's pixels.
    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto constexpr CellCount = 64u;
    auto const image = makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(CellCount) });
    renderRow(imageRenderer, image, CellCount);

    auto& backend = renderTarget.getMockImageBackend();
    CHECK(backend.createCommands.size() == 1);
    CHECK(backend.quadCommands.size() == CellCount);

    // Nothing goes through the tile atlas any more, so no tile location can be handed to two cells.
    CHECK(renderTarget.getMockBackend().uploadCommands.empty());

    // A second frame reuses the texture rather than re-uploading it.
    renderRow(imageRenderer, image, CellCount);
    CHECK(backend.createCommands.size() == 1);
}

TEST_CASE("ImageRenderer.each cell samples its own slice of the image", "[image][renderer]")
{
    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto constexpr CellCount = 4u;
    auto const image = makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(CellCount) });
    renderRow(imageRenderer, image, CellCount);

    auto const& quads = renderTarget.getMockImageBackend().quadCommands;
    REQUIRE(quads.size() == CellCount);

    // Every quad names the same texture, lands at its own target x, and samples its own quarter.
    for (auto const [index, quad]: crispy::views::enumerate(quads))
    {
        INFO("cell " << index);
        CHECK(quad.texture == quads.front().texture);
        CHECK(quad.x == static_cast<int>(index) * unbox<int>(CellSize.width));
        CHECK(quad.y == 0);
        CHECK(quad.targetSize == CellSize);
        CHECK(quad.source.width > 0.0f);
        if (index > 0)
            CHECK(quad.source.x > quads[static_cast<size_t>(index) - 1].source.x);
    }
}

TEST_CASE("ImageRenderer.routes layers to either side of the text", "[image][renderer]")
{
    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto const below =
        makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(1) }, ImageLayer::Below);
    renderRow(imageRenderer, below, 1);
    REQUIRE(renderTarget.getMockImageBackend().quadCommands.size() == 1);
    CHECK_FALSE(renderTarget.getMockImageBackend().quadCommands.front().aboveText);

    // Replace deliberately draws ABOVE the text, matching the behaviour this path replaced.
    auto renderTarget2 = MockRenderTarget {};
    auto imageRenderer2 = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer2.setRenderTarget(renderTarget2, directMappingAllocator);
    auto const replace =
        makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(1) }, ImageLayer::Replace);
    renderRow(imageRenderer2, replace, 1);
    REQUIRE(renderTarget2.getMockImageBackend().quadCommands.size() == 1);
    CHECK(renderTarget2.getMockImageBackend().quadCommands.front().aboveText);
}

TEST_CASE("ImageRenderer.discardImage releases the texture", "[image][renderer]")
{
    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto const image = makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(2) });
    renderRow(imageRenderer, image, 2);

    auto& backend = renderTarget.getMockImageBackend();
    REQUIRE(backend.createCommands.size() == 1);

    imageRenderer.discardImage(image->image().id());
    REQUIRE(backend.destroyCommands.size() == 1);
    CHECK(backend.destroyCommands.front().id == backend.createCommands.front().id);

    // Discarding twice must not double-destroy.
    imageRenderer.discardImage(image->image().id());
    CHECK(backend.destroyCommands.size() == 1);
}
