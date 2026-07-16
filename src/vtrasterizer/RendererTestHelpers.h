// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <vector>

namespace vtrasterizer
{

/// Records every command a Renderable schedules, so tests can assert on what would reach the GPU.
///
/// Uploads are recorded alongside draws because the two only mean something together: a tile's
/// pixels and the quads that sample it are scheduled separately, and the atlas is free to hand the
/// same location to two different tiles within one frame.
class MockAtlasBackend: public vtrasterizer::atlas::AtlasBackend
{
  public:
    std::vector<vtrasterizer::atlas::RenderTile> renderCommands;
    std::vector<vtrasterizer::atlas::UploadTile> uploadCommands;

    [[nodiscard]] vtbackend::ImageSize atlasSize() const noexcept override
    {
        return vtbackend::ImageSize { vtbackend::Width(1024), vtbackend::Height(1024) };
    }

    void configureAtlas(vtrasterizer::atlas::ConfigureAtlas) override {}
    void uploadTile(vtrasterizer::atlas::UploadTile tile) override
    {
        uploadCommands.emplace_back(std::move(tile));
    }
    void renderTile(vtrasterizer::atlas::RenderTile tile) override { renderCommands.emplace_back(tile); }
};

/// A headless RenderTarget whose texture scheduler is a MockAtlasBackend.
class MockRenderTarget: public vtrasterizer::RenderTarget
{
  public:
    void setRenderSize(vtbackend::ImageSize size) override { _size = size; }
    [[nodiscard]] vtbackend::ImageSize renderSize() const noexcept override { return _size; }
    void setMargin(vtrasterizer::PageMargin) override {}
    vtrasterizer::atlas::AtlasBackend& textureScheduler() override { return _textureScheduler; }
    MockAtlasBackend& getMockBackend() { return _textureScheduler; }

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
