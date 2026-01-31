#include <vtbackend/primitives.h>

#include <text_shaper/shaper.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace text;
using namespace vtbackend;

TEST_CASE("scale alpha mask", "[scale]")
{
    rasterized_glyph glyph;
    glyph.format = bitmap_format::alpha_mask;
    glyph.bitmapSize = ImageSize { Width(10), Height(10) };
    glyph.bitmap.resize(10 * 10, 0xFF);
    glyph.position = { .x = 0, .y = 0 };

    ImageSize targetSize { Width(5), Height(5) };

    auto [scaled, factor] = scale(glyph, targetSize);

    CHECK(scaled.format == bitmap_format::alpha_mask);
    CHECK(scaled.bitmapSize.width == Width(5));
    CHECK(scaled.bitmapSize.height == Height(5));
    CHECK(factor == 2.0f);
}

TEST_CASE("scale non-integer ratio RGBA", "[scale]")
{
    rasterized_glyph glyph;
    glyph.format = bitmap_format::rgba;
    glyph.bitmapSize = ImageSize { Width(100), Height(100) };
    glyph.bitmap.resize(100 * 100 * 4, 255); // Fill with white
    glyph.position = { .x = 0, .y = 0 };

    ImageSize targetSize { Width(66), Height(66) };
    auto [scaled, factor] = scale(glyph, targetSize);

    CHECK(factor == Catch::Approx(1.515f).epsilon(0.01f));
    CHECK(scaled.bitmapSize.width == Width(66));
    CHECK(scaled.bitmapSize.height == Height(66));

    if (!scaled.bitmap.empty())
    {
        size_t lastPixelIndex = (unbox(scaled.bitmapSize.width) * unbox(scaled.bitmapSize.height) - 1) * 4;
        CHECK(scaled.bitmap[lastPixelIndex] == 255);
    }
}
