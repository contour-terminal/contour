// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowShadowGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>

using namespace contour::platform;
using contour::config::configEnumValues;
using contour::config::ShadowSize;

namespace
{
[[nodiscard]] bool overlaps(ShadowTileRect const& a, ShadowTileRect const& b) noexcept
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
} // namespace

TEST_CASE("shadowMetricsFor is a table of Breeze's own numbers", "[contour][shadow]")
{
    SECTION("None is the zero value, so a zero-initialized metric means no shadow")
    {
        CHECK(shadowMetricsFor(ShadowSize::None) == ShadowMetrics {});
    }

    SECTION("reach grows strictly with size")
    {
        // A loop over the config table rather than four hand-written comparisons: a sixth size
        // would be covered the moment it is added to the enum, which is the point of the table.
        auto previous = 0;
        for (auto const& info: configEnumValues<ShadowSize>())
        {
            auto const reach = blurExtentFor(shadowMetricsFor(info.value).primary.radius);
            if (info.value != ShadowSize::None)
                CHECK(reach > previous);
            previous = reach;
        }
    }

    SECTION("every size but None displaces the shadow downward")
    {
        for (auto const& info: configEnumValues<ShadowSize>())
            if (info.value != ShadowSize::None)
                CHECK(shadowMetricsFor(info.value).offsetY > 0);
    }

    SECTION("the secondary layer is lifted and fainter than the primary")
    {
        // What makes the composite read as a shadow rather than a halo. If these ever invert, the
        // top edge of the window gains a bright rim.
        for (auto const& info: configEnumValues<ShadowSize>())
        {
            if (info.value == ShadowSize::None)
                continue;
            auto const metrics = shadowMetricsFor(info.value);
            CHECK(metrics.secondary.offsetY < 0);
            CHECK(metrics.secondary.opacity < metrics.primary.opacity);
            CHECK(metrics.secondary.radius < metrics.primary.radius);
        }
    }
}

TEST_CASE("blurExtentFor follows the CSS/SVG blur formula", "[contour][shadow]")
{
    // Breeze's Large radius, worked through stdDev = r/2 and the SVG scale factor.
    CHECK(blurExtentFor(48) == 68);
    CHECK(blurExtentFor(24) == 34);

    SECTION("a zero radius still yields a usable minimum")
    {
        // Guards the divide-and-floor: a zero-sized tile cannot be uploaded as an X pixmap.
        CHECK(blurExtentFor(0) == 2);
    }
}

TEST_CASE("shadowGeometryFor lays out a stretchable nine-patch", "[contour][shadow]")
{
    SECTION("None produces nothing at all")
    {
        auto const geometry = shadowGeometryFor(shadowMetricsFor(ShadowSize::None));
        CHECK(geometry == ShadowGeometry {});
        CHECK(geometry.atlasWidth == 0);
        for (auto const tile: AllShadowTiles)
            CHECK(tileRect(geometry, tile) == ShadowTileRect {});
    }

    auto const metrics = shadowMetricsFor(ShadowSize::Large);
    auto const geometry = shadowGeometryFor(metrics);

    SECTION("the downward offset makes the shadow reach further below than above")
    {
        // The asymmetry IS the shadow: equal offsets would read as a glow.
        CHECK(geometry.offsets.bottom > geometry.offsets.top);
        CHECK(geometry.offsets.left == geometry.offsets.right);
        CHECK(geometry.offsets.bottom - geometry.offsets.top == 2 * metrics.offsetY);
    }

    SECTION("the atlas is exactly the offsets plus the synthetic window")
    {
        CHECK(geometry.atlasWidth == geometry.offsets.left + geometry.windowExtent + geometry.offsets.right);
        CHECK(geometry.atlasHeight == geometry.offsets.top + geometry.windowExtent + geometry.offsets.bottom);
        CHECK(geometry.windowExtent > 0);
    }

    SECTION("every tile lies inside the atlas")
    {
        for (auto const tile: AllShadowTiles)
        {
            auto const& rect = tileRect(geometry, tile);
            INFO("tile " << static_cast<int>(tile));
            CHECK(rect.width > 0);
            CHECK(rect.height > 0);
            CHECK(rect.x >= 0);
            CHECK(rect.y >= 0);
            CHECK(rect.x + rect.width <= geometry.atlasWidth);
            CHECK(rect.y + rect.height <= geometry.atlasHeight);
        }
    }

    SECTION("no two tiles overlap")
    {
        // Written as a loop over pairs so a ninth tile is covered without editing the test.
        for (auto const i: std::views::iota(size_t { 0 }, AllShadowTiles.size()))
            for (auto const j: std::views::iota(i + 1, AllShadowTiles.size()))
            {
                INFO("tiles " << static_cast<int>(AllShadowTiles[i]) << " and "
                              << static_cast<int>(AllShadowTiles[j]));
                CHECK_FALSE(
                    overlaps(tileRect(geometry, AllShadowTiles[i]), tileRect(geometry, AllShadowTiles[j])));
            }
    }

    SECTION("the four edge tiles are one pixel along the axis the compositor stretches")
    {
        // KWin stretches the edge tiles between the corners; anything wider than a pixel is bytes
        // uploaded to be thrown away, and anything that varies along that axis would band.
        CHECK(tileRect(geometry, ShadowTile::Top).width == 1);
        CHECK(tileRect(geometry, ShadowTile::Bottom).width == 1);
        CHECK(tileRect(geometry, ShadowTile::Left).height == 1);
        CHECK(tileRect(geometry, ShadowTile::Right).height == 1);
    }

    SECTION("edge tiles are exactly as deep as the shadow reaches on their side")
    {
        CHECK(tileRect(geometry, ShadowTile::Top).height == geometry.offsets.top);
        CHECK(tileRect(geometry, ShadowTile::Bottom).height == geometry.offsets.bottom);
        CHECK(tileRect(geometry, ShadowTile::Left).width == geometry.offsets.left);
        CHECK(tileRect(geometry, ShadowTile::Right).width == geometry.offsets.right);
    }

    SECTION("corner tiles carry enough straight edge for the stretch to start on a settled profile")
    {
        // Each corner must extend past the window's corner by at least the blur's reach, or the
        // edge tile is cut where the corner is still bending the falloff and the seam shows.
        auto const reach = blurExtentFor(metrics.primary.radius);
        CHECK(tileRect(geometry, ShadowTile::TopLeft).width - geometry.offsets.left >= reach);
        CHECK(tileRect(geometry, ShadowTile::TopLeft).height - geometry.offsets.top >= reach);
        CHECK(tileRect(geometry, ShadowTile::BottomRight).width - geometry.offsets.right >= reach);
        CHECK(tileRect(geometry, ShadowTile::BottomRight).height - geometry.offsets.bottom >= reach);
    }

    SECTION("corners sit in the atlas corners, which is where the compositor pins them")
    {
        CHECK(tileRect(geometry, ShadowTile::TopLeft).x == 0);
        CHECK(tileRect(geometry, ShadowTile::TopLeft).y == 0);
        CHECK(tileRect(geometry, ShadowTile::TopRight).x + tileRect(geometry, ShadowTile::TopRight).width
              == geometry.atlasWidth);
        CHECK(tileRect(geometry, ShadowTile::BottomLeft).y + tileRect(geometry, ShadowTile::BottomLeft).height
              == geometry.atlasHeight);
    }

    SECTION("every size yields a consistent layout")
    {
        for (auto const& info: configEnumValues<ShadowSize>())
        {
            if (info.value == ShadowSize::None)
                continue;
            INFO("size " << info.token);
            auto const each = shadowGeometryFor(shadowMetricsFor(info.value));
            CHECK(each.atlasWidth > 0);
            CHECK(each.offsets.bottom > each.offsets.top);
            for (auto const tile: AllShadowTiles)
                CHECK(tileRect(each, tile).width > 0);
        }
    }
}

TEST_CASE("shadowVisibilityFor covers every window state", "[contour][shadow]")
{
    constexpr auto AllPresentations = std::array { WindowPresentation::Windowed,
                                                   WindowPresentation::Maximized,
                                                   WindowPresentation::FullScreen,
                                                   WindowPresentation::Tiled };
    constexpr auto AllDecorations = std::array { WindowDecoration::Client, WindowDecoration::Server };

    SECTION("only a windowed, client-decorated window with a real size gets a shadow")
    {
        // The full cross product as a table, so a fifth presentation or a third decoration cannot
        // quietly inherit "Shown".
        for (auto const presentation: AllPresentations)
            for (auto const decoration: AllDecorations)
                for (auto const& info: configEnumValues<ShadowSize>())
                {
                    auto const expected = presentation == WindowPresentation::Windowed
                                                  && decoration == WindowDecoration::Client
                                                  && info.value != ShadowSize::None
                                              ? ShadowVisibility::Shown
                                              : ShadowVisibility::Hidden;
                    INFO("presentation " << static_cast<int>(presentation) << ", decoration "
                                         << static_cast<int>(decoration) << ", size " << info.token);
                    CHECK(shadowVisibilityFor(presentation, decoration, info.value) == expected);
                }
    }

    SECTION("a server-decorated window never gets one, whatever its size")
    {
        // Two shadows stack into one visibly wrong one, so this is the rule that keeps
        // show_title_bar: true looking native.
        for (auto const& info: configEnumValues<ShadowSize>())
            CHECK(shadowVisibilityFor(WindowPresentation::Windowed, WindowDecoration::Server, info.value)
                  == ShadowVisibility::Hidden);
    }
}
