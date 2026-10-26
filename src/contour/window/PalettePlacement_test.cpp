// SPDX-License-Identifier: Apache-2.0
#include <contour/window/PalettePlacement.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::window::PaletteInputs;
using contour::window::PalettePlacement;
using contour::window::placePalette;

namespace
{
/// The MacOS row in a 1280 x 800 window, with the 16 pixel margin.
constexpr auto Base = PaletteInputs { .windowWidth = 1280,
                                      .windowHeight = 800,
                                      .width = 640,
                                      .maxHeight = 420,
                                      .marginX = 16,
                                      .marginY = 16,
                                      .topPercent = 18,
                                      .rowQuantum = 1 };
} // namespace

TEST_CASE("palette.centredEighteenPercentDown", "[paletteplacement]")
{
    CHECK(placePalette(Base) == PalettePlacement { .x = 320, .y = 144, .width = 640, .maxHeight = 420 });
}

TEST_CASE("palette.aSmallWindowShrinksIt", "[paletteplacement]")
{
    auto in = Base;
    in.windowWidth = 500;
    in.windowHeight = 300;
    CHECK(placePalette(in) == PalettePlacement { .x = 16, .y = 54, .width = 468, .maxHeight = 230 });
}

TEST_CASE("palette.neverNegative", "[paletteplacement]")
{
    auto in = Base;
    in.windowWidth = 20;
    in.windowHeight = 10;
    auto const placement = placePalette(in);
    CHECK(placement.width == 0);
    CHECK(placement.maxHeight == 0);
    CHECK(placement.x == 10);
}

TEST_CASE("palette.snapsToRowsOnTheTerminalRow", "[paletteplacement]")
{
    auto in = PaletteInputs { .windowWidth = 960,
                              .windowHeight = 600,
                              .width = 384,
                              .maxHeight = 360,
                              .marginX = 8,
                              .marginY = 18,
                              .topPercent = 18,
                              .rowQuantum = 18 };
    CHECK(placePalette(in).y == 108);

    in.windowHeight = 610; // 109.8, floored to a whole row
    auto const placement = placePalette(in);
    CHECK(placement.y == 108);
    CHECK(placement.maxHeight == 360);
}
