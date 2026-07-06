// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ImageSize.h>

#include <catch2/catch_test_macros.hpp>

using vtpty::Height;
using vtpty::ImageSize;
using vtpty::Width;

namespace
{

constexpr ImageSize imageSize(unsigned width, unsigned height) noexcept
{
    return ImageSize { .width = Width(width), .height = Height(height) };
}

} // namespace

TEST_CASE("ImageSize.subtraction", "[vtpty][imagesize]")
{
    SECTION("plain component-wise subtraction")
    {
        CHECK(imageSize(10, 8) - imageSize(3, 5) == imageSize(7, 3));
    }

    SECTION("subtraction saturates at zero instead of wrapping")
    {
        // Each axis clamps independently; the other axis still subtracts normally.
        CHECK(imageSize(2, 5) - imageSize(3, 1) == imageSize(0, 4));
        CHECK(imageSize(5, 2) - imageSize(1, 3) == imageSize(4, 0));
        CHECK(imageSize(0, 0) - imageSize(1, 1) == imageSize(0, 0));
    }

    SECTION("subtracting an equal size yields zero, not a wrap")
    {
        CHECK(imageSize(7, 7) - imageSize(7, 7) == imageSize(0, 0));
    }
}

TEST_CASE("ImageSize.scalar multiplication rounds up", "[vtpty][imagesize]")
{
    SECTION("integral scale factors are exact")
    {
        CHECK(imageSize(10, 20) * 2.0 == imageSize(20, 40));
        CHECK(imageSize(10, 20) * 1.0 == imageSize(10, 20));
    }

    SECTION("fractional scale factors round up to the next full pixel")
    {
        // A render target sized with this must never be smaller than its content.
        CHECK(imageSize(10, 10) * 1.75 == imageSize(18, 18));
        CHECK(imageSize(101, 53) * 1.25 == imageSize(127, 67));
    }
}

TEST_CASE("ImageSize.scalar division rounds up", "[vtpty][imagesize]")
{
    SECTION("exact divisors divide exactly")
    {
        CHECK(imageSize(20, 40) / 2.0 == imageSize(10, 20));
    }

    SECTION("non-exact divisors keep partially covered pixels")
    {
        CHECK(imageSize(10, 10) / 3.0 == imageSize(4, 4));
        CHECK(imageSize(7, 5) / 2.0 == imageSize(4, 3));
    }
}

TEST_CASE("ImageSize.comparisons and area", "[vtpty][imagesize]")
{
    CHECK(imageSize(1, 2) == imageSize(1, 2));
    CHECK(imageSize(1, 2) != imageSize(2, 1));
    CHECK(imageSize(1, 5) < imageSize(2, 1));
    CHECK(imageSize(2, 1) < imageSize(2, 5));
    CHECK(imageSize(3, 4).area() == 12);

    CHECK(min(imageSize(3, 9), imageSize(5, 4)) == imageSize(3, 4));
    CHECK(max(imageSize(3, 9), imageSize(5, 4)) == imageSize(5, 9));
}
