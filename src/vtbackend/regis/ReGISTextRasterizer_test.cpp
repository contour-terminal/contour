// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>
#include <vtbackend/regis/ReGISTextRasterizer.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtbackend::regis;

TEST_CASE("EmbeddedReGISTextRasterizer.spaceIsBlank", "[regis][text]")
{
    auto const rasterizer = EmbeddedReGISTextRasterizer {};
    auto const glyph = rasterizer.rasterize(U' ', ImageSize { Width(16), Height(16) });
    REQUIRE(glyph.has_value());
    CHECK(glyph->size == ImageSize { Width(16), Height(16) });
    for (auto const c: glyph->coverage)
        CHECK(c == 0);
}

TEST_CASE("EmbeddedReGISTextRasterizer.letterHasCoverage", "[regis][text]")
{
    auto const rasterizer = EmbeddedReGISTextRasterizer {};
    // Scale a letter up so anti-aliasing produces both fully-covered and partial-coverage pixels.
    auto const glyph = rasterizer.rasterize(U'H', ImageSize { Width(32), Height(40) });
    REQUIRE(glyph.has_value());
    REQUIRE(glyph->coverage.size() == glyph->size.area());

    auto full = 0;
    auto partial = 0;
    for (auto const c: glyph->coverage)
    {
        if (c == 255)
            ++full;
        else if (c != 0)
            ++partial;
    }
    CHECK(full > 0);    // solid interior of the strokes
    CHECK(partial > 0); // anti-aliased edges
}

TEST_CASE("EmbeddedReGISTextRasterizer.degenerateSize", "[regis][text]")
{
    auto const rasterizer = EmbeddedReGISTextRasterizer {};
    CHECK_FALSE(rasterizer.rasterize(U'A', ImageSize { Width(0), Height(10) }).has_value());
}
