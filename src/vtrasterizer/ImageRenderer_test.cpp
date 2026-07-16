// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>

#include <vtrasterizer/ImageRenderer.h>
#include <vtrasterizer/RendererTestHelpers.h>

#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <ranges>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// Builds a rasterized image spanning @p cellSpan cells, each pixel row carrying a distinct value so
/// that any two cells of the image have provably different content.
std::shared_ptr<RasterizedImage> makeGradientImage(GridSize cellSpan, ImageSize cellSize)
{
    auto const imageSize = ImageSize { Width::cast_from(unbox(cellSpan.columns) * unbox(cellSize.width)),
                                       Height::cast_from(unbox(cellSpan.lines) * unbox(cellSize.height)) };
    auto data = Image::Data(imageSize.area() * 4, 0);

    // A vertical gradient: every pixel row gets its own byte value, so a cell taking another cell's
    // texels shows up as a wrong value rather than as a coincidence.
    for (auto const y: std::views::iota(0u, unbox<unsigned>(imageSize.height)))
        for (auto const x: std::views::iota(0u, unbox<unsigned>(imageSize.width)))
        {
            auto const base = ((y * unbox<unsigned>(imageSize.width)) + x) * 4;
            data[base + 0] = static_cast<uint8_t>(y & 0xFF);
            data[base + 1] = static_cast<uint8_t>(x & 0xFF);
            data[base + 2] = 0x40;
            data[base + 3] = 0xFF;
        }

    auto image =
        std::make_shared<Image>(ImageId(1), ImageFormat::RGBA, std::move(data), imageSize, [](auto) {});

    return std::make_shared<RasterizedImage>(std::move(image),
                                             ImageAlignment::TopStart,
                                             ImageResize::NoResize,
                                             RGBAColor { 0, 0, 0, 0xFF },
                                             cellSpan,
                                             cellSize);
}

} // namespace

TEST_CASE("ImageRenderer.atlas_reuses_tile_locations_within_a_frame", "[image][renderer]")
{
    // An image is sliced into one atlas tile per grid cell, and those tiles live in an LRU whose
    // capacity is the atlas texture divided by the cell size. An image needing more cells than the
    // atlas holds therefore evicts its own tiles while drawing itself -- and eviction hands the
    // evicted entry's tile LOCATION straight to the next tile.
    //
    // That would be survivable if each quad were drawn against the atlas as it stood when the quad
    // was scheduled. It is not: renderTile() only queues, and the backend applies every upload of
    // the frame before any draw of that frame. So two cells sharing a location both sample whatever
    // was written there last, and all but the final writer render the wrong pixels.
    //
    // This is what makes a full-screen sixel come out as a repeating garble rather than an image.
    // It documents current behaviour and must be rewritten once images no longer go through the
    // per-cell atlas.
    auto constexpr CellSize = ImageSize { Width(10), Height(20) };
    auto const gridMetrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                           .cellSize = CellSize,
                                           .baseline = 15,
                                           .underline = { .position = 17, .thickness = 1 } };

    auto renderTarget = MockRenderTarget {};
    auto directMappingAllocator = Renderable::DirectMappingAllocator { 0 };

    // A deliberately small atlas: tileCount is only a floor on the texture size, so the capacity
    // that actually applies is (tilesInX * tilesInY) - directMappingCount - 1.
    auto atlas = Renderable::TextureAtlas { renderTarget.textureScheduler(),
                                            atlas::AtlasProperties {
                                                .format = atlas::Format::RGBA,
                                                .tileSize = CellSize,
                                                .hashCount = crispy::strong_hashtable_size { 64 },
                                                .tileCount = crispy::lru_capacity { 4 },
                                                .directMappingCount = 0,
                                            } };

    auto const capacity = static_cast<unsigned>(atlas.capacity());
    INFO("atlas capacity: " << capacity);
    REQUIRE(capacity > 0);

    // Ask for more cells than the atlas can hold, exactly as a full-screen image does.
    auto const cellCount = capacity * 2;
    auto const cellSpan = GridSize { .lines = LineCount(1), .columns = ColumnCount::cast_from(cellCount) };
    auto const rasterizedImage = makeGradientImage(cellSpan, CellSize);

    auto imageRenderer = ImageRenderer { gridMetrics, CellSize };
    imageRenderer.setRenderTarget(renderTarget, directMappingAllocator);
    imageRenderer.setTextureAtlas(atlas);

    imageRenderer.beginFrame();
    for (auto const column: std::views::iota(0u, cellCount))
    {
        auto const fragment = ImageFragment {
            rasterizedImage, CellLocation { .line = LineOffset(0), .column = ColumnOffset::cast_from(column) }
        };
        imageRenderer.renderImage(crispy::point { .x = static_cast<int>(column * 10), .y = 0 }, fragment);
    }
    imageRenderer.endFrame();

    auto& backend = renderTarget.getMockBackend();

    // Every cell is a unique key, so every cell misses and uploads.
    CHECK(backend.uploadCommands.size() == cellCount);

    // The atlas cannot hold them: at least two uploads in this one frame landed on the same texels,
    // carrying different pixels. Whichever quad drew first now samples the other's content.
    auto uploadsByLocation = std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> {};
    for (auto const [index, upload]: crispy::views::enumerate(backend.uploadCommands))
        uploadsByLocation[{ upload.location.x.value, upload.location.y.value }].emplace_back(
            static_cast<size_t>(index));

    auto collidingLocations = 0u;
    for (auto const& [location, uploadIndices]: uploadsByLocation)
    {
        if (uploadIndices.size() < 2)
            continue;
        ++collidingLocations;
        // The colliding uploads carry genuinely different pixels -- this is data loss, not a
        // harmless re-upload of identical content.
        auto const& first = backend.uploadCommands[uploadIndices.front()].bitmap;
        auto const& last = backend.uploadCommands[uploadIndices.back()].bitmap;
        CHECK(first != last);
    }

    INFO("colliding tile locations: " << collidingLocations);
    CHECK(collidingLocations > 0);
}
