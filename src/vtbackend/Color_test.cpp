// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>

#include <catch2/catch.hpp>

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
    RGBColor rgb0 = RGBColor { 0x12, 0x34, 0x56 };
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
