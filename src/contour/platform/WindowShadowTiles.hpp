// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowShadowGeometry.hpp>

#include <QtGui/QColor>
#include <QtGui/QImage>

#include <array>

namespace contour::platform
{

/// The eight images a compositor assembles a window's drop shadow from, and their layout.
///
/// The images are `QImage::Format_ARGB32_Premultiplied`, which is what painting them produces and
/// what Wayland's `wl_shm` ARGB8888 expects. The X11 adapter converts: KWin reads an X pixmap back
/// as plain `QImage::Format_ARGB32` (kwin/src/shadow.cpp), so handing it premultiplied bytes draws
/// a visibly too-dark shadow.
struct WindowShadowTiles
{
    ShadowGeometry geometry {};
    std::array<QImage, ShadowTileCount> tiles {};

    /// Whether there is anything to publish. False for @ref ShadowSize::None.
    [[nodiscard]] bool isEmpty() const noexcept { return geometry.atlasWidth == 0; }
};

/// Indexes @p tiles by @p tile.
[[nodiscard]] inline QImage const& tileImage(WindowShadowTiles const& tiles, ShadowTile tile) noexcept
{
    return tiles.tiles[static_cast<size_t>(tile)];
}

/// Renders @p metrics into the eight tiles of @ref shadowGeometryFor's atlas.
///
/// Deterministic: the same arguments produce byte-identical images, which is what lets a caller
/// cache the result and skip re-uploading an unchanged shadow.
///
/// @param metrics What to draw, @see shadowMetricsFor.
/// @param color   The shadow's colour. Its own alpha scales every layer, so a translucent colour
///                thins the whole shadow.
/// @return Empty tiles when @p metrics asks for no shadow.
[[nodiscard]] WindowShadowTiles renderWindowShadowTiles(ShadowMetrics metrics, QColor const& color);

} // namespace contour::platform
