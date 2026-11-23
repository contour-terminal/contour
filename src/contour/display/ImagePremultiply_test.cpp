// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ImagePremultiply.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using contour::display::premultiplyAlpha;

TEST_CASE("premultiplyAlpha scales colour by coverage and keeps opaque and empty texels", "[image]")
{
    auto const straight = std::array<uint8_t, 16> {
        200, 100, 50,  255, // opaque: unchanged
        200, 100, 50,  128, // half covered: halved, rounded to nearest
        255, 255, 255, 0,   // empty: no colour left
        255, 0,   1,   1,   // barely covered: rounding keeps tiny values honest
    };
    auto premultiplied = std::array<uint8_t, 16> {};
    premultiplyAlpha(straight, premultiplied);

    CHECK(premultiplied
          == std::array<uint8_t, 16> {
              200,
              100,
              50,
              255, //
              100,
              50,
              25,
              128, //
              0,
              0,
              0,
              0, //
              1,
              0,
              0,
              1, //
          });
}

TEST_CASE("premultiplyAlpha copies a trailing partial pixel as it is", "[image]")
{
    auto const straight = std::array<uint8_t, 6> { 10, 20, 30, 0, 40, 50 };
    auto premultiplied = std::array<uint8_t, 6> {};
    premultiplyAlpha(straight, premultiplied);
    CHECK(premultiplied == std::array<uint8_t, 6> { 0, 0, 0, 0, 40, 50 });
}

static_assert(
    [] {
        auto const straight = std::array<uint8_t, 4> { 255, 255, 255, 51 };
        auto premultiplied = std::array<uint8_t, 4> {};
        premultiplyAlpha(straight, premultiplied);
        return premultiplied == std::array<uint8_t, 4> { 51, 51, 51, 51 };
    }(),
    "premultiplyAlpha is usable in constant expressions");
