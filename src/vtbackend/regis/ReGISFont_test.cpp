// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISFont.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend::regis;

static_assert(BasicFont.size() == 95, "the base font covers printable ASCII 0x20..0x7E");

TEST_CASE("ReGISFont.spaceIsBlank", "[regis][font]")
{
    auto const font = ReGISBitmapFont {};
    for (auto const row: font.glyphOf(' ').rows)
        CHECK(row == 0);
}

TEST_CASE("ReGISFont.letterHasInk", "[regis][font]")
{
    auto const font = ReGISBitmapFont {};
    auto anySet = false;
    for (auto const row: font.glyphOf('A').rows)
        if (row != 0)
            anySet = true;
    CHECK(anySet);
}

TEST_CASE("ReGISFont.outOfRangeIsBlank", "[regis][font]")
{
    auto const font = ReGISBitmapFont {};
    // A control character has no printable glyph and falls back to the blank cell.
    for (auto const row: font.glyphOf('\x01').rows)
        CHECK(row == 0);
}
