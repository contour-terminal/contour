// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowShadowTiles.hpp>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <vector>

namespace contour::platform
{

namespace
{
    /// A single-channel coverage plane. The blur only ever touches alpha, so carrying three colour
    /// channels through it would trebled the work for bytes that never change.
    struct AlphaPlane
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> samples;

        AlphaPlane(int w, int h):
            width { w }, height { h }, samples(static_cast<size_t>(w) * static_cast<size_t>(h), 0)
        {
        }

        [[nodiscard]] uint8_t& at(int x, int y) noexcept
        {
            return samples[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)];
        }

        [[nodiscard]] uint8_t at(int x, int y) const noexcept
        {
            return samples[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)];
        }
    };

    /// One box-blur pass along a single row or column, as a sliding window.
    ///
    /// @param read   Samples the source at index i.
    /// @param write  Stores the result at index i.
    /// @param length How many samples the line has.
    /// @param radius Half the box width. The window spans [i - radius, i + radius].
    template <typename Read, typename Write>
    void boxBlurLine(Read read, Write write, int length, int radius)
    {
        // Edges extend rather than wrap, so the sum starts pre-loaded with `radius` copies of the
        // first sample -- otherwise a shadow would fade out at the atlas border, where it should be
        // uniform for the tile that gets stretched.
        auto sum = static_cast<int>(read(0)) * (radius + 1);
        for (auto const i: std::views::iota(1, std::min(radius + 1, length)))
            sum += read(i);
        if (length < radius + 1)
            sum += static_cast<int>(read(length - 1)) * (radius + 1 - length);

        auto const window = (2 * radius) + 1;
        for (auto const i: std::views::iota(0, length))
        {
            write(i, static_cast<uint8_t>(sum / window));
            auto const leaving = read(std::max(0, i - radius));
            auto const entering = read(std::min(length - 1, i + radius + 1));
            sum += static_cast<int>(entering) - static_cast<int>(leaving);
        }
    }

    /// Blurs @p plane in place.
    ///
    /// Three box passes, which is the standard approximation of a Gaussian: the error is below what
    /// any eye resolves in a drop shadow, and unlike Qt's private qt_blurImage it is ours, so it is
    /// deterministic across Qt versions and testable without a paint device.
    void blurAlphaPlane(AlphaPlane& plane, int radius)
    {
        if (radius <= 0)
            return;

        auto scratch = AlphaPlane { plane.width, plane.height };
        auto const width = plane.width;
        auto const height = plane.height;

        // Both passes go through AlphaPlane::at rather than indexing by hand: four open-coded
        // `y * width + x` expressions are four chances at a silent off-by-one.
        for ([[maybe_unused]] auto const pass: std::views::iota(0, 3))
        {
            for (auto const y: std::views::iota(0, height))
                boxBlurLine([&](int x) { return plane.at(x, y); },
                            [&](int x, uint8_t v) { scratch.at(x, y) = v; },
                            width,
                            radius);

            for (auto const x: std::views::iota(0, width))
                boxBlurLine([&](int y) { return scratch.at(x, y); },
                            [&](int y, uint8_t v) { plane.at(x, y) = v; },
                            height,
                            radius);
        }
    }

    /// The box radius whose three passes reach as far as @p radius's Gaussian does.
    ///
    /// Three boxes of half-width r reach 3r; @ref blurExtentFor puts the Gaussian's reach at about
    /// 1.41 * radius, and r = radius / 2 gives 1.5 * radius. Close enough that the tiles the
    /// geometry cut still contain the whole falloff.
    [[nodiscard]] int boxRadiusFor(int radius) noexcept
    {
        return std::max(1, static_cast<int>((static_cast<double>(radius) * 0.5) + 0.5));
    }

    /// Draws one layer's silhouette into a fresh plane and blurs it.
    [[nodiscard]] AlphaPlane renderLayer(ShadowLayer const& layer,
                                         ShadowGeometry const& geometry,
                                         int compositeOffsetY)
    {
        auto plane = AlphaPlane { geometry.atlasWidth, geometry.atlasHeight };

        // Where this layer's copy of the window sits: the window's place in the atlas, displaced by
        // the composite offset and then by the layer's own.
        auto const left = geometry.offsets.left;
        auto const top = geometry.offsets.top + compositeOffsetY + layer.offsetY;

        auto const extent = geometry.windowExtent;
        for (auto const y: std::views::iota(std::max(0, top), std::min(geometry.atlasHeight, top + extent)))
            for (auto const x:
                 std::views::iota(std::max(0, left), std::min(geometry.atlasWidth, left + extent)))
                plane.at(x, y) = 255;

        blurAlphaPlane(plane, boxRadiusFor(layer.radius));
        return plane;
    }

    /// How far the shadow reaches back UNDER the window, so no seam shows along the edge at
    /// fractional scale factors. Breeze does the same, by the same two pixels
    /// (`innerRect.adjust(2, 2, -2, -2)` in breezedecoration.cpp).
    constexpr auto InteriorTuck = 2;
} // namespace

WindowShadowTiles renderWindowShadowTiles(ShadowMetrics metrics, QColor const& color)
{
    auto const geometry = shadowGeometryFor(metrics);
    if (geometry.atlasWidth == 0)
        return {};

    auto const primary = renderLayer(metrics.primary, geometry, metrics.offsetY);
    auto const secondary = renderLayer(metrics.secondary, geometry, metrics.offsetY);

    auto atlas = QImage { geometry.atlasWidth, geometry.atlasHeight, QImage::Format_ARGB32_Premultiplied };
    atlas.fill(Qt::transparent);

    auto const red = color.red();
    auto const green = color.green();
    auto const blue = color.blue();
    auto const colorAlpha = color.alphaF();

    // Interior the window itself covers; cleared so the shadow does not show through a translucent
    // terminal background, less the two pixels that tuck under the window's edge.
    auto const interiorLeft = geometry.offsets.left + InteriorTuck;
    auto const interiorTop = geometry.offsets.top + InteriorTuck;
    auto const interiorRight = geometry.atlasWidth - geometry.offsets.right - InteriorTuck;
    auto const interiorBottom = geometry.atlasHeight - geometry.offsets.bottom - InteriorTuck;

    for (auto const y: std::views::iota(0, geometry.atlasHeight))
    {
        auto* scanline = reinterpret_cast<QRgb*>(atlas.scanLine(y));
        for (auto const x: std::views::iota(0, geometry.atlasWidth))
        {
            if (x >= interiorLeft && x < interiorRight && y >= interiorTop && y < interiorBottom)
                continue;

            // Source-over of the two layers, which is how Breeze's renderer stacks them.
            auto const a1 = (primary.at(x, y) / 255.0) * metrics.primary.opacity;
            auto const a2 = (secondary.at(x, y) / 255.0) * metrics.secondary.opacity;
            auto const combined = (a1 + (a2 * (1.0 - a1))) * colorAlpha;
            if (combined <= 0.0)
                continue;

            auto const alpha = static_cast<int>((combined * 255.0) + 0.5);
            // Premultiplied, as the format demands.
            scanline[x] = qRgba((red * alpha) / 255, (green * alpha) / 255, (blue * alpha) / 255, alpha);
        }
    }

    auto result = WindowShadowTiles { .geometry = geometry, .tiles = {} };
    for (auto const i: std::views::iota(size_t { 0 }, ShadowTileCount))
    {
        auto const& rect = geometry.tiles[i];
        result.tiles[i] = atlas.copy(rect.x, rect.y, rect.width, rect.height);
    }
    return result;
}

} // namespace contour::platform
