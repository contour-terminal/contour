// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISRasterizer.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtbackend::regis;
using crispy::point;

namespace
{
auto canvas(int w, int h)
{
    return ReGISRasterizer { ImageSize { Width(w), Height(h) } };
}

/// @return true if the pixel is opaque (alpha == 255).
bool painted(ReGISRasterizer const& r, int x, int y)
{
    return r.at(x, y).alpha() == 0xFF;
}
} // namespace

TEST_CASE("ReGISRasterizer.blankIsTransparent", "[regis][raster]")
{
    auto r = canvas(8, 8);
    CHECK_FALSE(painted(r, 0, 0));
    CHECK(r.at(3, 3).value == 0u);
}

TEST_CASE("ReGISRasterizer.eraseTo", "[regis][raster]")
{
    auto r = canvas(4, 4);
    r.eraseTo(RGBAColor { 10, 20, 30, 255 });
    auto const c = r.at(2, 2);
    CHECK(c.red() == 10);
    CHECK(c.green() == 20);
    CHECK(c.blue() == 30);
    CHECK(c.alpha() == 255);
}

TEST_CASE("ReGISRasterizer.plotDot", "[regis][raster]")
{
    auto r = canvas(8, 8);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotDot(pen, point { .x = 3, .y = 4 });
    CHECK(painted(r, 3, 4));
    CHECK_FALSE(painted(r, 2, 4));
    CHECK_FALSE(painted(r, 4, 4));
}

TEST_CASE("ReGISRasterizer.plotLine.horizontal", "[regis][raster]")
{
    auto r = canvas(10, 4);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotLine(pen, point { .x = 1, .y = 2 }, point { .x = 8, .y = 2 });
    for (auto x = 1; x <= 8; ++x)
        CHECK(painted(r, x, 2));
    CHECK_FALSE(painted(r, 0, 2));
    CHECK_FALSE(painted(r, 9, 2));
}

TEST_CASE("ReGISRasterizer.plotLine.diagonal", "[regis][raster]")
{
    auto r = canvas(6, 6);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotLine(pen, point { .x = 0, .y = 0 }, point { .x = 5, .y = 5 });
    for (auto i = 0; i <= 5; ++i)
        CHECK(painted(r, i, i));
}

TEST_CASE("ReGISRasterizer.plotLine.pattern", "[regis][raster]")
{
    // Pattern 0xaa = 10101010: every other pixel paints. Solid start at x=0.
    auto r = canvas(8, 2);
    auto pen = Pen { .color = RGBColor { 255, 255, 255 }, .pattern = 0xAA };
    r.resetPattern();
    r.plotLine(pen, point { .x = 0, .y = 0 }, point { .x = 7, .y = 0 });
    CHECK(painted(r, 0, 0));
    CHECK_FALSE(painted(r, 1, 0));
    CHECK(painted(r, 2, 0));
    CHECK_FALSE(painted(r, 3, 0));
}

TEST_CASE("ReGISRasterizer.writeMode.erase", "[regis][raster]")
{
    auto r = canvas(4, 4);
    r.eraseTo(RGBAColor { 200, 200, 200, 255 });
    auto const pen = Pen { .mode = WritingMode::Erase };
    r.plotDot(pen, point { .x = 1, .y = 1 });
    CHECK(r.at(1, 1).value == 0u); // erased back to transparent
    CHECK(painted(r, 0, 0));       // neighbour untouched
}

TEST_CASE("ReGISRasterizer.writeMode.complement", "[regis][raster]")
{
    auto r = canvas(4, 4);
    r.eraseTo(RGBAColor { 0, 0, 0, 255 });
    auto const pen = Pen { .mode = WritingMode::Complement };
    r.plotDot(pen, point { .x = 2, .y = 2 });
    auto const c = r.at(2, 2);
    CHECK(c.red() == 255);
    CHECK(c.green() == 255);
    CHECK(c.blue() == 255);
}

TEST_CASE("ReGISRasterizer.fillPolygon.triangle", "[regis][raster]")
{
    auto r = canvas(10, 10);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    auto const pts = std::array {
        point { .x = 1, .y = 1 },
        point { .x = 8, .y = 1 },
        point { .x = 1, .y = 8 },
    };
    r.fillPolygon(pen, pts);
    CHECK(painted(r, 2, 2));       // inside
    CHECK_FALSE(painted(r, 7, 7)); // outside the diagonal
}

TEST_CASE("ReGISRasterizer.plotCircle.onRadius", "[regis][raster]")
{
    auto r = canvas(21, 21);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotCircle(pen, point { .x = 10, .y = 10 }, 8);
    // Cardinal points of the circle are painted; the centre is not.
    CHECK(painted(r, 18, 10));
    CHECK(painted(r, 2, 10));
    CHECK(painted(r, 10, 18));
    CHECK(painted(r, 10, 2));
    CHECK_FALSE(painted(r, 10, 10));
}

TEST_CASE("ReGISRasterizer.plotCurve.passesThroughPoints", "[regis][raster]")
{
    auto r = canvas(30, 30);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    auto const pts = std::array {
        point { .x = 2, .y = 15 },
        point { .x = 15, .y = 2 },
        point { .x = 28, .y = 15 },
    };
    r.plotCurve(pen, pts, false);
    // A Catmull-Rom spline interpolates its control points, so each is painted.
    CHECK(painted(r, 2, 15));
    CHECK(painted(r, 15, 2));
    CHECK(painted(r, 28, 15));
}

TEST_CASE("ReGISRasterizer.shade.fillsToReference", "[regis][raster]")
{
    auto r = canvas(12, 12);
    // A horizontal line at y=2 shaded down to reference y=9 fills the band between them.
    auto const pen = Pen {
        .color = RGBColor { 255, 255, 255 }, .shade = true, .shadeVertical = false, .shadeReference = 9
    };
    r.plotLine(pen, point { .x = 1, .y = 2 }, point { .x = 10, .y = 2 });
    for (auto y = 2; y <= 9; ++y)
        CHECK(painted(r, 5, y)); // the column under the line is filled to the reference
    CHECK_FALSE(painted(r, 5, 11));
}

TEST_CASE("ReGISRasterizer.resize", "[regis][raster]")
{
    auto r = canvas(4, 4);
    r.eraseTo(RGBAColor { 1, 2, 3, 255 });
    r.resize(ImageSize { Width(8), Height(6) });
    CHECK(r.size() == ImageSize { Width(8), Height(6) });
    CHECK(r.at(7, 5).value == 0u); // fresh canvas is transparent
}

TEST_CASE("ReGISRasterizer.plotArc.boundsEnormousSweep", "[regis][raster]")
{
    // The arc sweep angle arrives unbounded from the wire (C(A<n>)). An unclamped step count would
    // overflow the double->int cast (undefined behaviour) and spin the rasterizer for a long time;
    // the cap keeps it bounded while still drawing the arc. This test must simply return.
    auto r = canvas(40, 40);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotArc(pen, point { .x = 20, .y = 20 }, 10, 0.0, 1e12);
    CHECK(painted(r, 30, 20)); // the arc's start point (radius 10 to the right of centre)
}

TEST_CASE("ReGISRasterizer.plotCircle.boundsEnormousRadius", "[regis][raster]")
{
    // A radius derived from unbounded wire coordinates drives the loop length, so it is capped. A
    // radius far beyond the canvas paints nothing on-canvas but must return promptly rather than spin.
    auto r = canvas(40, 40);
    auto const pen = Pen { .color = RGBColor { 255, 255, 255 } };
    r.plotCircle(pen, point { .x = 20, .y = 20 }, 1 << 20);
    SUCCEED("returned without spinning on an out-of-range radius");
}
