// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowShadowTiles.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using namespace contour::platform;
using contour::config::ShadowSize;

TEST_CASE("renderWindowShadowTiles produces uploadable tiles", "[contour][shadow]")
{
    // No QGuiApplication anywhere in this file: QImage and QPainter need no paint device and no
    // platform plugin, which is what keeps the shadow's appearance testable on a build machine.
    auto const tiles = renderWindowShadowTiles(shadowMetricsFor(ShadowSize::Large), Qt::black);

    SECTION("every tile has exactly the size the geometry promised")
    {
        // The adapters upload geometry.tiles[i].width x height worth of bytes; a mismatch here is a
        // buffer overrun on the Wayland path and a corrupt pixmap on the X11 one.
        REQUIRE_FALSE(tiles.isEmpty());
        for (auto const tile: AllShadowTiles)
        {
            auto const& rect = tileRect(tiles.geometry, tile);
            auto const& image = tileImage(tiles, tile);
            INFO("tile " << static_cast<int>(tile));
            CHECK(image.width() == rect.width);
            CHECK(image.height() == rect.height);
            CHECK(image.format() == QImage::Format_ARGB32_Premultiplied);
        }
    }

    SECTION("colour channels never exceed alpha")
    {
        // The premultiplication invariant. Painting with a non-premultiplied colour breaks it, and
        // the result is a shadow that goes lighter instead of darker as it fades.
        //
        // Counted rather than asserted per pixel: an assertion inside the loop would report a
        // quarter of a million results for one property, drowning every other check in the suite.
        auto violations = 0;
        for (auto const tile: AllShadowTiles)
        {
            auto const& image = tileImage(tiles, tile);
            for (auto const y: std::views::iota(0, image.height()))
                for (auto const x: std::views::iota(0, image.width()))
                {
                    auto const pixel = image.pixel(x, y);
                    auto const alpha = qAlpha(pixel);
                    if (qRed(pixel) > alpha || qGreen(pixel) > alpha || qBlue(pixel) > alpha)
                        ++violations;
                }
        }
        CHECK(violations == 0);
    }

    SECTION("the shadow fades outward")
    {
        // The one property that makes this a shadow rather than a grey box: along the left edge
        // tile, alpha must fall as we move away from the window.
        auto const& left = tileImage(tiles, ShadowTile::Left);
        REQUIRE(left.width() > 2);
        // x = width - 1 is against the window, x = 0 is the outermost sample.
        CHECK(qAlpha(left.pixel(left.width() - 1, 0)) > qAlpha(left.pixel(0, 0)));

        auto previous = -1;
        for (auto const x: std::views::iota(0, left.width()))
        {
            auto const alpha = qAlpha(left.pixel(x, 0));
            INFO("x " << x);
            CHECK(alpha >= previous);
            previous = alpha;
        }
    }

    SECTION("the bottom reaches further than the top, as the offsets say")
    {
        CHECK(tileImage(tiles, ShadowTile::Bottom).height() > tileImage(tiles, ShadowTile::Top).height());
    }

    SECTION("rendering is deterministic")
    {
        // What lets the controller cache tiles and skip an upload. If this ever fails, every
        // refresh re-uploads eight pixmaps.
        auto const again = renderWindowShadowTiles(shadowMetricsFor(ShadowSize::Large), Qt::black);
        for (auto const tile: AllShadowTiles)
            CHECK(tileImage(tiles, tile) == tileImage(again, tile));
    }

    SECTION("a translucent shadow colour thins the whole shadow")
    {
        auto const faint = renderWindowShadowTiles(shadowMetricsFor(ShadowSize::Large), QColor(0, 0, 0, 128));
        auto const& opaqueLeft = tileImage(tiles, ShadowTile::Left);
        auto const& faintLeft = tileImage(faint, ShadowTile::Left);
        REQUIRE(faintLeft.width() == opaqueLeft.width());
        CHECK(qAlpha(faintLeft.pixel(faintLeft.width() - 1, 0))
              < qAlpha(opaqueLeft.pixel(opaqueLeft.width() - 1, 0)));
    }
}

TEST_CASE("renderWindowShadowTiles draws nothing when asked for no shadow", "[contour][shadow]")
{
    auto const tiles = renderWindowShadowTiles(shadowMetricsFor(ShadowSize::None), Qt::black);

    CHECK(tiles.isEmpty());
    for (auto const tile: AllShadowTiles)
        CHECK(tileImage(tiles, tile).isNull());
}

TEST_CASE("every shadow size renders", "[contour][shadow]")
{
    // Cheap breadth: a size whose metrics produce a degenerate tile would abort the X11 upload.
    for (auto const& info: contour::config::configEnumValues<ShadowSize>())
    {
        INFO("size " << info.token);
        auto const tiles = renderWindowShadowTiles(shadowMetricsFor(info.value), Qt::black);
        if (info.value == ShadowSize::None)
        {
            CHECK(tiles.isEmpty());
            continue;
        }
        REQUIRE_FALSE(tiles.isEmpty());
        for (auto const tile: AllShadowTiles)
            CHECK_FALSE(tileImage(tiles, tile).isNull());
    }
}
