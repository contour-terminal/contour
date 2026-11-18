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

/// A cell 10 pixels wide and 20 high, its baseline 15 down.
constexpr auto OneCell = GlyphSpanGeometry { .width = 10, .height = 20, .baseline = 15 };

/// An outline occupying x 100..600 and y -200..800.
constexpr auto Bounds = GlyphOutlineBounds { .xMin = 100, .yMin = -200, .xMax = 600, .yMax = 800 };

/// A pixel of the span.
struct Pixel
{
    float x = 0;
    float y = 0;
};

/// @return Where @p transform puts the outline point (@p x, @p y).
[[nodiscard]] constexpr Pixel at(GlyphSpanTransform const& transform, float x, float y) noexcept
{
    return { .x = transform.tx + (x * transform.sx), .y = transform.ty - (y * transform.sy) };
}

/// A layout declaring the extent x 0..1000, y -250..750 (an em square with a descender).
[[nodiscard]] constexpr GlyphLayout declared(GlyphSizeMode size) noexcept
{
    return { .advanceWidth = 1000, .lineHeight = 1000, .extentBottom = -250, .size = size };
}

} // namespace

TEST_CASE("placeRegisteredGlyph: size modes on a declared extent", "[RegisteredGlyphPlacement]")
{
    // The extent is 1000 x 1000 and the cell 10 x 20, so the width fits at 0.01 and the height at 0.02.
    struct Case
    {
        GlyphSizeMode size;
        float sx;
        float sy;
    };
    for (auto const& [size, sx, sy]: {
             Case { .size = GlyphSizeMode::Contain, .sx = 0.01F, .sy = 0.01F },
             Case { .size = GlyphSizeMode::Advance, .sx = 0.01F, .sy = 0.01F },
             Case { .size = GlyphSizeMode::Height, .sx = 0.02F, .sy = 0.02F },
             Case { .size = GlyphSizeMode::Cover, .sx = 0.02F, .sy = 0.02F },
             Case { .size = GlyphSizeMode::Stretch, .sx = 0.01F, .sy = 0.02F },
         })
    {
        auto const transform = placeRegisteredGlyph(declared(size), Bounds, OneCell);
        REQUIRE(transform.has_value());
        CHECK(transform->sx == approx(sx));
        CHECK(transform->sy == approx(sy));
    }
}

TEST_CASE("placeRegisteredGlyph: a declared extent ignores the outline's bounds",
          "[RegisteredGlyphPlacement]")
{
    // What keeps the glyphs of a set from shifting against each other.
    auto const layout = declared(GlyphSizeMode::Contain);
    auto const other = GlyphOutlineBounds { .xMin = 400, .yMin = 0, .xMax = 450, .yMax = 10 };
    CHECK(placeRegisteredGlyph(layout, Bounds, OneCell) == placeRegisteredGlyph(layout, other, OneCell));
}

TEST_CASE("placeRegisteredGlyph: alignment of a contained extent", "[RegisteredGlyphPlacement]")
{
    // Contained, the extent is 10 x 10 pixels in a cell 10 x 20: no slack across, 10 pixels of it down.
    auto layout = declared(GlyphSizeMode::Contain);

    SECTION("centered by default")
    {
        auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
        CHECK(at(transform, 0, -250).y == approx(15)); // the extent's bottom
        CHECK(at(transform, 0, 750).y == approx(5));   // its top
        CHECK(at(transform, 0, 0).x == approx(0));
        CHECK(at(transform, 1000, 0).x == approx(10));
    }

    SECTION("start is the bottom")
    {
        layout.vertical = GlyphVerticalAlignment::Start;
        auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
        CHECK(at(transform, 0, -250).y == approx(20));
        CHECK(at(transform, 0, 750).y == approx(10));
    }

    SECTION("end is the top")
    {
        layout.vertical = GlyphVerticalAlignment::End;
        auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
        CHECK(at(transform, 0, 750).y == approx(0));
        CHECK(at(transform, 0, -250).y == approx(10));
    }

    SECTION("baseline puts y = 0 on the text baseline")
    {
        layout.vertical = GlyphVerticalAlignment::Baseline;
        auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
        CHECK(at(transform, 0, 0).y == approx(15));
        CHECK(at(transform, 0, -250).y == approx(17.5)); // the descender hangs below it
    }
}

TEST_CASE("placeRegisteredGlyph: horizontal alignment", "[RegisteredGlyphPlacement]")
{
    // Sized by height in a two-cell span: the extent is 20 pixels square in 20 x 20, so widen the span.
    auto const wide = GlyphSpanGeometry { .width = 40, .height = 20, .baseline = 15 };
    auto layout = declared(GlyphSizeMode::Height);

    layout.horizontal = GlyphHorizontalAlignment::Start;
    CHECK(at(placeRegisteredGlyph(layout, Bounds, wide).value(), 0, 0).x == approx(0));
    layout.horizontal = GlyphHorizontalAlignment::Center;
    CHECK(at(placeRegisteredGlyph(layout, Bounds, wide).value(), 0, 0).x == approx(10));
    layout.horizontal = GlyphHorizontalAlignment::End;
    CHECK(at(placeRegisteredGlyph(layout, Bounds, wide).value(), 1000, 0).x == approx(40));
}

TEST_CASE("placeRegisteredGlyph: stretch fills the span whatever the alignment", "[RegisteredGlyphPlacement]")
{
    auto layout = declared(GlyphSizeMode::Stretch);
    layout.vertical = GlyphVerticalAlignment::Baseline;
    layout.horizontal = GlyphHorizontalAlignment::End;
    auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
    CHECK(at(transform, 0, -250).x == approx(0));
    CHECK(at(transform, 0, -250).y == approx(20));
    CHECK(at(transform, 1000, 750).x == approx(10));
    CHECK(at(transform, 1000, 750).y == approx(0));
}

TEST_CASE("placeRegisteredGlyph: padding shrinks the span", "[RegisteredGlyphPlacement]")
{
    auto layout = declared(GlyphSizeMode::Stretch);
    layout.padding = { .top = 0.1F, .right = 0.2F, .bottom = 0.3F, .left = 0.4F };
    auto const transform = placeRegisteredGlyph(layout, Bounds, OneCell).value();
    CHECK(at(transform, 0, -250).x == approx(4));   // 0.4 of 10 from the left
    CHECK(at(transform, 1000, 750).x == approx(8)); // 0.2 of 10 from the right
    CHECK(at(transform, 0, 750).y == approx(2));    // 0.1 of 20 from the top
    CHECK(at(transform, 0, -250).y == approx(14));  // 0.3 of 20 from the bottom
}

TEST_CASE("placeRegisteredGlyph: an undeclared extent is the outline's bounds", "[RegisteredGlyphPlacement]")
{
    // Bounds of 500 x 1000 units, stretched: the bounds' corners land on the span's.
    auto const stretched =
        placeRegisteredGlyph(GlyphLayout { .size = GlyphSizeMode::Stretch }, Bounds, OneCell).value();
    CHECK(at(stretched, 100, -200).x == approx(0));
    CHECK(at(stretched, 100, -200).y == approx(20));
    CHECK(at(stretched, 600, 800).x == approx(10));
    CHECK(at(stretched, 600, 800).y == approx(0));

    // Contained, it is capped by the em: 10 pixels (the cell's smaller side) per 1000 units, so a small
    // outline stays small instead of growing to fill the cell.
    auto const dot = GlyphOutlineBounds { .xMin = 0, .yMin = 0, .xMax = 100, .yMax = 100 };
    auto const contained = placeRegisteredGlyph(GlyphLayout {}, dot, OneCell).value();
    CHECK(contained.sx == approx(0.01F));
    CHECK(contained.sy == approx(0.01F));
    // And centered: a 1 x 1 pixel dot in the middle of the cell.
    CHECK(at(contained, 0, 0).x == approx(4.5));
    CHECK(at(contained, 0, 0).y == approx(10.5));
}

TEST_CASE("placeRegisteredGlyph: nothing to draw", "[RegisteredGlyphPlacement]")
{
    auto const flat = GlyphOutlineBounds { .xMin = 0, .yMin = 5, .xMax = 100, .yMax = 5 };
    CHECK_FALSE(placeRegisteredGlyph(GlyphLayout {}, flat, OneCell).has_value());
    CHECK_FALSE(placeRegisteredGlyph(GlyphLayout {}, GlyphOutlineBounds {}, OneCell).has_value());
    CHECK_FALSE(
        placeRegisteredGlyph(declared(GlyphSizeMode::Contain), Bounds, GlyphSpanGeometry {}).has_value());
}

TEST_CASE("registeredGlyphSpan: two cells only where the next one is blank", "[RegisteredGlyphPlacement]")
{
    auto const cellAt = [](int column, std::u32string text) {
        auto cell = RenderCell {};
        cell.position = CellLocation { .line = LineOffset(3), .column = ColumnOffset(column) };
        cell.codepoints = std::move(text);
        return cell;
    };
    auto const wide = RegisteredGlyph { .layout = GlyphLayout { .span = GlyphRenderSpan::TwoCells } };
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
