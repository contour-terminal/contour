// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowShadow.hpp>
#include <contour/platform/WindowDecoration.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace contour::platform
{

using config::ShadowSize;

/// One tile of the nine-patch a compositor assembles the shadow from.
///
/// The enumerator ORDER is the wire order of `_KDE_NET_WM_SHADOW`'s eight pixmap slots
/// (kwin/src/shadow.h, `enum ShadowElements`), so the X11 adapter can index this array straight
/// into the property. Wayland issues eight separately named `attach_*` requests and reaches them
/// through a table keyed on this enum.
enum class ShadowTile : uint8_t
{
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    TopLeft,
};

inline constexpr auto ShadowTileCount = size_t { 8 };

/// Every tile, in wire order.
///
/// The list itself, so the adapters and the tests iterate one table rather than each spelling out
/// eight enumerators -- which is what would otherwise make "add a ninth" a three-file edit.
inline constexpr auto AllShadowTiles = std::array<ShadowTile, ShadowTileCount> {
    ShadowTile::Top,    ShadowTile::TopRight,   ShadowTile::Right, ShadowTile::BottomRight,
    ShadowTile::Bottom, ShadowTile::BottomLeft, ShadowTile::Left,  ShadowTile::TopLeft,
};

/// One blurred layer of the composite.
struct ShadowLayer
{
    int offsetY = 0;      ///< Vertical displacement of this layer alone, downward.
    int radius = 0;       ///< CSS-style blur radius. The VISIBLE reach is @ref blurExtentFor.
    double opacity = 0.0; ///< Peak alpha of this layer, 0..1.

    [[nodiscard]] constexpr bool operator==(ShadowLayer const&) const noexcept = default;
};

/// What one @ref ShadowSize resolves to, in logical pixels.
///
/// Two layers, not one: Breeze composites a broad soft shadow with a tighter, fainter one lifted
/// slightly upward, and the pair is what gives the result its shape. A single Gaussian at the same
/// radius reads as a flat grey halo next to it.
struct ShadowMetrics
{
    int offsetY = 0;          ///< Displacement of the whole shadow, downward.
    ShadowLayer primary {};   ///< The broad layer that gives the shadow its reach.
    ShadowLayer secondary {}; ///< A tighter, fainter layer, lifted so the top edge stays crisp.

    [[nodiscard]] constexpr bool operator==(ShadowMetrics const&) const noexcept = default;
};

namespace detail
{
    /// Breeze's own shadow parameters, in enumerator order.
    ///
    /// Copied verbatim from `s_shadowParams` in breeze/kdecoration/breezedecoration.cpp rather than
    /// invented, so a Contour window and a Breeze-decorated window next to it cast the same shadow.
    inline constexpr auto ShadowMetricsTable = std::array<ShadowMetrics, 5> {
        // None
        ShadowMetrics {},
        // Small
        ShadowMetrics { .offsetY = 4,
                        .primary = { .offsetY = 0, .radius = 16, .opacity = 1.0 },
                        .secondary = { .offsetY = -2, .radius = 8, .opacity = 0.4 } },
        // Medium
        ShadowMetrics { .offsetY = 8,
                        .primary = { .offsetY = 0, .radius = 32, .opacity = 0.9 },
                        .secondary = { .offsetY = -4, .radius = 16, .opacity = 0.3 } },
        // Large
        ShadowMetrics { .offsetY = 12,
                        .primary = { .offsetY = 0, .radius = 48, .opacity = 0.8 },
                        .secondary = { .offsetY = -6, .radius = 24, .opacity = 0.2 } },
        // VeryLarge
        ShadowMetrics { .offsetY = 16,
                        .primary = { .offsetY = 0, .radius = 64, .opacity = 0.7 },
                        .secondary = { .offsetY = -8, .radius = 32, .opacity = 0.1 } },
    };

    /// How far the shadow tucks UNDER the window, so no seam shows at the edge.
    /// `Metrics::Shadow_Overlap` in breeze/kdecoration/breeze.h.
    inline constexpr auto ShadowOverlap = 3;

    /// Converts a CSS blur radius to a Gaussian standard deviation, then that to a pixel reach.
    ///
    /// Both steps are Breeze's (breezeboxshadowrenderer.cpp), which in turn are the SVG and CSS
    /// specifications': stdDev = radius / 2, and the reach is stdDev * 3 * sqrt(2*pi) / 4 * 1.5.
    /// The constant is spelled out because std::sqrt is not usable in a constant expression.
    inline constexpr auto GaussianScaleFactor = 2.8199568089598753;

    /// Rounds a NON-NEGATIVE @p value to the nearest integer.
    ///
    /// Spelled out rather than reached for: std::lround is not usable in a constant expression with
    /// the standard libraries this builds against, and the obvious `static_cast<int>(value + 0.5)`
    /// is a documented rounding bug -- for values just below .5 the addition can carry in binary
    /// floating point and round the wrong way.
    [[nodiscard]] constexpr int roundToInt(double value) noexcept
    {
        auto const truncated = static_cast<int>(value);
        return (value - truncated) >= 0.5 ? truncated + 1 : truncated;
    }
} // namespace detail

/// The visible reach of a blur of @p radius, in logical pixels.
[[nodiscard]] constexpr int blurExtentFor(int radius) noexcept
{
    auto const extent = detail::roundToInt(static_cast<double>(radius) * 0.5 * detail::GaussianScaleFactor);
    return std::max(2, extent);
}

/// The parameters @p size is drawn with.
[[nodiscard]] constexpr ShadowMetrics shadowMetricsFor(ShadowSize size) noexcept
{
    return detail::ShadowMetricsTable[static_cast<size_t>(size)];
}

/// How far the shadow reaches beyond the window, per side, in logical pixels.
struct ShadowEdges
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] constexpr bool operator==(ShadowEdges const&) const noexcept = default;
};

/// One tile's rectangle within the rendered atlas.
struct ShadowTileRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(ShadowTileRect const&) const noexcept = default;
};

/// Everything the adapters need: where the shadow reaches, and how to cut the atlas into tiles.
///
/// There is ONE unit here, deliberately. KWin measures the shadow's outer rectangle in its own
/// logical coordinates but takes the tile sizes straight from the images' pixel dimensions
/// (`Shadow::elementSize` returns `m_shadowElements[element].size()`, and ShadowItem lays those out
/// against a logical rect) -- on both the X11 and the Wayland path. So a tile rendered at 2x would
/// simply be drawn twice as large. Everything below is therefore logical pixels, and the tiles are
/// rendered at 1x whatever the screen's scale. The cost is a slightly soft shadow on a HiDPI
/// screen; the alternative is a shadow of visibly wrong SIZE, which is worse.
struct ShadowGeometry
{
    ShadowEdges offsets {};
    std::array<ShadowTileRect, ShadowTileCount> tiles {};
    int atlasWidth = 0;
    int atlasHeight = 0;

    /// Side of the synthetic square window the atlas is the shadow of.
    ///
    /// Kept rather than left to be recovered as `atlasHeight - offsets.top - offsets.bottom`: the
    /// renderer needs it, and inverting the layout formula at a distance is a second place that has
    /// to agree with this one.
    int windowExtent = 0;

    [[nodiscard]] constexpr bool operator==(ShadowGeometry const&) const noexcept = default;
};

/// Indexes @p tiles by @p tile.
[[nodiscard]] constexpr ShadowTileRect const& tileRect(ShadowGeometry const& geometry,
                                                       ShadowTile tile) noexcept
{
    return geometry.tiles[static_cast<size_t>(tile)];
}

/// The atlas layout and shadow reach for @p metrics.
///
/// The atlas is the complete shadow of a synthetic square window just large enough that the middle
/// of each edge has settled into a one-dimensional profile -- which is what lets the four edge
/// tiles be one pixel wide and stretched by the compositor. The corner tiles carry that settling
/// distance with them, so the stretch always begins where the profile has stopped changing.
[[nodiscard]] constexpr ShadowGeometry shadowGeometryFor(ShadowMetrics metrics) noexcept
{
    if (metrics == ShadowMetrics {})
        return {};

    // The layer that reaches furthest decides the margin, exactly as Breeze's
    // calculateMinimumShadowTextureSize does when it sizes the texture for the wider of the two.
    auto const reach =
        std::max(blurExtentFor(metrics.primary.radius) + std::abs(metrics.primary.offsetY),
                 blurExtentFor(metrics.secondary.radius) + std::abs(metrics.secondary.offsetY));

    auto const sideways = std::max(0, reach - detail::ShadowOverlap);
    auto const offsets = ShadowEdges {
        .left = sideways,
        .top = std::max(0, reach - detail::ShadowOverlap - metrics.offsetY),
        .right = sideways,
        .bottom = std::max(0, reach - detail::ShadowOverlap + metrics.offsetY),
    };

    // How much straight edge each corner tile carries beyond the window's corner. One full reach:
    // that is the distance over which a corner still bends the shadow's profile, so beyond it the
    // edge is uniform and safe to stretch.
    auto const corner = reach;

    // The synthetic window the atlas is the shadow of: square, and just wide enough for the two
    // corners plus the single pixel column the edge tiles are cut from.
    auto const windowExtent = (2 * corner) + 1;

    auto const atlasWidth = offsets.left + windowExtent + offsets.right;
    auto const atlasHeight = offsets.top + windowExtent + offsets.bottom;

    // The one-pixel row/column at the exact middle of each edge, where the profile has settled.
    auto const midX = offsets.left + corner;
    auto const midY = offsets.top + corner;

    auto tiles = std::array<ShadowTileRect, ShadowTileCount> {};
    auto at = [&tiles](ShadowTile tile) -> ShadowTileRect& {
        return tiles[static_cast<size_t>(tile)];
    };

    at(ShadowTile::TopLeft) = {
        .x = 0, .y = 0, .width = offsets.left + corner, .height = offsets.top + corner
    };
    at(ShadowTile::Top) = { .x = midX, .y = 0, .width = 1, .height = offsets.top };
    at(ShadowTile::TopRight) = { .x = atlasWidth - (offsets.right + corner),
                                 .y = 0,
                                 .width = offsets.right + corner,
                                 .height = offsets.top + corner };
    at(ShadowTile::Right) = {
        .x = atlasWidth - offsets.right, .y = midY, .width = offsets.right, .height = 1
    };
    at(ShadowTile::BottomRight) = { .x = atlasWidth - (offsets.right + corner),
                                    .y = atlasHeight - (offsets.bottom + corner),
                                    .width = offsets.right + corner,
                                    .height = offsets.bottom + corner };
    at(ShadowTile::Bottom) = {
        .x = midX, .y = atlasHeight - offsets.bottom, .width = 1, .height = offsets.bottom
    };
    at(ShadowTile::BottomLeft) = { .x = 0,
                                   .y = atlasHeight - (offsets.bottom + corner),
                                   .width = offsets.left + corner,
                                   .height = offsets.bottom + corner };
    at(ShadowTile::Left) = { .x = 0, .y = midY, .width = offsets.left, .height = 1 };

    return ShadowGeometry { .offsets = offsets,
                            .tiles = tiles,
                            .atlasWidth = atlasWidth,
                            .atlasHeight = atlasHeight,
                            .windowExtent = windowExtent };
}

/// Whether a window in a given state should carry a compositor drop shadow.
enum class ShadowVisibility : uint8_t
{
    Hidden = 0,
    Shown,
};

/// How the window is currently presented.
///
/// Four states rather than a bool, because three of them mean "no shadow" for three different
/// reasons and a fifth (say, a compositor's own half-screen mode) has to be able to join them.
enum class WindowPresentation : uint8_t
{
    Windowed,
    Maximized,
    FullScreen,
    Tiled,
};

/// Whether to publish a shadow for a window in this state.
///
/// A server-side-decorated window already has the window manager's shadow; publishing a second one
/// stacks them. A window that fills its screen or its tile has no outside for a shadow to fall on,
/// and a maximized window that publishes one makes the compositor reserve space beyond the work
/// area for it.
[[nodiscard]] constexpr ShadowVisibility shadowVisibilityFor(WindowPresentation presentation,
                                                             WindowDecoration decoration,
                                                             ShadowSize size) noexcept
{
    if (size == ShadowSize::None || decoration == WindowDecoration::Server)
        return ShadowVisibility::Hidden;

    switch (presentation)
    {
        case WindowPresentation::Windowed: return ShadowVisibility::Shown;
        case WindowPresentation::Maximized:
        case WindowPresentation::FullScreen:
        case WindowPresentation::Tiled: return ShadowVisibility::Hidden;
    }
    return ShadowVisibility::Hidden;
}

} // namespace contour::platform
