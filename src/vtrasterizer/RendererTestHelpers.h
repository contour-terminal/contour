// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

namespace vtrasterizer
{

/// Records every command a Renderable schedules, so tests can assert on what would reach the GPU.
///
/// Uploads are recorded alongside draws because the two only mean something together: a tile's
/// pixels and the quads that sample it are scheduled separately, and the atlas is free to hand the
/// same location to two different tiles within one frame.
///
/// The atlas is a fixed GRID: TextureAtlas spaces tile origins exactly @c tileSize apart, so a
/// bitmap larger than one tile does not merely look wrong -- it is a write into the neighbouring
/// tile, silently corrupting an unrelated glyph. This mock therefore enforces that bound the way
/// the real backend cannot: RhiRenderer only logs the violation and uploads anyway.
///
/// Recording uploads without checking them is what let a regression that flooded the real renderer
/// with hundreds of overflows pass the entire suite.
class MockAtlasBackend: public vtrasterizer::atlas::AtlasBackend
{
  public:
    std::vector<vtrasterizer::atlas::RenderTile> renderCommands;
    std::vector<vtrasterizer::atlas::UploadTile> uploadCommands;

    /// Indices into @c uploadCommands of uploads that did not fit the configured tile. Indices
    /// rather than pointers: uploadCommands reallocates as it grows.
    ///
    /// These are asserted on as they arrive, so a test need not opt in; this is for tests that
    /// want to name the violation.
    std::vector<size_t> oversizedUploads;

    [[nodiscard]] vtbackend::ImageSize atlasSize() const noexcept override { return _atlasSize; }

    /// The tile size the atlas last configured, or a zero size if it never did.
    [[nodiscard]] vtbackend::ImageSize tileSize() const noexcept { return _properties.tileSize; }

    void configureAtlas(vtrasterizer::atlas::ConfigureAtlas atlas) override
    {
        _atlasSize = atlas.size;
        _properties = atlas.properties;
    }

    void uploadTile(vtrasterizer::atlas::UploadTile tile) override
    {
        // A backend that was never configured has no bound to enforce; some tests drive a
        // Renderable directly without an atlas.
        auto const configured =
            unbox(_properties.tileSize.width) != 0 && unbox(_properties.tileSize.height) != 0;
        auto const fits = !configured
                          || (tile.bitmapSize.width <= _properties.tileSize.width
                              && tile.bitmapSize.height <= _properties.tileSize.height);

        INFO(std::format("tile {} vs atlas tile {}", tile.bitmapSize, _properties.tileSize));
        CHECK(fits);

        uploadCommands.emplace_back(std::move(tile));
        if (!fits)
            oversizedUploads.emplace_back(uploadCommands.size() - 1);
    }

    void renderTile(vtrasterizer::atlas::RenderTile tile) override { renderCommands.emplace_back(tile); }

  private:
    vtbackend::ImageSize _atlasSize { vtbackend::Width(1024), vtbackend::Height(1024) };
    vtrasterizer::atlas::AtlasProperties _properties {};
};

/// Records every whole-image command a Renderable schedules.
class MockImageTextureBackend: public vtrasterizer::atlas::ImageTextureBackend
{
  public:
    std::vector<vtrasterizer::atlas::CreateImageTexture> createCommands;
    std::vector<vtrasterizer::atlas::DestroyImageTexture> destroyCommands;
    std::vector<vtrasterizer::atlas::RenderImageQuad> quadCommands;

    void createImageTexture(vtrasterizer::atlas::CreateImageTexture param) override
    {
        createCommands.emplace_back(std::move(param));
    }
    void destroyImageTexture(vtrasterizer::atlas::DestroyImageTexture param) override
    {
        destroyCommands.emplace_back(param);
    }
    void renderImageQuad(vtrasterizer::atlas::RenderImageQuad param) override
    {
        quadCommands.emplace_back(param);
        drawOrder.emplace_back(param);
    }

    void renderImageGap(vtrasterizer::atlas::RenderImageGap param) override
    {
        gapCommands.emplace_back(param);
        drawOrder.emplace_back(param);
    }

    std::vector<vtrasterizer::atlas::RenderImageGap> gapCommands;

    /// Quads and gap fills as issued, interleaved -- what composites is the ORDER, so a test that only
    /// counted them could not tell a gap drawn over the image from one drawn under it.
    std::vector<std::variant<vtrasterizer::atlas::RenderImageQuad, vtrasterizer::atlas::RenderImageGap>>
        drawOrder;

    /// Ids the next takeFailedImageTextures() call reports, standing in for a backend that could not
    /// create the texture (out of GPU memory, no RHI yet).
    std::vector<vtrasterizer::atlas::ImageTextureId> failedImageTextures;

    [[nodiscard]] std::vector<vtrasterizer::atlas::ImageTextureId> takeFailedImageTextures() override
    {
        return std::exchange(failedImageTextures, {});
    }
};

/// A headless RenderTarget whose texture scheduler is a MockAtlasBackend.
class MockRenderTarget: public vtrasterizer::RenderTarget
{
  public:
    void setRenderSize(vtbackend::ImageSize size) override { _size = size; }
    [[nodiscard]] vtbackend::ImageSize renderSize() const noexcept override { return _size; }
    void setMargin(vtrasterizer::PageMargin) override {}
    vtrasterizer::atlas::AtlasBackend& textureScheduler() override { return _textureScheduler; }
    vtrasterizer::atlas::ImageTextureBackend& imageScheduler() override { return _imageScheduler; }
    MockAtlasBackend& getMockBackend() { return _textureScheduler; }
    MockImageTextureBackend& getMockImageBackend() { return _imageScheduler; }

    void renderRectangle(int, int, vtbackend::Width, vtbackend::Height, vtbackend::RGBAColor) override {}
    void setScissorRect(int, int, int, int) override {}
    void clearScissorRect() override {}
    void scheduleScreenshot(ScreenshotCallback) override {}
    void execute(std::chrono::steady_clock::time_point) override {}
    void clearCache() override {}
    std::optional<vtrasterizer::AtlasTextureScreenshot> readAtlas() override { return std::nullopt; }
    void inspect(std::ostream&) const override {}

  private:
    vtbackend::ImageSize _size {};
    MockAtlasBackend _textureScheduler;
    MockImageTextureBackend _imageScheduler;
};

template <typename TileCreateData>
void verifyBitmap(TileCreateData const& tileData, std::vector<std::string> const& pattern)
{
    // Check dimensions
    REQUIRE(tileData.bitmapSize.height.template as<int>() == static_cast<int>(pattern.size()));
    if (!pattern.empty())
        REQUIRE(tileData.bitmapSize.width.template as<int>() == static_cast<int>(pattern[0].size()));

    auto const width = tileData.bitmapSize.width.template as<size_t>();
    auto const height = tileData.bitmapSize.height.template as<size_t>();
    auto const componentCount = atlas::element_count(tileData.bitmapFormat);

    // Assuming AlphaMask (Red) format from BDF / BoxDrawing
    REQUIRE(tileData.bitmapFormat == atlas::Format::Red);

    for (auto const y: std::views::iota(0zu, height))
    {
        std::string actualRow;
        actualRow.reserve(width);

        for (auto const x: std::views::iota(0zu, width))
        {
            auto const pixelIndex = (y * width + x) * componentCount;
            auto const pixelValue = tileData.bitmap[pixelIndex];
            actualRow += (pixelValue > 0 ? '#' : '.');
        }

        INFO("Row: " << y);
        CHECK(actualRow == pattern[y]);
    }
}

} // namespace vtrasterizer
