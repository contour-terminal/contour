// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VectorGlyph.hpp>

#include <vtrasterizer/PathCoverage.hpp>
#include <vtrasterizer/PathGeometry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <numeric>
#include <ranges>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// @return The segments of path data @p path, read in a viewBox large enough for any test.
[[nodiscard]] std::vector<VectorSegment> pathOf(std::string_view path)
{
    auto const glyph = parseVectorGlyph(std::format("fill=nonzero,fg:{}", path),
                                        VectorViewBox { .x = 0, .y = 0, .width = 100, .height = 100 });
    REQUIRE(glyph.has_value());
    return glyph->layers.front().segments;
}

/// @return The polylines path data @p path flattens to.
[[nodiscard]] std::vector<Polyline> flatten(std::string_view path, float tolerance = 0.01F)
{
    return flattenPath(pathOf(path), tolerance);
}

/// @return The area the stroke of @p path with @p style covers, in a 32 x 32 pixel window, one unit a
///         pixel.
[[nodiscard]] double strokedArea(std::string_view path, VectorStrokeStyle const& style)
{
    auto const outlines = strokePolylines(flatten(path), style, 0.01F);
    auto const window = ImageSize { Width(32), Height(32) };
    auto const pixels = pathCoverage(outlines, VectorFillRule::NonZero, window).expanded(window);
    return std::accumulate(pixels.begin(), pixels.end(), 0.0);
}

/// @return Whether the stroke of @p path with @p style covers the pixel whose top-left corner is (@p x,
///         @p y).
[[nodiscard]] bool strokeCovers(std::string_view path, VectorStrokeStyle const& style, int x, int y)
{
    auto const outlines = strokePolylines(flatten(path), style, 0.01F);
    auto const window = ImageSize { Width(32), Height(32) };
    auto const pixels = pathCoverage(outlines, VectorFillRule::NonZero, window).expanded(window);
    return pixels[static_cast<size_t>((y * 32) + x)] > 0.99F;
}

constexpr auto Pi = std::numbers::pi;

} // namespace

TEST_CASE("Affine: compose and inverse", "[PathGeometry]")
{
    auto const scale = Affine { .a = 2, .b = 0, .c = 0, .d = 3, .e = 0, .f = 0 };
    auto const move = Affine { .a = 1, .b = 0, .c = 0, .d = 1, .e = 10, .f = 20 };

    // The inner map first, then the outer.
    CHECK(compose(move, scale).apply({ 1, 1 }) == PathPoint { 12, 23 });
    CHECK(compose(scale, move).apply({ 1, 1 }) == PathPoint { 22, 63 });

    auto const both = compose(move, scale);
    auto const back = inverse(both);
    REQUIRE(back.has_value());
    CHECK(back->apply(both.apply({ 5, 7 })).x == Catch::Approx(5));
    CHECK(back->apply(both.apply({ 5, 7 })).y == Catch::Approx(7));
    // A map that flattens the plane cannot be undone.
    CHECK_FALSE(inverse(Affine { .a = 1, .b = 1, .c = 1, .d = 1 }).has_value());

    CHECK(largestStretch(scale) == Catch::Approx(3));
    CHECK(largestStretch(Affine { .a = 0, .b = 5, .c = -5, .d = 0 }) == Catch::Approx(5)); // a turn, scaled
    CHECK(toAffine(VectorMatrix { .a = 1, .b = 2, .c = 3, .d = 4, .e = 5, .f = 6 })
          == Affine { .a = 1, .b = 2, .c = 3, .d = 4, .e = 5, .f = 6 });
}

TEST_CASE("flattenPath: lines, subpaths and closure", "[PathGeometry]")
{
    auto const polylines = flatten("M1 1h4v4ZM10 10l2 2");
    REQUIRE(polylines.size() == 2);
    CHECK(polylines[0]
          == Polyline { .points = { { 1, 1 }, { 5, 1 }, { 5, 5 } }, .closure = PathClosure::Closed });
    CHECK(polylines[1] == Polyline { .points = { { 10, 10 }, { 12, 12 } }, .closure = PathClosure::Open });

    // A closing line said twice is said once.
    CHECK(flatten("M1 1h4v4H1V1Z").front().points.size() == 4);
    // A subpath that only moves draws nothing; one that moves and closes is a point with something to
    // draw at it (a stroke's caps).
    CHECK(flatten("M1 1M2 2").empty());
    CHECK(flatten("M3 3Z")
          == std::vector { Polyline { .points = { { 3, 3 } }, .closure = PathClosure::Closed } });
    CHECK(flatten("M3 3L3 3")
          == std::vector { Polyline { .points = { { 3, 3 } }, .closure = PathClosure::Open } });
}

TEST_CASE("flattenPath: curves stay within tolerance", "[PathGeometry]")
{
    // A quarter circle of radius 10 as a cubic: every point of the polyline is on the circle to within
    // the tolerance, and a tighter tolerance takes more lines.
    auto const arc = std::string_view { "M10 0A10 10 0 0 1 0 10" };
    for (auto const tolerance: { 1.0F, 0.1F, 0.01F })
    {
        auto const polyline = flatten(arc, tolerance).front();
        for (auto const& point: polyline.points)
            // The cubic itself is within 0.03% of a circle.
            CHECK(std::hypot(point.x, point.y) == Catch::Approx(10).margin(tolerance + 0.01));
    }
    CHECK(flatten(arc, 0.01F).front().points.size() > flatten(arc, 1.0F).front().points.size());

    // A curve flat enough is one line.
    CHECK(flatten("M0 0Q5 0.001 10 0").front().points.size() == 2);
    // A quadratic ends where it says, and passes its midpoint.
    auto const quad = flatten("M0 0Q5 10 10 0").front();
    CHECK(quad.points.back() == PathPoint { 10, 0 });
    CHECK(std::ranges::max(quad.points, {}, &PathPoint::y).y == Catch::Approx(5).margin(0.02));
}

TEST_CASE("strokePolylines: a straight line", "[PathGeometry]")
{
    using enum VectorLineCap;
    auto const line = std::string_view { "M8 16h16" };

    // 16 long and 4 wide: 64, with nothing past its ends.
    CHECK(strokedArea(line, { .width = 4, .cap = Butt }) == Catch::Approx(64.0).margin(0.01));
    // A square cap adds half the width at each end.
    CHECK(strokedArea(line, { .width = 4, .cap = Square }) == Catch::Approx(80.0).margin(0.01));
    // A round cap adds half a disc at each.
    CHECK(strokedArea(line, { .width = 4, .cap = Round }) == Catch::Approx(64.0 + (Pi * 4)).margin(0.15));

    // Whichever way the line runs.
    CHECK(strokedArea("M24 16h-16", { .width = 4, .cap = Square }) == Catch::Approx(80.0).margin(0.01));
    CHECK(strokedArea("M16 8v16", { .width = 4, .cap = Butt }) == Catch::Approx(64.0).margin(0.01));
}

TEST_CASE("strokePolylines: joins", "[PathGeometry]")
{
    using enum VectorLineJoin;
    // A right angle with its corner at (16, 16), 4 wide: the outside of the corner is the square pixel
    // block from (16, 14) to (18, 16).
    auto const corner = std::string_view { "M6 16h10v10" };
    auto const style = [](VectorLineJoin join, float limit = 4) {
        return VectorStrokeStyle {
            .width = 4, .cap = VectorLineCap::Butt, .join = join, .miterLimit = limit
        };
    };

    // The two arms overlap in a 2 x 2 square inside the corner: 40 + 40 - 4.
    auto const bevel = strokedArea(corner, style(Bevel));
    CHECK(bevel == Catch::Approx(76.0 + 2.0).margin(0.01)); // and half the outer square
    // A miter fills the outer square; a round join a quarter disc of it.
    CHECK(strokedArea(corner, style(Miter)) == Catch::Approx(76.0 + 4.0).margin(0.01));
    CHECK(strokedArea(corner, style(Round)) == Catch::Approx(76.0 + Pi).margin(0.05));

    // The outermost pixel of the corner is there with a miter and not with a bevel.
    CHECK(strokeCovers(corner, style(Miter), 17, 14));
    CHECK_FALSE(strokeCovers(corner, style(Bevel), 17, 14));

    // A right angle's miter is 1.414 times the width: a limit below that falls back to a bevel.
    CHECK(strokedArea(corner, style(Miter, 1.5F)) == Catch::Approx(80.0).margin(0.01));
    CHECK(strokedArea(corner, style(Miter, 1.4F)) == Catch::Approx(bevel).margin(0.01));

    // The turn the other way has the same join on its other side.
    CHECK(strokedArea("M6 16h10v-10", style(Miter)) == Catch::Approx(80.0).margin(0.01));
}

TEST_CASE("strokePolylines: a closed path has an outer and an inner edge", "[PathGeometry]")
{
    using enum VectorLineJoin;
    auto const style = VectorStrokeStyle { .width = 2, .cap = VectorLineCap::Butt, .join = Miter };
    // A 12 x 12 square, 2 wide with mitered corners: 14 x 14 less the 10 x 10 inside.
    auto const square = std::string_view { "M10 10h12v12h-12Z" };
    CHECK(strokedArea(square, style) == Catch::Approx(96.0).margin(0.01));
    // Wound the other way it is the same ring.
    CHECK(strokedArea("M10 10v12h12v-12Z", style) == Catch::Approx(96.0).margin(0.01));
    // The middle is not covered, the edge is.
    CHECK_FALSE(strokeCovers(square, style, 16, 16));
    CHECK(strokeCovers(square, style, 9, 9)); // the outer corner, mitered
    CHECK(strokeCovers(square, style, 15, 9));

    // Left open, the same lines have no join at their start: a corner is missing.
    auto const open = std::string_view { "M10 10h12v12h-12V10" };
    CHECK(strokedArea(open, style) == Catch::Approx(95.0).margin(0.01));
    CHECK_FALSE(strokeCovers(open, style, 9, 9));

    // Two strokes that cross cover their crossing once.
    CHECK(strokedArea("M8 16h16M16 8v16", { .width = 2, .cap = VectorLineCap::Butt })
          == Catch::Approx(32.0 + 32.0 - 4.0).margin(0.01));
}

TEST_CASE("strokePolylines: a subpath of no length draws its caps", "[PathGeometry]")
{
    using enum VectorLineCap;
    // What a dotted icon is made of.
    // A round cap is a polygon inside its circle, so a little less than the circle.
    CHECK(strokedArea("M16 16Z", { .width = 6, .cap = Round }) == Catch::Approx(Pi * 9).margin(0.15));
    CHECK(strokedArea("M16 16h0", { .width = 6, .cap = Round }) == Catch::Approx(Pi * 9).margin(0.15));
    CHECK(strokedArea("M16 16Z", { .width = 6, .cap = Square }) == Catch::Approx(36.0).margin(0.01));
    CHECK(strokedArea("M16 16Z", { .width = 6, .cap = Butt }) == 0.0);
    // A dot and a line that overlap it cover their union, not their difference.
    CHECK(strokedArea("M16 16ZM10 16h12", { .width = 6, .cap = Butt }) == Catch::Approx(72.0).margin(0.2));
}

TEST_CASE("strokePolylines: a round stroke of a curve is as wide as it says everywhere", "[PathGeometry]")
{
    // A full circle of radius 8, 4 wide: the ring between radius 6 and 10.
    auto const circle = std::string_view { "M24 16A8 8 0 1 1 8 16A8 8 0 1 1 24 16Z" };
    CHECK(strokedArea(circle, { .width = 4 }) == Catch::Approx(Pi * (100 - 36)).epsilon(0.005));
}

TEST_CASE("transformPolylines", "[PathGeometry]")
{
    auto polylines = flatten("M1 2L3 4");
    transformPolylines(polylines, Affine { .a = 2, .b = 0, .c = 0, .d = 2, .e = 10, .f = 0 });
    CHECK(polylines.front().points == std::vector<PathPoint> { { 12, 4 }, { 16, 8 } });
}
