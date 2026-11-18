// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/RegisteredGlyphPlacement.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// @return A matcher for @p value to within a thousandth of a pixel, which is far below what can be seen.
[[nodiscard]] Catch::Approx approx(double value)
{
    return Catch::Approx(value).margin(0.001);
}

/// A cell 10 pixels wide and 20 high.
constexpr auto OneCell = GlyphSpanGeometry { .width = 10, .height = 20 };

/// A square viewBox that does not start at the origin: x 100..124, y 200..224.
constexpr auto Box = VectorViewBox { .x = 100, .y = 200, .width = 24, .height = 24 };

/// A pixel of the span.
struct Pixel
{
    float x = 0;
    float y = 0;
};

/// @return Where @p transform puts the viewBox point (@p x, @p y).
[[nodiscard]] constexpr Pixel at(GlyphSpanTransform const& transform, float x, float y) noexcept
{
    return { .x = transform.tx + (x * transform.sx), .y = transform.ty + (y * transform.sy) };
}

[[nodiscard]] constexpr GlyphLayout layoutOf(GlyphFit fit,
                                             GlyphAlignment horizontal = GlyphAlignment::Center,
                                             GlyphAlignment vertical = GlyphAlignment::Center) noexcept
{
    return { .fit = fit, .horizontal = horizontal, .vertical = vertical };
}

} // namespace

TEST_CASE("placeRegisteredGlyph: fit", "[RegisteredGlyphPlacement]")
{
    // The viewBox is 24 x 24 and the cell 10 x 20: the width fits at 10/24 and the height at 20/24.
    auto const contain = placeRegisteredGlyph(layoutOf(GlyphFit::Contain), Box, OneCell).value();
    CHECK(contain.sx == approx(10.0 / 24));
    CHECK(contain.sy == approx(10.0 / 24));

    auto const cover = placeRegisteredGlyph(layoutOf(GlyphFit::Cover), Box, OneCell).value();
    CHECK(cover.sx == approx(20.0 / 24));
    CHECK(cover.sy == approx(20.0 / 24));

    auto const stretch = placeRegisteredGlyph(layoutOf(GlyphFit::Stretch), Box, OneCell).value();
    CHECK(stretch.sx == approx(10.0 / 24));
    CHECK(stretch.sy == approx(20.0 / 24));
}

TEST_CASE("placeRegisteredGlyph: stretched, the viewBox's corners are the span's",
          "[RegisteredGlyphPlacement]")
{
    // Y points down in both, so the viewBox's top-left is the span's.
    auto const transform = placeRegisteredGlyph(layoutOf(GlyphFit::Stretch), Box, OneCell).value();
    CHECK(at(transform, 100, 200).x == approx(0));
    CHECK(at(transform, 100, 200).y == approx(0));
    CHECK(at(transform, 124, 224).x == approx(10));
    CHECK(at(transform, 124, 224).y == approx(20));
}

TEST_CASE("placeRegisteredGlyph: alignment of a contained viewBox", "[RegisteredGlyphPlacement]")
{
    // Contained, the viewBox is 10 x 10 pixels in a cell 10 x 20: no slack across, 10 pixels of it down.
    using enum GlyphAlignment;
    auto const topOf = [](GlyphAlignment vertical) {
        return at(placeRegisteredGlyph(layoutOf(GlyphFit::Contain, Center, vertical), Box, OneCell).value(),
                  100,
                  200)
            .y;
    };
    CHECK(topOf(Start) == approx(0)); // the top, as SVG's yMin
    CHECK(topOf(Center) == approx(5));
    CHECK(topOf(End) == approx(10));

    // In a span two cells wide the slack is across: a 20 x 20 viewBox in 40 x 20.
    auto const wide = GlyphSpanGeometry { .width = 40, .height = 20 };
    auto const leftOf = [&](GlyphAlignment horizontal) {
        return at(placeRegisteredGlyph(layoutOf(GlyphFit::Contain, horizontal, Center), Box, wide).value(),
                  100,
                  200)
            .x;
    };
    CHECK(leftOf(Start) == approx(0));
    CHECK(leftOf(Center) == approx(10));
    CHECK(leftOf(End) == approx(20));
}

TEST_CASE("placeRegisteredGlyph: alignment of a covering viewBox picks what is cut off",
          "[RegisteredGlyphPlacement]")
{
    // Covering, the viewBox is 20 x 20 pixels in a cell 10 wide: 10 pixels of it do not fit across.
    using enum GlyphAlignment;
    auto const leftOf = [](GlyphAlignment horizontal) {
        return at(placeRegisteredGlyph(layoutOf(GlyphFit::Cover, horizontal, Center), Box, OneCell).value(),
                  100,
                  200)
            .x;
    };
    CHECK(leftOf(Start) == approx(0)); // its left edge on the cell's, its right half cut off
    CHECK(leftOf(Center) == approx(-5));
    CHECK(leftOf(End) == approx(-10)); // its right edge on the cell's
}

TEST_CASE("placeRegisteredGlyph: nothing to draw into", "[RegisteredGlyphPlacement]")
{
    CHECK_FALSE(placeRegisteredGlyph(GlyphLayout {}, Box, GlyphSpanGeometry {}).has_value());
    CHECK_FALSE(placeRegisteredGlyph(GlyphLayout {}, VectorViewBox {}, OneCell).has_value());
}

TEST_CASE("registeredGlyphSpan: two cells only where the next one is blank", "[RegisteredGlyphPlacement]")
{
    auto const cellAt = [](int column, std::u32string text) {
        auto cell = RenderCell {};
        cell.position = CellLocation { .line = LineOffset(3), .column = ColumnOffset(column) };
        cell.codepoints = std::move(text);
        return cell;
    };
    auto wide = RegisteredGlyph {};
    wide.definition.layout.span = GlyphRenderSpan::TwoCells;
    auto const narrow = RegisteredGlyph {};
    auto const cell = cellAt(4, U"\U00100000");

    CHECK(registeredGlyphSpan(wide, cell, nullptr) == GlyphRenderSpan::OneCell); // the last column

    auto const empty = cellAt(5, U"");
    auto const space = cellAt(5, U" ");
    auto const letter = cellAt(5, U"x");
    CHECK(registeredGlyphSpan(wide, cell, &empty) == GlyphRenderSpan::TwoCells);
    CHECK(registeredGlyphSpan(wide, cell, &space) == GlyphRenderSpan::TwoCells);
    CHECK(registeredGlyphSpan(wide, cell, &letter) == GlyphRenderSpan::OneCell);

    // A glyph registered one cell wide never spills, however blank what follows.
    CHECK(registeredGlyphSpan(narrow, cell, &empty) == GlyphRenderSpan::OneCell);

    // The cell rendered next is not the one beside it: another row, or a gap.
    auto nextRow = cellAt(5, U"");
    nextRow.position.line = LineOffset(4);
    CHECK(registeredGlyphSpan(wide, cell, &nextRow) == GlyphRenderSpan::OneCell);
    auto const apart = cellAt(6, U"");
    CHECK(registeredGlyphSpan(wide, cell, &apart) == GlyphRenderSpan::OneCell);

    // On a double-width line the cell beside it is two display columns on.
    auto doubled = cell;
    doubled.attributes.lineFlags = LineFlag::DoubleWidth;
    CHECK(registeredGlyphSpan(wide, doubled, &apart) == GlyphRenderSpan::TwoCells);
    CHECK(registeredGlyphSpan(wide, doubled, &empty) == GlyphRenderSpan::OneCell);
}
