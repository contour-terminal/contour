// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/OutlineRasterizer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

constexpr auto Window = ImageSize { Width(8), Height(8) };

/// One unit is one pixel, and y = 8 is the window's top row.
constexpr auto PixelUnits = GlyphSpanTransform { .sx = 1, .sy = 1, .tx = 0, .ty = 8 };

[[nodiscard]] constexpr GlyphOutlinePoint on(int32_t x, int32_t y) noexcept
{
    return { .x = x, .y = y, .kind = GlyphPointKind::OnCurve };
}

[[nodiscard]] constexpr GlyphOutlinePoint off(int32_t x, int32_t y) noexcept
{
    return { .x = x, .y = y, .kind = GlyphPointKind::OffCurve };
}

/// @return An outline of @p contours.
[[nodiscard]] GlyphOutline outlineOf(std::vector<std::vector<GlyphOutlinePoint>> const& contours)
{
    auto outline = GlyphOutline {};
    for (auto const& contour: contours)
    {
        outline.points.insert(outline.points.end(), contour.begin(), contour.end());
        outline.contourEnds.push_back(static_cast<uint16_t>(outline.points.size() - 1));
    }
    return outline;
}

/// @return The rectangle from (@p left, @p bottom) to (@p right, @p top), clockwise in Y-up units.
[[nodiscard]] std::vector<GlyphOutlinePoint> rectangle(int left, int bottom, int right, int top)
{
    return { on(left, bottom), on(left, top), on(right, top), on(right, bottom) };
}

/// @return The coverage of the pixel at column @p x of row @p y, the top row being 0.
[[nodiscard]] int pixel(std::span<uint8_t const> pixels, int x, int y, int width = 8)
{
    return pixels[static_cast<size_t>((y * width) + x)];
}

/// @return The ink of a bitmap, in whole pixels.
[[nodiscard]] double ink(std::span<uint8_t const> pixels)
{
    return std::accumulate(pixels.begin(), pixels.end(), 0.0) / 255.0;
}

} // namespace

TEST_CASE("rasterizeOutline: a pixel-aligned rectangle fills exactly its pixels", "[OutlineRasterizer]")
{
    // Units x 2..6, y 1..5: columns 2..5, and rows 3..6 from the top.
    auto const pixels = rasterizeOutline(outlineOf({ rectangle(2, 1, 6, 5) }), PixelUnits, Window);
    REQUIRE(pixels.size() == 64);
    for (auto const y: std::views::iota(0, 8))
        for (auto const x: std::views::iota(0, 8))
        {
            INFO("pixel " << x << ", " << y);
            auto const inside = x >= 2 && x < 6 && y >= 3 && y < 7;
            CHECK(pixel(pixels, x, y) == (inside ? 255 : 0));
        }
}

TEST_CASE("rasterizeOutline: an edge between pixels is antialiased", "[OutlineRasterizer]")
{
    // Scaled by a half, the rectangle's edges fall mid-pixel: x 1.5..2.5 and y 1.5..2.5 from the bottom.
    auto const half = GlyphSpanTransform { .sx = 0.5F, .sy = 0.5F, .tx = 0, .ty = 8 };
    auto const pixels = rasterizeOutline(outlineOf({ rectangle(3, 3, 5, 5) }), half, Window);
    CHECK(pixel(pixels, 1, 5) == 64); // a quarter of the corner pixel
    CHECK(pixel(pixels, 2, 5) == 64);
    CHECK(pixel(pixels, 1, 6) == 64);
    CHECK(pixel(pixels, 2, 6) == 64);
    CHECK(ink(pixels) < 1.01);
    CHECK(ink(pixels) > 0.99); // one square pixel of ink in all
}

TEST_CASE("rasterizeOutline: the winding direction does not matter", "[OutlineRasterizer]")
{
    auto clockwise = rectangle(1, 1, 7, 7);
    auto counterClockwise = clockwise;
    std::ranges::reverse(counterClockwise);
    CHECK(rasterizeOutline(outlineOf({ clockwise }), PixelUnits, Window)
          == rasterizeOutline(outlineOf({ counterClockwise }), PixelUnits, Window));
}

TEST_CASE("rasterizeOutline: a contour wound the other way cuts a hole", "[OutlineRasterizer]")
{
    auto hole = rectangle(3, 3, 5, 5);
    std::ranges::reverse(hole);
    auto const pixels = rasterizeOutline(outlineOf({ rectangle(1, 1, 7, 7), hole }), PixelUnits, Window);
    CHECK(pixel(pixels, 1, 1) == 255);
    CHECK(pixel(pixels, 3, 3) == 0);
    CHECK(pixel(pixels, 4, 4) == 0);
    CHECK(pixel(pixels, 5, 5) == 255);
    CHECK(ink(pixels) == 32.0); // 36 pixels less the hole's 4
}

TEST_CASE("rasterizeOutline: contours wound the same way overlap without a seam", "[OutlineRasterizer]")
{
    auto const pixels =
        rasterizeOutline(outlineOf({ rectangle(1, 1, 5, 5), rectangle(3, 3, 7, 7) }), PixelUnits, Window);
    CHECK(pixel(pixels, 3, 3) == 255); // in both
    CHECK(pixel(pixels, 4, 4) == 255);
    CHECK(ink(pixels) == 28.0); // 16 + 16 less the 4 they share
}

TEST_CASE("rasterizeOutline: off-curve points draw quadratic curves", "[OutlineRasterizer]")
{
    // Four off-curve points at the corners of the window and none on the curve: every on-curve point is
    // implied, halfway along each side, and the contour is the rounded shape through them. Each corner
    // loses the 8/3 between its curve and the corner itself, so the area is 64 - 32/3.
    auto const round = outlineOf({ { off(0, 0), off(0, 8), off(8, 8), off(8, 0) } });
    auto const pixels = rasterizeOutline(round, PixelUnits, Window);
    CHECK(pixel(pixels, 0, 0) == 0); // the corners are cut
    CHECK(pixel(pixels, 7, 7) == 0);
    CHECK(pixel(pixels, 4, 4) == 255);
    // The four sides are reached, and alike: the shape is symmetric, and so is what is drawn of it.
    CHECK(pixel(pixels, 0, 3) > 224);
    CHECK(pixel(pixels, 0, 3) == pixel(pixels, 7, 3));
    CHECK(pixel(pixels, 0, 3) == pixel(pixels, 3, 0));
    CHECK(pixel(pixels, 0, 3) == pixel(pixels, 3, 7));
    // Curves are drawn as short lines inside them, so a little less than the exact area.
    auto const exact = 64.0 - (32.0 / 3);
    CHECK(ink(pixels) < exact);
    CHECK(ink(pixels) > exact - 1.5);
}

TEST_CASE("rasterizeOutline: a contour may start on an off-curve point", "[OutlineRasterizer]")
{
    // The same triangle with a bulge, written from each of its three points: where a contour starts must
    // not change what it draws.
    auto const fromOn = outlineOf({ { on(1, 1), off(4, 9), on(7, 1) } });
    auto const fromOff = outlineOf({ { off(4, 9), on(7, 1), on(1, 1) } });
    auto const endsOff = outlineOf({ { on(7, 1), on(1, 1), off(4, 9) } });
    auto const reference = rasterizeOutline(fromOn, PixelUnits, Window);
    CHECK(ink(reference) > 10);
    CHECK(rasterizeOutline(fromOff, PixelUnits, Window) == reference);
    CHECK(rasterizeOutline(endsOff, PixelUnits, Window) == reference);
}

TEST_CASE("rasterizeOutline: what lies outside the window is clipped", "[OutlineRasterizer]")
{
    SECTION("an outline larger than the window fills it")
    {
        auto const pixels =
            rasterizeOutline(outlineOf({ rectangle(-100, -100, 100, 100) }), PixelUnits, Window);
        CHECK(std::ranges::all_of(pixels, [](uint8_t value) { return value == 255; }));
    }

    SECTION("an outline wholly outside leaves it clear")
    {
        for (auto const& outside: { rectangle(-20, 0, -10, 8),
                                    rectangle(10, 0, 20, 8),
                                    rectangle(0, -20, 8, -10),
                                    rectangle(0, 10, 8, 20) })
        {
            auto const pixels = rasterizeOutline(outlineOf({ outside }), PixelUnits, Window);
            CHECK(std::ranges::all_of(pixels, [](uint8_t value) { return value == 0; }));
        }
    }

    SECTION("a slanted edge leaving through the left border keeps its coverage inside")
    {
        // A triangle with a corner far to the left: within the window it is what a window-sized cut of
        // the same triangle, rasterized into a bitmap wide enough to hold all of it, shows there.
        auto const triangle = outlineOf({ { on(-8, 0), on(8, 8), on(8, 0) } });
        auto const cut = rasterizeOutline(triangle, PixelUnits, Window);

        auto const shifted = GlyphSpanTransform { .sx = 1, .sy = 1, .tx = 8, .ty = 8 };
        auto const whole = rasterizeOutline(triangle, shifted, ImageSize { Width(16), Height(8) });
        for (auto const y: std::views::iota(0, 8))
            for (auto const x: std::views::iota(0, 8))
            {
                INFO("pixel " << x << ", " << y);
                CHECK(pixel(cut, x, y) == pixel(whole, x + 8, y, 16));
            }
    }
}

TEST_CASE("rasterizeOutline: tiles cut from a span join into the span", "[OutlineRasterizer]")
{
    // How a two-cell glyph is drawn: each cell's tile is a window of the same span.
    auto const span = ImageSize { Width(16), Height(8) };
    auto const shape = outlineOf({ { on(1, 1), off(8, 12), on(15, 1) } });
    auto const whole = rasterizeOutline(shape, PixelUnits, span);
    auto const left = rasterizeOutline(shape, PixelUnits, Window, OutlineWindowOrigin { .x = 0, .y = 0 });
    auto const right = rasterizeOutline(shape, PixelUnits, Window, OutlineWindowOrigin { .x = 8, .y = 0 });
    for (auto const y: std::views::iota(0, 8))
        for (auto const x: std::views::iota(0, 8))
        {
            INFO("pixel " << x << ", " << y);
            CHECK(pixel(left, x, y) == pixel(whole, x, y, 16));
            CHECK(pixel(right, x, y) == pixel(whole, x + 8, y, 16));
        }
}

TEST_CASE("rasterizeOutline: degenerate input draws nothing", "[OutlineRasterizer]")
{
    CHECK(rasterizeOutline(GlyphOutline {}, PixelUnits, Window) == std::vector<uint8_t>(64, 0));
    CHECK(rasterizeOutline(outlineOf({ rectangle(1, 1, 7, 7) }), PixelUnits, ImageSize {}).empty());
    // A single point, and a line there and back, enclose no area.
    CHECK(ink(rasterizeOutline(outlineOf({ { on(4, 4) } }), PixelUnits, Window)) == 0.0);
    CHECK(ink(rasterizeOutline(outlineOf({ { on(1, 1), on(7, 7) } }), PixelUnits, Window)) == 0.0);
}
