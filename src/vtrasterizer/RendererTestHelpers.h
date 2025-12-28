// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <vector>

namespace vtrasterizer
{

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
