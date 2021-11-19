/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Color.h>
#include <catch2/catch_all.hpp>

using namespace terminal;

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
    RGBColor rgb0 = RGBColor{0x12, 0x34, 0x56};
    CHECK(rgb0.red == 0x12);
    CHECK(rgb0.green == 0x34);
    CHECK(rgb0.blue == 0x56);

    Color const c = Color(RGBColor{0x12, 0x34, 0x56});
    REQUIRE(isRGBColor(c));
    auto const rgb = getRGBColor(c);
    CHECK(rgb.red == 0x12);
    CHECK(rgb.green == 0x34);
    CHECK(rgb.blue == 0x56);
}

