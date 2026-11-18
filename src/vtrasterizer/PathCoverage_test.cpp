// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/PathCoverage.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

constexpr auto Window = ImageSize { Width(8), Height(8) };

/// @return The rectangle from (@p left, @p top) to (@p right, @p bottom), in pixels.
[[nodiscard]] Polyline rectangle(float left, float top, float right, float bottom)
{
    return { .points = { { left, top }, { right, top }, { right, bottom }, { left, bottom } },
             .closure = PathClosure::Closed };
}

[[nodiscard]] Polyline reversed(Polyline polygon)
{
    std::ranges::reverse(polygon.points);
    return polygon;
}

/// @return The coverage of the pixel at column @p x of row @p y.
[[nodiscard]] float pixel(std::span<float const> pixels, int x, int y, int width = 8)
{
    return pixels[static_cast<size_t>((y * width) + x)];
}

/// @return The area covered, in pixels.
[[nodiscard]] double area(std::span<float const> pixels)
{
    return std::accumulate(pixels.begin(), pixels.end(), 0.0);
}

[[nodiscard]] std::vector<float> nonZero(std::vector<Polyline> const& polygons, ImageSize window = Window)
{
    return pathCoverage(polygons, VectorFillRule::NonZero, window);
}

[[nodiscard]] std::vector<float> evenOdd(std::vector<Polyline> const& polygons)
{
    return pathCoverage(polygons, VectorFillRule::EvenOdd, Window);
}

} // namespace

TEST_CASE("pathCoverage: a pixel-aligned rectangle covers exactly its pixels", "[PathCoverage]")
{
    auto const pixels = nonZero({ rectangle(2, 3, 6, 7) });
    REQUIRE(pixels.size() == 64);
    for (auto const y: std::views::iota(0, 8))
        for (auto const x: std::views::iota(0, 8))
        {
            INFO("pixel " << x << ", " << y);
            CHECK(pixel(pixels, x, y) == (x >= 2 && x < 6 && y >= 3 && y < 7 ? 1.0F : 0.0F));
        }
}

TEST_CASE("pathCoverage: an edge between pixels covers its share of them", "[PathCoverage]")
{
    // A unit square over the corner where four pixels meet: a quarter of each.
    auto const pixels = nonZero({ rectangle(1.5F, 5.5F, 2.5F, 6.5F) });
    CHECK(pixel(pixels, 1, 5) == Catch::Approx(0.25));
    CHECK(pixel(pixels, 2, 5) == Catch::Approx(0.25));
    CHECK(pixel(pixels, 1, 6) == Catch::Approx(0.25));
    CHECK(pixel(pixels, 2, 6) == Catch::Approx(0.25));
    CHECK(area(pixels) == Catch::Approx(1.0));

    // A triangle: half the rectangle around it.
    auto const triangle =
        Polyline { .points = { { 1, 1 }, { 7, 1 }, { 1, 7 } }, .closure = PathClosure::Closed };
    CHECK(area(nonZero({ triangle })) == Catch::Approx(18.0));
}

TEST_CASE("pathCoverage: every polygon is closed, and the way it is wound does not matter", "[PathCoverage]")
{
    auto open = rectangle(1, 1, 7, 7);
    open.closure = PathClosure::Open;
    CHECK(nonZero({ open }) == nonZero({ rectangle(1, 1, 7, 7) }));
    CHECK(nonZero({ reversed(rectangle(1, 1, 7, 7)) }) == nonZero({ rectangle(1, 1, 7, 7) }));
}

TEST_CASE("pathCoverage: non-zero and even-odd differ where polygons overlap", "[PathCoverage]")
{
    auto const outer = rectangle(1, 1, 7, 7);
    auto const inner = rectangle(3, 3, 5, 5);

    SECTION("wound the same way: one is inside the other by non-zero, a hole in it by even-odd")
    {
        CHECK(area(nonZero({ outer, inner })) == Catch::Approx(36.0));
        auto const pixels = evenOdd({ outer, inner });
        CHECK(pixel(pixels, 1, 1) == 1.0F);
        CHECK(pixel(pixels, 3, 3) == 0.0F);
        CHECK(pixel(pixels, 4, 4) == 0.0F);
        CHECK(area(pixels) == Catch::Approx(32.0));
    }

    SECTION("wound against each other: a hole by either rule")
    {
        CHECK(area(nonZero({ outer, reversed(inner) })) == Catch::Approx(32.0));
        CHECK(area(evenOdd({ outer, reversed(inner) })) == Catch::Approx(32.0));
    }

    SECTION("three deep, even-odd fills the innermost again")
    {
        auto const pixels = evenOdd({ rectangle(0, 0, 8, 8), rectangle(2, 2, 6, 6), rectangle(3, 3, 5, 5) });
        CHECK(pixel(pixels, 0, 0) == 1.0F);
        CHECK(pixel(pixels, 2, 2) == 0.0F);
        CHECK(pixel(pixels, 3, 3) == 1.0F);
    }

    SECTION("an antialiased edge stays one under even-odd")
    {
        // The triangle wave is 1 at an odd winding and falls off either side of it: a half-covered pixel
        // is half covered.
        auto const pixels = evenOdd({ rectangle(1.5F, 1, 6.5F, 7) });
        CHECK(pixel(pixels, 1, 3) == Catch::Approx(0.5));
        CHECK(pixel(pixels, 6, 3) == Catch::Approx(0.5));
        CHECK(pixel(pixels, 3, 3) == 1.0F);
    }
}

TEST_CASE("pathCoverage: what lies outside the window is clipped", "[PathCoverage]")
{
    SECTION("a polygon larger than the window covers it")
    {
        auto const pixels = nonZero({ rectangle(-100, -100, 100, 100) });
        CHECK(std::ranges::all_of(pixels, [](float value) { return value == 1.0F; }));
    }

    SECTION("a polygon wholly outside covers nothing")
    {
        for (auto const& outside: { rectangle(-20, 0, -10, 8),
                                    rectangle(10, 0, 20, 8),
                                    rectangle(0, -20, 8, -10),
                                    rectangle(0, 10, 8, 20) })
        {
            auto const pixels = nonZero({ outside });
            CHECK(std::ranges::all_of(pixels, [](float value) { return value == 0.0F; }));
        }
    }

    SECTION("a slanted edge leaving through the left border keeps its coverage inside")
    {
        // A triangle with a corner far to the left: within the window it is what a window-sized cut of
        // the same triangle, drawn into a window wide enough to hold all of it, shows there.
        auto const cut = nonZero({ Polyline { .points = { { -8, 8 }, { 8, 0 }, { 8, 8 } } } });
        auto const whole = nonZero({ Polyline { .points = { { 0, 8 }, { 16, 0 }, { 16, 8 } } } },
                                   ImageSize { Width(16), Height(8) });
        for (auto const y: std::views::iota(0, 8))
            for (auto const x: std::views::iota(0, 8))
            {
                INFO("pixel " << x << ", " << y);
                CHECK(pixel(cut, x, y) == Catch::Approx(pixel(whole, x + 8, y, 16)).margin(1e-5));
            }
    }

    SECTION("an edge on the left border itself covers the row beside it")
    {
        // Where an edge runs down the window's very first column, stepping along it must not land a hair
        // outside and wrap a pixel index.
        auto const curve = Polyline {
            .points = { { 4, 0 }, { 0.25F, 2.25F }, { 0, 4 }, { 0.25F, 5.75F }, { 4, 8 }, { 8, 8 }, { 8, 0 } }
        };
        auto const pixels = nonZero({ curve });
        CHECK(pixel(pixels, 0, 3) > 0.9F);
        CHECK(pixel(pixels, 0, 4) > 0.9F);
        CHECK(pixel(pixels, 0, 3) == Catch::Approx(pixel(pixels, 0, 4)));
    }
}

TEST_CASE("pathCoverage: degenerate input covers nothing", "[PathCoverage]")
{
    CHECK(nonZero({}) == std::vector<float>(64, 0.0F));
    CHECK(pathCoverage(std::vector { rectangle(1, 1, 7, 7) }, VectorFillRule::NonZero, ImageSize {}).empty());
    // A single point, and a line there and back, enclose no area.
    CHECK(area(nonZero({ Polyline { .points = { { 4, 4 } } } })) == 0.0);
    CHECK(area(nonZero({ Polyline { .points = { { 1, 1 }, { 7, 7 } } } })) == 0.0);
}
