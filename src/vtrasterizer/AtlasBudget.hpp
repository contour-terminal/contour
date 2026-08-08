// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Primitives.hpp>

#include <crispy/StrongLRUHashtable.hpp>
#include <crispy/Utils.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>

/// How large a glyph atlas has to be for the page it is rendering.
///
/// This is a correctness bound, not a tuning knob. A tile's location is handed out by the atlas' LRU when
/// the tile is created, and its normalized texture coordinates are baked into the vertex data there and
/// then — while the tile's pixels are only uploaded at the end of the frame. So if a single frame needs
/// more distinct tiles than the atlas holds, a slot is recycled *within* that frame: two glyphs upload to
/// the same location, the last writer wins, and every quad already recorded against that location samples
/// the wrong glyph. Text renders as other text.
///
/// The budget must therefore cover the whole page, and it must be revisited when the page grows — a pane
/// created small (which a freshly split pane always is) and then grown by a window resize would otherwise
/// keep its small-page atlas.
///
/// Pure and dependency-free so the policy is unit-testable without a font, a GPU or a render target.
namespace vtrasterizer::atlasbudget
{

/// Headroom multiplier over the page's own cell count.
///
/// Sixel images allocate atlas tiles on top of the page's glyphs, and a single image can be far larger
/// than the page it is drawn into.
constexpr uint32_t PageHeadroomFactor = 3;

/// Tiles the atlas must hold to render @p pageSize without recycling a slot mid-frame.
/// @param configured The tile count the configuration asked for; the budget never falls below it.
/// @param pageSize   The page the renderer is sizing for.
/// @return The effective tile budget.
[[nodiscard]] constexpr crispy::LRUCapacity tileCountFor(crispy::LRUCapacity configured,
                                                         vtbackend::PageSize pageSize) noexcept
{
    auto const cells = pageSize.area();
    auto const required = cells > 0 ? static_cast<uint32_t>(cells) * PageHeadroomFactor : 0u;
    return crispy::LRUCapacity { std::max(configured.value, required) };
}

/// Tiles the atlas spends on top of the budget: slot 0, which is never handed out, plus every
/// direct-mapped tile. atlas::computeAtlasSize() rounds `1 + tileCount + directMappingCount` up to a
/// power of two, and both functions below need that same total to reason about the texture.
/// @param directMappingCount Tiles reserved for direct mapping.
/// @return The tiles the budget does not get.
[[nodiscard]] constexpr uint32_t reservedTileCount(uint32_t directMappingCount) noexcept
{
    return 1 + directMappingCount;
}

/// Rounds @p tileCount up to the atlas' own allocation granularity.
///
/// atlas::computeAtlasSize() rounds the total tile count up to a power of two, so every budget within
/// one power-of-two band yields the SAME texture — growing to a value inside the current band buys
/// nothing and costs a full rebuild. Snapping to the top of the band makes Renderer's grow-only guard
/// hold for every page size in it, which is what keeps an enlarging resize drag from reallocating the
/// atlas and flushing every renderable cache once per added row or column.
/// @param tileCount          The budget the page asked for.
/// @param directMappingCount Tiles reserved for direct mapping.
/// @return The budget rounded up to the top of its band.
[[nodiscard]] constexpr crispy::LRUCapacity quantizedTileCountFor(crispy::LRUCapacity tileCount,
                                                                  uint32_t directMappingCount) noexcept
{
    auto const reserved = reservedTileCount(directMappingCount);
    return crispy::LRUCapacity { crispy::nextPowerOfTwo(tileCount.value + reserved) - reserved };
}

/// Largest atlas texture edge, in pixels, a budget may lead to.
///
/// The page-derived budget is a lower bound, but the GPU imposes an upper one, and the atlas has no
/// failure path: a texture larger than the driver's maximum simply does not allocate, and the pane then
/// renders no glyphs at all — strictly worse than the mid-frame tile recycling the budget exists to
/// prevent. 8192 is supported by every backend Contour targets (GL 4.x and D3D11 feature level 11_0
/// both guarantee 16384; Vulkan guarantees 4096 but no desktop driver reports less than 8192), while
/// 16384 — which a maximized window on a 5K display reaches — is exactly the value many GPUs report as
/// their maximum and therefore the value most likely to fail.
constexpr uint32_t MaxAtlasTextureEdge = 8192;

/// The largest tile budget whose atlas texture stays within @p maxTextureEdge on both edges.
///
/// The exact inverse of atlas::computeAtlasSize(): that function takes the total tile count to a power
/// of two P, lays the tiles out in a ceil(sqrt(P)) square and rounds each pixel edge up to a power of
/// two. `ceil(sqrt(P)) * tileEdge <= maxTextureEdge` is therefore the whole constraint, since rounding
/// a value at or below a power of two up to a power of two cannot exceed it.
/// @param tileSize           The atlas tile size, i.e. one grid cell in device pixels.
/// @param directMappingCount Tiles reserved for direct mapping.
/// @param maxTextureEdge     The largest texture edge to allow, in pixels; must be a power of two.
/// @return The tile budget ceiling; never zero, so the atlas stays constructible.
[[nodiscard]] constexpr crispy::LRUCapacity maxTileCountFor(vtbackend::ImageSize tileSize,
                                                            uint32_t directMappingCount,
                                                            uint32_t maxTextureEdge) noexcept
{
    auto const tileEdge = std::max(unbox<uint32_t>(tileSize.width), unbox<uint32_t>(tileSize.height));
    auto const squareEdge = tileEdge != 0 ? maxTextureEdge / tileEdge : 0u;
    auto const totalTiles = squareEdge != 0 ? std::bit_floor(squareEdge * squareEdge) : 0u;
    auto const reserved = reservedTileCount(directMappingCount);
    return crispy::LRUCapacity { totalTiles > reserved ? totalTiles - reserved : 1u };
}

/// Hashtable slots to map @p tileCount tiles with.
///
/// Kept proportional to the tile count. The slot count used to stay pinned at whatever the
/// configuration named while the tile count was raised to the page area, so a large page chained
/// heavily through a small table.
/// @param configured The slot count the configuration asked for; the result never falls below it.
/// @param tileCount  The effective tile budget the atlas is being built for.
/// @return The slot count, rounded up to the power of two the atlas requires.
[[nodiscard]] constexpr crispy::StrongHashtableSize slotCountFor(crispy::StrongHashtableSize configured,
                                                                 crispy::LRUCapacity tileCount) noexcept
{
    return crispy::StrongHashtableSize { crispy::nextPowerOfTwo(
        std::max(configured.value, tileCount.value)) };
}

} // namespace vtrasterizer::atlasbudget
