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
/// @param cellSpan how many cells the image covers.
/// @param layer which side of the text it composites on.
/// @param imageId its vtbackend image id; distinct ids are distinct images to the renderer's cache.
std::shared_ptr<RasterizedImage> makeImage(GridSize cellSpan,
                                           ImageLayer layer = ImageLayer::Replace,
                                           ImageId imageId = ImageId(1))
{
    auto const imageSize = ImageSize { Width::cast_from(unbox(cellSpan.columns) * unbox(CellSize.width)),
                                       Height::cast_from(unbox(cellSpan.lines) * unbox(CellSize.height)) };
    auto data = Image::Data(imageSize.area() * 4, 0x7F);
    auto image = std::make_shared<Image>(imageId, ImageFormat::RGBA, std::move(data), imageSize, [](auto) {});

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

TEST_CASE("ImageRenderer.widens a 24-bit RGB image for upload", "[image][renderer]")
{
    // An Image keeps whatever the protocol sent, and GIP's f=2 really is three bytes per pixel.
    // Handing that to the GPU as RGBA makes the driver read a quarter more than exists -- an
    // out-of-bounds memcpy inside the graphics driver, which is a crash rather than a wrong colour.
    auto constexpr Span = GridSize { .lines = LineCount(1), .columns = ColumnCount(2) };
    auto const imageSize = ImageSize { Width::cast_from(unbox(Span.columns) * unbox(CellSize.width)),
                                       Height::cast_from(unbox(Span.lines) * unbox(CellSize.height)) };
    auto const pixelCount = imageSize.area();

    auto rgb = Image::Data(pixelCount * 3);
    for (auto const pixel: std::views::iota(size_t { 0 }, pixelCount))
    {
        rgb[(pixel * 3) + 0] = 0x11;
        rgb[(pixel * 3) + 1] = 0x22;
        rgb[(pixel * 3) + 2] = 0x33;
    }
    auto image =
        std::make_shared<Image>(ImageId(2), ImageFormat::RGB, std::move(rgb), imageSize, [](auto) {});
    auto const rasterized = std::make_shared<RasterizedImage>(std::move(image),
                                                              ImageAlignment::TopStart,
                                                              ImageResize::NoResize,
                                                              RGBAColor { 0, 0, 0, 0xFF },
                                                              Span,
                                                              CellSize);

    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);
    renderRow(imageRenderer, rasterized, 2);

    auto const& creates = renderTarget.getMockImageBackend().createCommands;
    REQUIRE(creates.size() == 1);
    CHECK(creates.front().format == atlas::Format::RGBA);
    // The texture must hold exactly four bytes per pixel, and the alpha must be opaque.
    REQUIRE(creates.front().data.size() == pixelCount * 4);
    CHECK(creates.front().data[0] == 0x11);
    CHECK(creates.front().data[1] == 0x22);
    CHECK(creates.front().data[2] == 0x33);
    CHECK(creates.front().data[3] == 0xFF);
}

TEST_CASE("ImageRenderer.bounds resident texture memory", "[image][renderer]")
{
    // The tile atlas this replaced was a fixed-size allocation, so image memory was bounded by
    // construction. One texture per image is bounded by nothing but how many images are alive, and an
    // image lives as long as a grid cell references it -- so a session scrolled through hundreds of
    // sixel frames pinned one full-resolution texture per frame with no ceiling at all.
    auto constexpr Span = GridSize { .lines = LineCount(1), .columns = ColumnCount(1) };
    auto constexpr ImageBytes = size_t { 10 * 20 * 4 }; // one cell at 4 bytes per pixel

    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    // A budget of two images, so the third must evict the first.
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize, 2 * ImageBytes };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto& backend = renderTarget.getMockImageBackend();

    // Three images, each drawn in its own frame: by the third, the first is the least recently used.
    for (auto const index: std::views::iota(1u, 4u))
        renderRow(imageRenderer, makeImage(Span, ImageLayer::Replace, ImageId(index)), 1);

    REQUIRE(backend.createCommands.size() == 3);
    REQUIRE(backend.destroyCommands.size() == 1);
    CHECK(backend.destroyCommands.front().id == backend.createCommands.front().id); // the oldest went
}

TEST_CASE("ImageRenderer.never evicts an image the current frame draws", "[image][renderer]")
{
    // Eviction runs while the frame is still being built, and this frame's quads already name their
    // texture by id -- releasing one would drop it from the very frame it is visible in, and it would
    // be re-uploaded next frame only to be dropped again. A working set over budget must overshoot
    // rather than thrash.
    auto constexpr Span = GridSize { .lines = LineCount(1), .columns = ColumnCount(1) };
    auto constexpr ImageBytes = size_t { 10 * 20 * 4 };

    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize, ImageBytes }; // room for one
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    // Three distinct images within ONE frame, all over budget together.
    auto const images = std::array {
        makeImage(Span, ImageLayer::Replace, ImageId(1)),
        makeImage(Span, ImageLayer::Replace, ImageId(2)),
        makeImage(Span, ImageLayer::Replace, ImageId(3)),
    };

    imageRenderer.beginFrame();
    for (auto const [index, image]: crispy::views::enumerate(images))
    {
        // Each image spans one cell, so the fragment names offset (0,0) WITHIN ITS OWN image; only
        // where it lands on the grid differs. An offset outside the image is an alignment gap, which
        // renderImage() drops before it ever asks for a texture.
        auto const fragment =
            ImageFragment { image, CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) } };
        imageRenderer.renderImage(
            crispy::point { .x = static_cast<int>(index) * unbox<int>(CellSize.width), .y = 0 }, fragment);
    }
    imageRenderer.endFrame();

    auto& backend = renderTarget.getMockImageBackend();
    CHECK(backend.createCommands.size() == 3);
    CHECK(backend.destroyCommands.empty()); // all three are drawn by this frame: none may go

    // Once a later frame draws none of them, the budget applies again.
    renderRow(imageRenderer, makeImage(Span, ImageLayer::Replace, ImageId(4)), 1);
    CHECK(backend.destroyCommands.size() == 3);
}

TEST_CASE("ImageRenderer.re-uploads an evicted image when it is seen again", "[image][renderer]")
{
    // Eviction is a cache miss, not a loss: the mapping is dropped so the next sight uploads again.
    auto constexpr Span = GridSize { .lines = LineCount(1), .columns = ColumnCount(1) };
    auto constexpr ImageBytes = size_t { 10 * 20 * 4 };

    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize, ImageBytes }; // room for one
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto const first = makeImage(Span, ImageLayer::Replace, ImageId(1));
    renderRow(imageRenderer, first, 1);
    renderRow(imageRenderer, makeImage(Span, ImageLayer::Replace, ImageId(2)), 1); // evicts the first

    auto& backend = renderTarget.getMockImageBackend();
    REQUIRE(backend.destroyCommands.size() == 1);

    renderRow(imageRenderer, first, 1);
    CHECK(backend.createCommands.size() == 3); // uploaded again rather than silently missing
}

TEST_CASE("ImageRenderer.clearCache releases the textures", "[image][renderer]")
{
    // "The textures belong to the render target, so a new one starts with none of them" holds only
    // for the setRenderTarget() caller. Renderer::updateFontMetrics() clears on a target that is
    // still very much alive: dropping the ids alone stranded one texture per cached image on the GPU
    // with nothing left able to name it, so every font-size step leaked the whole resident set.
    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };
    auto imageRenderer = ImageRenderer { testGridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);

    auto const image = makeImage(GridSize { .lines = LineCount(1), .columns = ColumnCount(2) });
    renderRow(imageRenderer, image, 2);

    auto& backend = renderTarget.getMockImageBackend();
    REQUIRE(backend.createCommands.size() == 1);

    imageRenderer.clearCache();

    REQUIRE(backend.destroyCommands.size() == 1);
    CHECK(backend.destroyCommands.front().id == backend.createCommands.front().id);

    // The mapping went with it, so the image uploads again rather than naming a destroyed texture.
    renderRow(imageRenderer, image, 2);
    CHECK(backend.createCommands.size() == 2);
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
