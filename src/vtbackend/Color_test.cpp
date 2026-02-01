// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("Color.Bright", "[Color]")
{
    Color const c = Color(BrightColor::Cyan);
    REQUIRE(isBrightColor(c));
    REQUIRE(getBrightColor(c) == int(BrightColor::Cyan));
}

TEST_CASE("Color.Indexed", "[Color]")
{
    Color const c = Color(IndexedColor::Blue);
    REQUIRE(isIndexedColor(c));
    REQUIRE(getIndexedColor(c) == int(IndexedColor::Blue));
}

TEST_CASE("Color.RGB", "[Color]")
{
    RGBColor const rgb0 = RGBColor { 0x12, 0x34, 0x56 };
    CHECK(rgb0.red == 0x12);
    CHECK(rgb0.green == 0x34);
    CHECK(rgb0.blue == 0x56);

    Color const c = Color(RGBColor { 0x12, 0x34, 0x56 });
    REQUIRE(isRGBColor(c));
    auto const rgb = getRGBColor(c);
    CHECK(rgb.red == 0x12);
    CHECK(rgb.green == 0x34);
    CHECK(rgb.blue == 0x56);
}

TEST_CASE("Color.mixColor.at_t0_returns_a", "[Color]")
{
    auto const a = RGBColor { 10, 20, 30 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 0.0f);
    CHECK(result.red == a.red);
    CHECK(result.green == a.green);
    CHECK(result.blue == a.blue);
}

TEST_CASE("Color.mixColor.at_t1_returns_b", "[Color]")
{
    auto const a = RGBColor { 10, 20, 30 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 1.0f);
    CHECK(result.red == b.red);
    CHECK(result.green == b.green);
    CHECK(result.blue == b.blue);
}

TEST_CASE("Color.mixColor.at_t05_returns_midpoint", "[Color]")
{
    auto const a = RGBColor { 0, 0, 0 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 0.5f);
    CHECK(result.red == 100);
    CHECK(result.green == 50);
    CHECK(result.blue == 25);
}

TEST_CASE("Color.mixColor.clamps_to_valid_range", "[Color]")
{
    auto const a = RGBColor { 250, 250, 250 };
    auto const b = RGBColor { 255, 255, 255 };
    // t > 1 would overshoot without clamping
    auto const result = mixColor(a, b, 2.0f);
    CHECK(result.red == 255);
    CHECK(result.green == 255);
    CHECK(result.blue == 255);
}

TEST_CASE("Color.mixColor.RGBColorPair_overload", "[Color]")
{
    auto const a =
        RGBColorPair { .foreground = RGBColor { 0, 0, 0 }, .background = RGBColor { 100, 100, 100 } };
    auto const b =
        RGBColorPair { .foreground = RGBColor { 200, 200, 200 }, .background = RGBColor { 50, 50, 50 } };

    auto const atZero = mixColor(a, b, 0.0f);
    CHECK(atZero.foreground.red == 0);
    CHECK(atZero.background.red == 100);

    auto const atOne = mixColor(a, b, 1.0f);
    CHECK(atOne.foreground.red == 200);
    CHECK(atOne.background.red == 50);

    auto const atHalf = mixColor(a, b, 0.5f);
    CHECK(atHalf.foreground.red == 100);
    CHECK(atHalf.background.red == 75);
}
