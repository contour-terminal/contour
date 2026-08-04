// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Primitives.hpp>

#include <crispy/StrongLRUHashtable.hpp>

#include <algorithm>
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
