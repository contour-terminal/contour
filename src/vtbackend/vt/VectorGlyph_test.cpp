// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VectorGlyph.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace vtbackend;
using Kind = VectorSegmentKind;

namespace
{

constexpr auto Box24 = VectorViewBox { .x = 0, .y = 0, .width = 24, .height = 24 };

constexpr auto Malformed = std::unexpected { VectorGlyphError::Malformed };
constexpr auto TooLarge = std::unexpected { VectorGlyphError::TooLarge };

[[nodiscard]] std::expected<VectorGlyph, VectorGlyphError> parse(std::string_view payload,
                                                                 VectorViewBox viewBox = Box24)
{
    return parseVectorGlyph(payload, viewBox);
}

/// @return The segments of the one filled layer whose path data is @p path.
[[nodiscard]] std::vector<VectorSegment> segmentsOf(std::string_view path)
{
    auto const glyph = parse(std::format("fill=nonzero,fg:{}", path));
    REQUIRE(glyph.has_value());
    return glyph->layers.front().segments;
}

/// @return Whether path data @p path is taken.
[[nodiscard]] bool takesPath(std::string_view path)
{
    return parse(std::format("fill=nonzero,fg:{}", path)).has_value();
}

/// @return The kinds of @p segments, in order.
[[nodiscard]] std::vector<Kind> kindsOf(std::vector<VectorSegment> const& segments)
{
    return std::ranges::to<std::vector>(segments | std::views::transform(&VectorSegment::kind));
}

/// @return The point a segment ends at.
[[nodiscard]] VectorPoint endOf(VectorSegment const& segment)
{
    switch (segment.kind)
    {
        case Kind::QuadTo: return segment.points[1];
        case Kind::CubicTo: return segment.points[2];
        default: return segment.points[0];
    }
}

[[nodiscard]] VectorSegment moveTo(float x, float y)
{
    return { .kind = Kind::MoveTo, .points = { VectorPoint { x, y } } };
}

[[nodiscard]] VectorSegment lineTo(float x, float y)
{
    return { .kind = Kind::LineTo, .points = { VectorPoint { x, y } } };
}

[[nodiscard]] VectorSegment closePath()
{
    return { .kind = Kind::Close, .points = {} };
}

} // namespace

TEST_CASE("parseVectorViewBox", "[VectorGlyph]")
{
    CHECK(parseVectorViewBox("0,0,24,24") == Box24);
    CHECK(parseVectorViewBox(" 0 0 24 24 ") == Box24);
    CHECK(parseVectorViewBox("-1.5, 2e1 ,3,.5")
          == VectorViewBox { .x = -1.5F, .y = 20, .width = 3, .height = 0.5F });

    CHECK_FALSE(parseVectorViewBox("0,0,24").has_value());
    CHECK_FALSE(parseVectorViewBox("0,0,24,24,1").has_value());
    CHECK_FALSE(parseVectorViewBox("0,0,0,24").has_value());  // no width
    CHECK_FALSE(parseVectorViewBox("0,0,24,-1").has_value()); // a negative height
    CHECK_FALSE(parseVectorViewBox("0,,0,24,24").has_value());
    CHECK_FALSE(parseVectorViewBox("0,0,1000001,24").has_value());
    CHECK_FALSE(parseVectorViewBox("-1000001,0,24,24").has_value());
    CHECK(parseVectorViewBox("-1000000,0,1000000,24").has_value());
    CHECK_FALSE(parseVectorViewBox("").has_value());
}

TEST_CASE("VectorGlyph: path commands, absolute and relative", "[VectorGlyph]")
{
    CHECK(segmentsOf("M4 4h16v16H4Z")
          == std::vector { moveTo(4, 4), lineTo(20, 4), lineTo(20, 20), lineTo(4, 20), closePath() });
    CHECK(segmentsOf("m4 4l2 3L10,11V2z")
          == std::vector { moveTo(4, 4), lineTo(6, 7), lineTo(10, 11), lineTo(10, 2), closePath() });

    // A moveto's extra pairs are linetos, relative ones measured from the point before each.
    CHECK(segmentsOf("M1 1 2 2 3 3") == std::vector { moveTo(1, 1), lineTo(2, 2), lineTo(3, 3) });
    CHECK(segmentsOf("m1 1 2 2 3 3") == std::vector { moveTo(1, 1), lineTo(3, 3), lineTo(6, 6) });

    // A command repeats while arguments follow.
    CHECK(segmentsOf("M0 0h1 2 3") == std::vector { moveTo(0, 0), lineTo(1, 0), lineTo(3, 0), lineTo(6, 0) });
    CHECK(kindsOf(segmentsOf("M0 0C1 1 2 2 3 3 4 4 5 5 6 6"))
          == std::vector { Kind::MoveTo, Kind::CubicTo, Kind::CubicTo });

    // Path data that only moves, or only moves and closes, draws nothing and is valid.
    CHECK(segmentsOf("M3 3") == std::vector { moveTo(3, 3) });
    CHECK(segmentsOf("M3 3Z") == std::vector { moveTo(3, 3), closePath() });
}

TEST_CASE("VectorGlyph: curves, and the control point S and T reflect", "[VectorGlyph]")
{
    auto const cubics = segmentsOf("M0 0C0 4 4 8 8 8S16 4 16 0");
    REQUIRE(kindsOf(cubics) == std::vector { Kind::MoveTo, Kind::CubicTo, Kind::CubicTo });
    // The second cubic's first control point mirrors (4, 8) in (8, 8).
    CHECK(cubics[2].points[0] == VectorPoint { 12, 8 });
    CHECK(cubics[2].points[1] == VectorPoint { 16, 4 });
    CHECK(endOf(cubics[2]) == VectorPoint { 16, 0 });

    auto const quads = segmentsOf("M0 0Q4 8 8 0T16 0t8 0");
    REQUIRE(kindsOf(quads) == std::vector { Kind::MoveTo, Kind::QuadTo, Kind::QuadTo, Kind::QuadTo });
    CHECK(quads[2].points[0] == VectorPoint { 12, -8 });
    CHECK(quads[3].points[0] == VectorPoint { 20, 8 });

    // Without a curve of its own kind before it, the control point is the current point.
    CHECK(segmentsOf("M2 2S6 6 8 2")[1].points[0] == VectorPoint { 2, 2 });
    CHECK(segmentsOf("M2 2Q3 3 4 2S6 6 8 2")[2].points[0] == VectorPoint { 4, 2 });
    CHECK(segmentsOf("M2 2T8 2")[1].points[0] == VectorPoint { 2, 2 });
}

TEST_CASE("VectorGlyph: numbers as the SVG grammar spells them", "[VectorGlyph]")
{
    // Separated by a sign, or by a `.` that cannot continue the number before it.
    CHECK(segmentsOf("M1-2L.5.5") == std::vector { moveTo(1, -2), lineTo(0.5F, 0.5F) });
    CHECK(segmentsOf("M+1,+2") == std::vector { moveTo(1, 2) });
    CHECK(segmentsOf("M1e1 2.5E-1") == std::vector { moveTo(10, 0.25F) });
    CHECK(segmentsOf("M0.1 12.3456789") == std::vector { moveTo(0.1F, 12.3456789F) });
    // More digits than a float holds are still the number they spell.
    CHECK(segmentsOf("M1.0000000000000000000000001 2")[0].points[0].x == 1.0F);
    CHECK(segmentsOf("M0.000000000000000000000001e24 2")[0].points[0].x == Catch::Approx(1.0F));

    CHECK_FALSE(takesPath("M1. 2"));   // a `.` needs a digit after it
    CHECK_FALSE(takesPath("M1 2,"));   // a comma commits to another argument
    CHECK_FALSE(takesPath("M1,,2"));   // at most one comma
    CHECK_FALSE(takesPath("M1 2 3"));  // half a pair
    CHECK_FALSE(takesPath("M 1 2 L")); // a command without its arguments
    CHECK_FALSE(takesPath("M1 2e"));   // an `e` without digits is not an exponent, and not a command
    CHECK_FALSE(takesPath("M0x10 2"));
    CHECK_FALSE(takesPath("L1 2")); // path data starts with a moveto
    CHECK_FALSE(takesPath(""));
    CHECK_FALSE(takesPath("M1 2 X3 4")); // no such command
    // Every number fits a 32-bit float.
    CHECK(parse("fill=nonzero,fg:M1e39 2") == TooLarge);
    CHECK(parse("fill=nonzero,fg:M1e999999999 2") == TooLarge);
}

TEST_CASE("VectorGlyph: arcs become cubics, one for each quarter turn", "[VectorGlyph]")
{
    // Half a circle of radius 5: two quarter turns.
    auto const half = segmentsOf("M5 10A5 5 0 0 1 15 10");
    REQUIRE(kindsOf(half) == std::vector { Kind::MoveTo, Kind::CubicTo, Kind::CubicTo });
    CHECK(endOf(half[1]).x == Catch::Approx(10));
    CHECK(endOf(half[1]).y == Catch::Approx(5)); // over the top: y points down
    // The last piece lands exactly on the endpoint given.
    CHECK(endOf(half[2]) == VectorPoint { 15, 10 });

    // The other sweep goes under.
    CHECK(endOf(segmentsOf("M5 10A5 5 0 0 0 15 10")[1]).y == Catch::Approx(15));

    // A quarter is one cubic; three quarters, the large arc, three.
    CHECK(segmentsOf("M5 10A5 5 0 0 1 10 5").size() == 2);
    CHECK(segmentsOf("M5 10A5 5 0 1 1 10 15").size() == 4);

    // Flags need no separator.
    CHECK(segmentsOf("M5 10A5 5 0 0115 10") == half);
    CHECK(segmentsOf("M5 10a5 5 0 0110 0") == half);

    // Radii too small for the endpoints are scaled up to a half circle between them.
    auto const scaled = segmentsOf("M5 10A1 1 0 0 1 15 10");
    REQUIRE(scaled.size() == 3);
    CHECK(endOf(scaled[1]).y == Catch::Approx(5));

    // A zero radius draws a line, and identical endpoints nothing.
    CHECK(segmentsOf("M5 10A0 5 0 0 1 15 10") == std::vector { moveTo(5, 10), lineTo(15, 10) });
    CHECK(segmentsOf("M5 10A5 5 0 0 1 5 10") == std::vector { moveTo(5, 10) });

    CHECK_FALSE(takesPath("M5 10A5 5 0 2 1 15 10")); // a flag is 0 or 1
    CHECK_FALSE(takesPath("M5 10A5 5 0 0 1 15"));
}

TEST_CASE("VectorGlyph: drawing after Z starts at the subpath's start", "[VectorGlyph]")
{
    // The lineto after the close opens a subpath at (2, 2), with a moveto of its own.
    CHECK(
        segmentsOf("M2 2h4v4ZL8 8")
        == std::vector { moveTo(2, 2), lineTo(6, 2), lineTo(6, 6), closePath(), moveTo(2, 2), lineTo(8, 8) });
    // A relative command after it is measured from there too.
    CHECK(endOf(segmentsOf("M2 2h4v4Zl1 1").back()) == VectorPoint { 3, 3 });
    // A Z right after another closes nothing.
    CHECK(segmentsOf("M2 2h4ZZZ") == std::vector { moveTo(2, 2), lineTo(6, 2), closePath() });
}

TEST_CASE("VectorGlyph: layers", "[VectorGlyph]")
{
    auto const glyph =
        parse("fill=nonzero,fg:M4 4h16v16H4Z|fill=evenodd,#e5534bff:M17 13h1|stroke=2,fg@0.2:M6 12h12");
    REQUIRE(glyph.has_value());
    REQUIRE(glyph->layers.size() == 3);

    auto const& first = std::get<VectorFill>(glyph->layers[0].operation);
    CHECK(first.rule == VectorFillRule::NonZero);
    CHECK(first.color == VectorColor { VectorSolid { VectorForeground { .alpha = 0xFF } } });

    auto const& second = std::get<VectorFill>(glyph->layers[1].operation);
    CHECK(second.rule == VectorFillRule::EvenOdd);
    CHECK(second.color
          == VectorColor {
              VectorSolid { VectorFixedColor { .red = 0xE5, .green = 0x53, .blue = 0x4B, .alpha = 0xFF } } });

    auto const& third = std::get<VectorStroke>(glyph->layers[2].operation);
    CHECK(third.style == VectorStrokeStyle { .width = 2 });
    CHECK(third.color == VectorColor { VectorSolid { VectorForeground { .alpha = 51 } } });

    // Spaces around the parts of a header are not part of them.
    CHECK(parse(" fill=nonzero , fg : M1 1h2").has_value());

    CHECK(parse("fill=nonzero,fg") == Malformed);       // no path
    CHECK(parse("fill=nonzero:M1 1") == Malformed);     // no colour
    CHECK(parse("fill=winding,fg:M1 1") == Malformed);  // no such rule
    CHECK(parse("paint=nonzero,fg:M1 1") == Malformed); // no such layer
    CHECK(parse("fill=nonzero,fg:M1 1|") == Malformed); // an empty layer
    CHECK(parse("") == Malformed);
    CHECK(parse("fill=nonzero,fg:M1 1\n") == Malformed); // printable ASCII only
    CHECK(parse("fill=nonzero,fg:M1\t1") == Malformed);
}

TEST_CASE("VectorGlyph: stroke styles", "[VectorGlyph]")
{
    auto const styleOf = [](std::string_view header) {
        auto const glyph = parse(std::format("{},fg:M6 12h12", header));
        REQUIRE(glyph.has_value());
        return std::get<VectorStroke>(glyph->layers.front().operation).style;
    };
    using Cap = VectorLineCap;
    using Join = VectorLineJoin;

    CHECK(styleOf("stroke=1.5")
          == VectorStrokeStyle { .width = 1.5F, .cap = Cap::Round, .join = Join::Round });
    CHECK(styleOf("stroke=2/butt").cap == Cap::Butt);
    CHECK(styleOf("stroke=2/square").cap == Cap::Square);
    CHECK(styleOf("stroke=2/round/bevel").join == Join::Bevel);
    CHECK(styleOf("stroke=2/butt/miter")
          == VectorStrokeStyle { .width = 2, .cap = Cap::Butt, .join = Join::Miter, .miterLimit = 4 });
    CHECK(styleOf("stroke=2/butt/miter10.5").miterLimit == 10.5F);
    CHECK(styleOf("stroke=2/butt/miter1").miterLimit == 1);
    // As wide as the viewBox is the most a stroke may be.
    CHECK(styleOf("stroke=24").width == 24);

    for (auto const header: { "stroke=0",
                              "stroke=-1",
                              "stroke=",
                              "stroke=24.5", // wider than the viewBox
                              "stroke=2/flat",
                              "stroke=2/butt/sharp",
                              "stroke=2/butt/miter0.5", // a limit below 1
                              "stroke=2/butt/miter1001",
                              "stroke=2/butt/miter/4",
                              "stroke=2px" })
    {
        INFO(header);
        CHECK(parse(std::format("{},fg:M6 12h12", header)) == Malformed);
    }
}

TEST_CASE("VectorGlyph: colours", "[VectorGlyph]")
{
    auto const colorOf = [](std::string_view color) {
        return parse(std::format("fill=nonzero,{}:M1 1h2", color)).transform([](VectorGlyph const& glyph) {
            return std::get<VectorSolid>(std::get<VectorFill>(glyph.layers.front().operation).color);
        });
    };
    CHECK(colorOf("fg") == VectorSolid { VectorForeground {} });
    CHECK(colorOf("fg@0") == VectorSolid { VectorForeground { .alpha = 0 } });
    CHECK(colorOf("fg@1") == VectorSolid { VectorForeground { .alpha = 255 } });
    CHECK(colorOf("fg@.5") == VectorSolid { VectorForeground { .alpha = 128 } });
    CHECK(colorOf("#AbCdEf")
          == VectorSolid { VectorFixedColor { .red = 0xAB, .green = 0xCD, .blue = 0xEF } });
    CHECK(colorOf("#01020304")
          == VectorSolid { VectorFixedColor { .red = 1, .green = 2, .blue = 3, .alpha = 4 } });

    for (auto const color: { "fg@1.5",
                             "fg@-0.1",
                             "fg@",
                             "fg@x",
                             "red",
                             "#fff",
                             "#12345",
                             "#1234567",
                             "#gggggg",
                             "#+f+f+f",
                             "bg" })
    {
        INFO(color);
        CHECK(colorOf(color) == Malformed);
    }
}

TEST_CASE("VectorGlyph: gradients", "[VectorGlyph]")
{
    auto const gradientOf = [](std::string_view color) {
        return parse(std::format("fill=nonzero,{}:M1 1h2", color)).transform([](VectorGlyph const& glyph) {
            return std::get<VectorGradient>(std::get<VectorFill>(glyph.layers.front().operation).color);
        });
    };

    auto const linear = gradientOf("lin(0 0 24 0 / 0 fg / 0.5 #ff0000 / 1 fg@0.5)");
    REQUIRE(linear.has_value());
    CHECK(linear->shape
          == std::variant<VectorLinearGradient, VectorRadialGradient> {
              VectorLinearGradient { .x1 = 0, .y1 = 0, .x2 = 24, .y2 = 0 } });
    CHECK(linear->spread == VectorSpread::Pad);
    CHECK(linear->transform == VectorMatrix {});
    REQUIRE(linear->stops.size() == 3);
    CHECK(linear->stops[1]
          == VectorGradientStop { .offset = 0.5F, .color = VectorFixedColor { .red = 0xFF } });
    CHECK(linear->stops[2].color == VectorSolid { VectorForeground { .alpha = 128 } });

    auto const radial = gradientOf("rad(12 12 0 12 12 10 reflect m 1 0 0 2 3 4 / 0 #000000 / 1 #ffffff)");
    REQUIRE(radial.has_value());
    CHECK(radial->shape
          == std::variant<VectorLinearGradient, VectorRadialGradient> { VectorRadialGradient {
              .focalX = 12, .focalY = 12, .focalRadius = 0, .centerX = 12, .centerY = 12, .radius = 10 } });
    CHECK(radial->spread == VectorSpread::Reflect);
    CHECK(radial->transform == VectorMatrix { .a = 1, .b = 0, .c = 0, .d = 2, .e = 3, .f = 4 });
    CHECK(gradientOf("lin(0 0 1 1 repeat/0 fg/1 fg)")->spread == VectorSpread::Repeat);

    // A gradient with no extent is valid: it paints its last stop.
    CHECK(gradientOf("lin(5 5 5 5 / 0 fg / 1 #ff0000)").has_value());
    // Stops may share an offset.
    CHECK(gradientOf("lin(0 0 1 1 / 0 fg / 0.5 fg / 0.5 #ff0000 / 1 fg)").has_value());

    for (auto const color: {
             "lin(0 0 24 0)",                   // no stops
             "lin(0 0 24 0 / 0 fg)",            // one stop
             "lin(0 0 24 / 0 fg / 1 fg)",       // three numbers
             "lin(0 0 24 0 / 0.5 fg / 0.2 fg)", // offsets going down
             "lin(0 0 24 0 / 0 fg / 1.5 fg)",   // an offset past 1
             "lin(0 0 24 0 / -1 fg / 1 fg)",
             "lin(0 0 24 0 / 0fg / 1 fg)",                          // no space after the offset
             "lin(0 0 24 0 / 0 fg / 1 lin(0 0 1 1 / 0 fg / 1 fg))", // a stop is not a gradient
             "lin(0 0 24 0 mirror / 0 fg / 1 fg)",                  // no such spread
             "lin(0 0 24 0 padm 1 0 0 1 0 0 / 0 fg / 1 fg)",
             "lin(0 0 24 0 / 0 fg / 1 fg",           // not closed
             "rad(12 12 -1 12 12 10 / 0 fg / 1 fg)", // a negative radius
             "rad(12 12 0 12 12 -10 / 0 fg / 1 fg)",
             "rad(12 12 0 12 12 / 0 fg / 1 fg)",
             "lin(0 0 24 0 m 0 0 0 0 0 0 / 0 fg / 1 fg)", // a matrix that flattens
         })
    {
        INFO(color);
        CHECK(gradientOf(color) == Malformed);
    }

    // Sixteen stops are the most.
    auto stops = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 16))
        stops += " / 0.5 fg";
    CHECK(gradientOf(std::format("lin(0 0 24 0{})", stops)).has_value());
    CHECK(gradientOf(std::format("lin(0 0 24 0{} / 1 fg)", stops)) == TooLarge);
}

TEST_CASE("VectorGlyph: a layer's matrix", "[VectorGlyph]")
{
    auto const glyph =
        parse("fill=nonzero,fg,m 2 0 0 2 1 1:M1 1h2|stroke=1,lin(0 0 1 1 / 0 fg / 1 fg),m1,0,0,1,0,0:M1 1h2");
    REQUIRE(glyph.has_value());
    CHECK(glyph->layers[0].transform == VectorMatrix { .a = 2, .b = 0, .c = 0, .d = 2, .e = 1, .f = 1 });
    // The path is kept in the layer's own coordinates.
    CHECK(glyph->layers[0].segments == std::vector { moveTo(1, 1), lineTo(3, 1) });
    CHECK(glyph->layers[1].transform == VectorMatrix {});
    CHECK_FALSE(parse("fill=nonzero,fg:M1 1h2")->layers[0].transform.has_value());

    // A clip's matrix follows its rule.
    CHECK(parse("clip=1,evenodd,m 1 0 0 1 2 2:M0 0h4v4H0Z|fill=nonzero,fg:M1 1h2")->layers[0].transform
          == VectorMatrix { .a = 1, .b = 0, .c = 0, .d = 1, .e = 2, .f = 2 });

    CHECK(parse("fill=nonzero,fg,m 1 0 0 1 0:M1 1h2") == Malformed);     // five numbers
    CHECK(parse("fill=nonzero,fg,m 1 0 0 1 0 0 0:M1 1h2") == Malformed); // seven
    CHECK(parse("fill=nonzero,fg,x 1 0 0 1 0 0:M1 1h2") == Malformed);
    CHECK(parse("fill=nonzero,fg,:M1 1h2") == Malformed);
    // Stretching a length more than 4096 times, or shrinking it more, is refused: so is flattening it.
    CHECK(parse("fill=nonzero,fg,m 4096 0 0 1 0 0:M0 0h0.001").has_value());
    CHECK(parse("fill=nonzero,fg,m 4097 0 0 1 0 0:M0 0h0.001") == Malformed);
    CHECK(parse("fill=nonzero,fg,m 0.0001 0 0 1 0 0:M1 1h2") == Malformed);
    CHECK(parse("fill=nonzero,fg,m 1 1 1 1 0 0:M1 1h2") == Malformed);

    // A stroke's width is judged through the matrix: 13 units drawn twice as wide is past the viewBox.
    CHECK(parse("stroke=12,fg,m 2 0 0 2 0 0:M1 1h2").has_value());
    CHECK(parse("stroke=13,fg,m 2 0 0 2 0 0:M1 1h2") == Malformed);
}

TEST_CASE("VectorGlyph: clips cover layers that exist and are not clips", "[VectorGlyph]")
{
    auto const glyph = parse(
        "clip=2,nonzero:M0 0h12v24H0Z|fill=nonzero,fg:M2 2h20|stroke=1,fg:M2 2h20|fill=nonzero,fg:M1 1h1");
    REQUIRE(glyph.has_value());
    CHECK(std::get<VectorClip>(glyph->layers[0].operation)
          == VectorClip { .rule = VectorFillRule::NonZero, .count = 2 });

    CHECK(parse("clip=1,nonzero:M0 0h12v24H0Z") == Malformed);                         // nothing to clip
    CHECK(parse("clip=2,nonzero:M0 0h12v24H0Z|fill=nonzero,fg:M2 2h20") == Malformed); // one layer short
    CHECK(parse("clip=1,nonzero:M0 0h1|clip=1,nonzero:M0 0h1|fill=nonzero,fg:M2 2h2") == Malformed); // nested
    CHECK(parse("clip=0,nonzero:M0 0h1|fill=nonzero,fg:M2 2h2") == Malformed);
    CHECK(parse("clip=128,nonzero:M0 0h1|fill=nonzero,fg:M2 2h2") == Malformed);
    CHECK(parse("clip=one,nonzero:M0 0h1|fill=nonzero,fg:M2 2h2") == Malformed);
    CHECK(parse("clip=1,fg:M0 0h1|fill=nonzero,fg:M2 2h2") == Malformed); // a rule, not a colour
    // One clip after another has run out is fine.
    CHECK(parse("clip=1,nonzero:M0 0h1|fill=nonzero,fg:M2 2h2|clip=1,evenodd:M0 0h1|fill=nonzero,fg:M2 2h2")
              .has_value());
}

TEST_CASE("VectorGlyph: every point stays within one viewBox size of the viewBox", "[VectorGlyph]")
{
    // The viewBox is 0..24, so points may lie in -24..48.
    CHECK(parse("fill=nonzero,fg:M-24 -24L48 48").has_value());
    CHECK(parse("fill=nonzero,fg:M-24.5 0L1 1") == TooLarge);
    CHECK(parse("fill=nonzero,fg:M0 0L1 48.5") == TooLarge);
    // Control points count.
    CHECK(parse("fill=nonzero,fg:M0 0Q49 0 1 1") == TooLarge);
    CHECK(parse("fill=nonzero,fg:M0 0C1 1 1 -25 2 2") == TooLarge);
    // After the layer's matrix: (20, 0) doubled is still inside, (25, 0) is not.
    CHECK(parse("fill=nonzero,fg,m 2 0 0 2 0 0:M20 0h1").has_value());
    CHECK(parse("fill=nonzero,fg,m 2 0 0 2 0 0:M25 0h1") == TooLarge);
    // The margin is the larger side, on both axes.
    auto const wide = VectorViewBox { .x = 0, .y = 0, .width = 100, .height = 10 };
    CHECK(parse("fill=nonzero,fg:M0 -100L0 110", wide).has_value());
    CHECK(parse("fill=nonzero,fg:M0 -101L0 0", wide) == TooLarge);
}

TEST_CASE("VectorGlyph: at most 2048 segments, movetos and closes included", "[VectorGlyph]")
{
    // `M0 0Z` is two segments.
    auto const repeated = [](size_t count) {
        auto path = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, count))
            path += "M0 0Z";
        return path;
    };
    CHECK(takesPath(repeated(1024)));
    CHECK(parse(std::format("fill=nonzero,fg:{}", repeated(1025))) == TooLarge);

    // Shared by the layers, clips included.
    CHECK(
        parse(std::format("clip=1,nonzero:{}|fill=nonzero,fg:{}", repeated(512), repeated(512))).has_value());
    CHECK(parse(std::format("clip=1,nonzero:{}|fill=nonzero,fg:{}M0 0", repeated(512), repeated(512)))
          == TooLarge);

    // An arc counts as its cubics, and the moveto a drawing after Z implies counts too.
    auto arcs = std::string { "M5 10" };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 1023))
        arcs += "A5 5 0 0 1 15 10A5 5 0 0 1 5 10";
    CHECK(parse(std::format("fill=nonzero,fg:{}", arcs)) == TooLarge);
}

TEST_CASE("VectorGlyph: at most 64 viewBox sizes of path", "[VectorGlyph]")
{
    // The viewBox is 24, so 1536 units of path. A line across and back is 48.
    auto const backAndForth = [](size_t count) {
        auto path = std::string { "M0 12" };
        for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, count))
            path += "h24h-24";
        return path;
    };
    CHECK(parse(std::format("fill=nonzero,fg:{}", backAndForth(32))).has_value());
    CHECK(parse(std::format("fill=nonzero,fg:{}h1", backAndForth(32))) == TooLarge);

    // A stroke also costs its width for every segment, the moveto included: 65 segments of a 2-wide
    // stroke are 130 more, so the same path no longer fits.
    CHECK(parse(std::format("stroke=2,fg:{}", backAndForth(32))) == TooLarge);
    CHECK(parse(std::format("stroke=2,fg:{}", backAndForth(29))).has_value());

    // A curve is charged its control polygon: here 24 up, 24 across and 24 down, 72 for a curve that is
    // shorter. Twenty-one of them are 1512; one more is past the 1536.
    auto curves = std::string { "M0 24" };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
        curves += "C0 0 24 0 24 24C24 0 0 0 0 24";
    curves += "C0 0 24 0 24 24";
    CHECK(parse(std::format("fill=nonzero,fg:{}", curves)).has_value());
    CHECK(parse(std::format("fill=nonzero,fg:{}C24 0 0 0 0 24", curves)) == TooLarge);

    // Measured after the layer's matrix.
    CHECK(parse(std::format("fill=nonzero,fg,m 0.5 0 0 1 0 0:{}", backAndForth(64))).has_value());
    CHECK(parse(std::format("fill=nonzero,fg,m 0.5 0 0 1 0 0:{}", backAndForth(65))) == TooLarge);
}

TEST_CASE("VectorGlyph: at most 128 layers", "[VectorGlyph]")
{
    auto const layers = [](size_t count) {
        auto payload = std::string {};
        for (auto const index: std::views::iota(size_t { 0 }, count))
            payload += std::format("{}fill=nonzero,fg:M1 1h1", index == 0 ? "" : "|");
        return payload;
    };
    CHECK(parse(layers(128)).has_value());
    CHECK(parse(layers(129)) == TooLarge);
}

TEST_CASE("VectorGlyph: what its colours draw from", "[VectorGlyph]")
{
    using enum VectorPalette;
    CHECK(parse("fill=nonzero,fg:M1 1h1|stroke=1,fg@0.5:M1 1h1")->palette() == ForegroundOnly);
    CHECK(parse("fill=nonzero,lin(0 0 1 1 / 0 fg / 1 fg@0):M1 1h1")->palette() == ForegroundOnly);
    CHECK(parse("fill=nonzero,#ff0000:M1 1h1")->palette() == FixedOnly);
    CHECK(parse("fill=nonzero,lin(0 0 1 1 / 0 #000000 / 1 #ffffff):M1 1h1")->palette() == FixedOnly);
    CHECK(parse("fill=nonzero,fg:M1 1h1|fill=nonzero,#ff0000:M1 1h1")->palette() == Mixed);
    CHECK(parse("fill=nonzero,lin(0 0 1 1 / 0 fg / 1 #ffffff):M1 1h1")->palette() == Mixed);
    // A clip paints nothing.
    CHECK(parse("clip=1,nonzero:M0 0h1|fill=nonzero,fg:M1 1h1")->palette() == ForegroundOnly);
}

TEST_CASE("VectorGlyph: the specification's examples", "[VectorGlyph]")
{
    // Lucide's folder, stroked, and a two-colour badge.
    CHECK(parse("stroke=2,fg:M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 "
                "7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z")
              .has_value());
    CHECK(parse("fill=nonzero,fg:M4 4h16v16H4Z|fill=evenodd,#e5534bff:M17 13a4 4 0 1 0 0 8a4 4 0 1 0 0-8Z")
              .has_value());
    CHECK(parse("clip=1,nonzero:M0 0h12v24H0Z|fill=nonzero,lin(0 0 24 0 / 0 fg / 1 #e5534b):M2 2h20v20H2Z")
              .has_value());
    CHECK(parse("fill=nonzero,fg:M4 4h16v16H4Z|stroke=2/butt/miter,#e5534bff:M6 12h12").has_value());
}
