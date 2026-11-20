// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VectorGlyph.hpp>

#include <vtrasterizer/VectorGlyphRasterizer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// The window every test draws into: 8 x 8 pixels, the viewBox 8 x 8 units, one unit a pixel.
constexpr auto Window = ImageSize { Width(8), Height(8) };
constexpr auto Box8 = VectorViewBox { .x = 0, .y = 0, .width = 8, .height = 8 };
constexpr auto PixelUnits = GlyphSpanTransform { .sx = 1, .sy = 1, .tx = 0, .ty = 0 };
constexpr auto Blue = RGBColor { 0x00, 0x00, 0xFF };

/// A colour pixel: red, green, blue, alpha, premultiplied.
using Rgba = std::array<int, 4>;

[[nodiscard]] VectorGlyph glyphOf(std::string_view payload, VectorViewBox viewBox = Box8)
{
    auto glyph = parseVectorGlyph(payload, viewBox);
    REQUIRE(glyph.has_value());
    return *std::move(glyph);
}

/// @return @p payload drawn into the window with a blue foreground.
[[nodiscard]] RasterizedGlyph draw(std::string_view payload,
                                   GlyphSpanTransform const& placement = PixelUnits,
                                   GlyphWindowOrigin origin = {})
{
    return rasterizeVectorGlyph(glyphOf(payload), placement, Window, origin, Blue);
}

/// @return The coverage of the pixel at column @p x of row @p y of a glyph drawn as coverage.
[[nodiscard]] int coverageAt(RasterizedGlyph const& drawn, int x, int y)
{
    REQUIRE(drawn.format == GlyphPixels::Coverage);
    return drawn.pixels[static_cast<size_t>((y * 8) + x)];
}

/// @return The pixel at column @p x of row @p y of a glyph drawn in colour.
[[nodiscard]] Rgba colorAt(RasterizedGlyph const& drawn, int x, int y)
{
    REQUIRE(drawn.format == GlyphPixels::Color);
    auto const offset = static_cast<size_t>(((y * 8) + x) * 4);
    return {
        drawn.pixels[offset], drawn.pixels[offset + 1], drawn.pixels[offset + 2], drawn.pixels[offset + 3]
    };
}

constexpr auto Clear = Rgba { 0, 0, 0, 0 };

} // namespace

TEST_CASE("rasterizeVectorGlyph: a foreground-only glyph is coverage", "[VectorGlyphRasterizer]")
{
    // Y points down: the square's top-left corner is the window's.
    auto const drawn = draw("fill=nonzero,fg:M0 0h4v4H0Z");
    REQUIRE(drawn.format == GlyphPixels::Coverage);
    REQUIRE(drawn.pixels.size() == 64);
    CHECK(coverageAt(drawn, 0, 0) == 255);
    CHECK(coverageAt(drawn, 3, 3) == 255);
    CHECK(coverageAt(drawn, 4, 3) == 0);
    CHECK(coverageAt(drawn, 3, 4) == 0);

    // The foreground's alpha is the coverage, layers compositing into it.
    CHECK(coverageAt(draw("fill=nonzero,fg@0.5:M0 0h8v8H0Z"), 4, 4) == 128);
    CHECK(coverageAt(draw("fill=nonzero,fg@0.5:M0 0h8v8H0Z|fill=nonzero,fg@0.5:M0 0h8v8H0Z"), 4, 4) == 192);
    // Whatever the foreground is: it is not in the pixels.
    CHECK(rasterizeVectorGlyph(
              glyphOf("fill=nonzero,fg:M0 0h4v4H0Z"), PixelUnits, Window, {}, RGBColor { 9, 9, 9 })
              .pixels
          == drawn.pixels);
}

TEST_CASE("rasterizeVectorGlyph: a glyph with a fixed colour is drawn in colour", "[VectorGlyphRasterizer]")
{
    auto const drawn = draw("fill=nonzero,#ff8000:M0 0h4v8H0Z|fill=nonzero,fg:M4 0h4v8H4Z");
    REQUIRE(drawn.format == GlyphPixels::Color);
    REQUIRE(drawn.pixels.size() == 64 * 4);
    CHECK(colorAt(drawn, 1, 1) == Rgba { 0xFF, 0x80, 0x00, 0xFF });
    // The foreground is resolved to the one given.
    CHECK(colorAt(drawn, 6, 1) == Rgba { 0x00, 0x00, 0xFF, 0xFF });

    // Colours are premultiplied by their alpha.
    CHECK(colorAt(draw("fill=nonzero,#ff800080:M0 0h8v8H0Z"), 4, 4) == Rgba { 128, 64, 0, 128 });
    CHECK(colorAt(draw("fill=nonzero,#ff0000:M0 0h1v1H0Z"), 4, 4) == Clear);
}

TEST_CASE("rasterizeVectorGlyph: layers are painted in order, each over the last", "[VectorGlyphRasterizer]")
{
    auto const drawn = draw("fill=nonzero,#ff0000:M0 0h8v8H0Z|fill=nonzero,#00ff00:M2 2h4v4H2Z");
    CHECK(colorAt(drawn, 0, 0) == Rgba { 0xFF, 0, 0, 0xFF });
    CHECK(colorAt(drawn, 3, 3) == Rgba { 0, 0xFF, 0, 0xFF });

    // A half-transparent layer lets half of what is under it through.
    auto const blended = draw("fill=nonzero,#ff0000:M0 0h8v8H0Z|fill=nonzero,#0000ff80:M0 0h8v8H0Z");
    CHECK(colorAt(blended, 3, 3) == Rgba { 127, 0, 128, 0xFF });
}

TEST_CASE("rasterizeVectorGlyph: fill rules", "[VectorGlyphRasterizer]")
{
    // Two squares wound the same way, one inside the other.
    auto const path = std::string_view { "M0 0h8v8H0ZM2 2h4v4H2Z" };
    CHECK(coverageAt(draw(std::format("fill=nonzero,fg:{}", path)), 3, 3) == 255);
    CHECK(coverageAt(draw(std::format("fill=evenodd,fg:{}", path)), 3, 3) == 0);
    CHECK(coverageAt(draw(std::format("fill=evenodd,fg:{}", path)), 0, 0) == 255);
}

TEST_CASE("rasterizeVectorGlyph: strokes", "[VectorGlyphRasterizer]")
{
    // A horizontal line through the middle, 2 wide: rows 3 and 4.
    auto const butt = draw("stroke=2/butt,fg:M2 4h4");
    CHECK(coverageAt(butt, 3, 3) == 255);
    CHECK(coverageAt(butt, 3, 4) == 255);
    CHECK(coverageAt(butt, 3, 2) == 0);
    CHECK(coverageAt(butt, 3, 5) == 0);
    CHECK(coverageAt(butt, 1, 3) == 0); // nothing past its end
    CHECK(coverageAt(butt, 6, 3) == 0);
    // A square cap reaches a pixel further; a round one part of it.
    CHECK(coverageAt(draw("stroke=2/square,fg:M2 4h4"), 1, 3) == 255);
    auto const round = coverageAt(draw("stroke=2,fg:M2 4h4"), 1, 3);
    CHECK(round > 150);
    CHECK(round < 230);

    // A closed path strokes its outline and leaves its inside.
    auto const ring = draw("stroke=1/butt/miter,fg:M1.5 1.5h5v5h-5Z");
    CHECK(coverageAt(ring, 1, 1) == 255);
    CHECK(coverageAt(ring, 6, 6) == 255);
    CHECK(coverageAt(ring, 4, 4) == 0);
}

TEST_CASE("rasterizeVectorGlyph: a layer's matrix moves its path, its stroke and its gradient",
          "[VectorGlyphRasterizer]")
{
    // The same unit square, drawn at (2, 2) scaled by 4.
    auto const moved = draw("fill=nonzero,fg,m 4 0 0 4 2 2:M0 0h1v1H0Z");
    CHECK(coverageAt(moved, 2, 2) == 255);
    CHECK(coverageAt(moved, 5, 5) == 255);
    CHECK(coverageAt(moved, 1, 1) == 0);
    CHECK(coverageAt(moved, 6, 6) == 0);

    // A stroke goes through it too: a pen 1 wide, stretched 4 times down, is 4 pixels tall on a
    // horizontal line -- the elliptical pen SVG draws.
    auto const stretched = draw("stroke=1/butt,fg,m 1 0 0 4 0 4:M1 0h6");
    CHECK(coverageAt(stretched, 4, 2) == 255);
    CHECK(coverageAt(stretched, 4, 5) == 255);
    CHECK(coverageAt(stretched, 4, 1) == 0);
    CHECK(coverageAt(stretched, 4, 6) == 0);

    // And a gradient: one across the unit square is, scaled by 8, one across the window.
    auto const gradient = draw("fill=nonzero,lin(0 0 1 0 / 0 #000000 / 1 #ffffff),m 8 0 0 8 0 0:M0 0h1v1H0Z");
    CHECK(colorAt(gradient, 0, 4)[0] < 32);
    CHECK(colorAt(gradient, 7, 4)[0] > 223);
}

TEST_CASE("rasterizeVectorGlyph: a clip limits only the layers it covers", "[VectorGlyphRasterizer]")
{
    // The clip is the left half and covers one layer; the layer after that is not clipped.
    auto const drawn =
        draw("clip=1,nonzero:M0 0h4v8H0Z|fill=nonzero,#ff0000:M0 0h8v4H0Z|fill=nonzero,#00ff00:M0 4h8v4H0Z");
    CHECK(colorAt(drawn, 1, 1) == Rgba { 0xFF, 0, 0, 0xFF });
    CHECK(colorAt(drawn, 6, 1) == Clear); // the red, outside its clip
    CHECK(colorAt(drawn, 1, 6) == Rgba { 0, 0xFF, 0, 0xFF });
    CHECK(colorAt(drawn, 6, 6) == Rgba { 0, 0xFF, 0, 0xFF }); // the green, which has none

    // A clip's edge is antialiased like any other.
    CHECK(coverageAt(draw("clip=1,nonzero:M0 0h3.5v8H0Z|fill=nonzero,fg:M0 0h8v8H0Z"), 3, 4) == 128);
    // A clip that encloses no area hides what it covers.
    CHECK(coverageAt(draw("clip=1,nonzero:M0 0h8|fill=nonzero,fg:M0 0h8v8H0Z"), 4, 4) == 0);
    // A stroke is clipped as a fill is.
    CHECK(coverageAt(draw("clip=1,nonzero:M0 0h4v8H0Z|stroke=2/butt,fg:M0 4h8"), 6, 4) == 0);
}

TEST_CASE("rasterizeVectorGlyph: a linear gradient runs from its first point to its second",
          "[VectorGlyphRasterizer]")
{
    auto const drawn = draw("fill=nonzero,lin(0 0 8 0 / 0 #000000 / 1 #ffffff):M0 0h8v8H0Z");
    // Sampled at the middle of each pixel: 0.5 / 8 of the way at the first, 7.5 / 8 at the last.
    CHECK(colorAt(drawn, 0, 0)[0] == 16);
    CHECK(colorAt(drawn, 4, 0)[0] == 143);
    CHECK(colorAt(drawn, 7, 0)[0] == 239);
    // The same down every column.
    CHECK(colorAt(drawn, 4, 7) == colorAt(drawn, 4, 0));

    SECTION("stops, and stops that share an offset")
    {
        auto const hard =
            draw("fill=nonzero,lin(0 0 8 0 / 0 #ff0000 / 0.5 #ff0000 / 0.5 #0000ff / 1 #0000ff):M0 0h8v8H0Z");
        CHECK(colorAt(hard, 3, 0) == Rgba { 0xFF, 0, 0, 0xFF });
        CHECK(colorAt(hard, 4, 0) == Rgba { 0, 0, 0xFF, 0xFF });
    }

    SECTION("past its ends it pads, repeats or reflects")
    {
        // The gradient covers only the left half of the window.
        auto const at = [](std::string_view spread, int x) {
            return colorAt(
                draw(std::format("fill=nonzero,lin(0 0 4 0 {} / 0 #000000 / 1 #ffffff):M0 0h8v8H0Z", spread)),
                x,
                0)[0];
        };
        CHECK(at("pad", 6) == 255);
        CHECK(at("repeat", 4) == at("repeat", 0));
        CHECK(at("repeat", 6) == at("repeat", 2));
        CHECK(at("reflect", 4) == at("reflect", 3));
        CHECK(at("reflect", 7) == at("reflect", 0));
    }

    SECTION("a gradient of no extent paints its last stop")
    {
        CHECK(colorAt(draw("fill=nonzero,lin(4 4 4 4 / 0 #ff0000 / 1 #0000ff):M0 0h8v8H0Z"), 2, 2)
              == Rgba { 0, 0, 0xFF, 0xFF });
    }

    SECTION("alpha is mixed like the colours, and then multiplied in")
    {
        auto const fading = draw("fill=nonzero,lin(0 0 8 0 / 0 #ff000000 / 1 #ff0000ff):M0 0h8v8H0Z");
        CHECK(colorAt(fading, 4, 0) == Rgba { 143, 0, 0, 143 });
    }

    SECTION("its own matrix maps it into the layer")
    {
        // The gradient runs along x in its own space, and is turned a quarter to run down the window.
        auto const turned =
            draw("fill=nonzero,lin(0 0 8 0 m 0 1 -1 0 0 0 / 0 #000000 / 1 #ffffff):M0 0h8v8H0Z");
        CHECK(colorAt(turned, 4, 0)[0] == 16);
        CHECK(colorAt(turned, 4, 7)[0] == 239);
        CHECK(colorAt(turned, 0, 4) == colorAt(turned, 7, 4));
    }
}

TEST_CASE("rasterizeVectorGlyph: a radial gradient changes from its centre out", "[VectorGlyphRasterizer]")
{
    auto const drawn = draw("fill=nonzero,rad(4 4 0 4 4 4 / 0 #ffffff / 1 #000000):M0 0h8v8H0Z");
    auto const centre = colorAt(drawn, 4, 4)[0];
    auto const halfway = colorAt(drawn, 6, 4)[0];
    auto const corner = colorAt(drawn, 0, 0)[0];
    CHECK(centre > halfway);
    CHECK(halfway > corner);
    CHECK(corner == 0); // past the outer circle it pads
    // The same in every direction.
    CHECK(colorAt(drawn, 6, 4) == colorAt(drawn, 1, 3));
    CHECK(colorAt(drawn, 4, 6) == colorAt(drawn, 3, 1));

    // A focal point off the centre brightens the side it is on.
    auto const focal = draw("fill=nonzero,rad(2 4 0 4 4 4 / 0 #ffffff / 1 #000000):M0 0h8v8H0Z");
    CHECK(colorAt(focal, 2, 4)[0] > colorAt(focal, 5, 4)[0]);

    // Two circles that are the same have no extent: the last stop.
    CHECK(colorAt(draw("fill=nonzero,rad(4 4 2 4 4 2 / 0 #ff0000 / 1 #0000ff):M0 0h8v8H0Z"), 1, 1)
          == Rgba { 0, 0, 0xFF, 0xFF });
}

TEST_CASE("rasterizeVectorGlyph: a foreground gradient is still coverage", "[VectorGlyphRasterizer]")
{
    // Every stop is the foreground, so the alphas are all there is to draw: a glyph that fades out.
    auto const drawn = draw("fill=nonzero,lin(0 0 8 0 / 0 fg / 1 fg@0):M0 0h8v8H0Z");
    REQUIRE(drawn.format == GlyphPixels::Coverage);
    CHECK(coverageAt(drawn, 0, 4) == 239);
    CHECK(coverageAt(drawn, 7, 4) == 16);
}

TEST_CASE("rasterizeVectorGlyph: placement scales and moves the viewBox", "[VectorGlyphRasterizer]")
{
    // A 24-unit viewBox into the 8 pixel window: three units a pixel.
    auto const third = GlyphSpanTransform { .sx = 1.0F / 3, .sy = 1.0F / 3, .tx = 0, .ty = 0 };
    auto const drawn = rasterizeVectorGlyph(
        glyphOf("fill=nonzero,fg:M6 6h12v12H6Z", VectorViewBox { .x = 0, .y = 0, .width = 24, .height = 24 }),
        third,
        Window,
        {},
        Blue);
    CHECK(coverageAt(drawn, 2, 2) == 255);
    CHECK(coverageAt(drawn, 5, 5) == 255);
    CHECK(coverageAt(drawn, 1, 1) == 0);
    CHECK(coverageAt(drawn, 6, 6) == 0);
}

TEST_CASE("rasterizeVectorGlyph: tiles cut from a span join into the span", "[VectorGlyphRasterizer]")
{
    // How a two-cell glyph is drawn: each cell's tile is a window of the same span, here 16 pixels of a
    // gradient and a stroked curve, each of which must carry across the seam.
    auto const wide = VectorViewBox { .x = 0, .y = 0, .width = 16, .height = 8 };
    auto const glyph = glyphOf(
        "fill=nonzero,lin(0 0 16 0 / 0 #000000 / 1 #ffffff):M0 0h16v8H0Z|stroke=2,#ff0000:M1 7Q8 -6 15 7",
        wide);
    auto const whole = rasterizeVectorGlyph(glyph, PixelUnits, ImageSize { Width(16), Height(8) }, {}, Blue);
    auto const left =
        rasterizeVectorGlyph(glyph, PixelUnits, Window, GlyphWindowOrigin { .x = 0, .y = 0 }, Blue);
    auto const right =
        rasterizeVectorGlyph(glyph, PixelUnits, Window, GlyphWindowOrigin { .x = 8, .y = 0 }, Blue);
    for (auto const y: std::views::iota(0, 8))
        for (auto const x: std::views::iota(0, 8))
            for (auto const channel: std::views::iota(0, 4))
            {
                INFO("pixel " << x << ", " << y << " channel " << channel);
                auto const inWhole = [&](int column) {
                    return whole.pixels[static_cast<size_t>((((y * 16) + column) * 4) + channel)];
                };
                CHECK(left.pixels[static_cast<size_t>((((y * 8) + x) * 4) + channel)] == inWhole(x));
                CHECK(right.pixels[static_cast<size_t>((((y * 8) + x) * 4) + channel)] == inWhole(x + 8));
            }
}

TEST_CASE("rasterizeVectorGlyph: degenerate input draws nothing", "[VectorGlyphRasterizer]")
{
    // Path data that only moves.
    CHECK(std::ranges::all_of(draw("fill=nonzero,fg:M4 4").pixels, [](uint8_t value) { return value == 0; }));
    // A window of no size.
    CHECK(rasterizeVectorGlyph(glyphOf("fill=nonzero,fg:M0 0h8v8H0Z"), PixelUnits, ImageSize {}, {}, Blue)
              .pixels.empty());
}
