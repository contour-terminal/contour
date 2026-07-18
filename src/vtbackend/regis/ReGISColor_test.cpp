// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISColor.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtbackend::regis;

TEST_CASE("ReGISColor.hlsToRgb.primaries", "[regis][color]")
{
    // DEC HLS places blue at hue 0. Lightness 50, saturation 100 yields the pure primaries, matching
    // the Sixel colour introducer's convention.
    CHECK(hlsToRgb(0, 50, 100) == RGBColor { 0, 0, 255 });   // blue
    CHECK(hlsToRgb(120, 50, 100) == RGBColor { 255, 0, 0 }); // red
    CHECK(hlsToRgb(240, 50, 100) == RGBColor { 0, 255, 0 }); // green
}

TEST_CASE("ReGISColor.hlsToRgb.achromatic", "[regis][color]")
{
    // Zero saturation is a pure gray whatever the hue.
    CHECK(hlsToRgb(0, 0, 0) == RGBColor { 0, 0, 0 });
    CHECK(hlsToRgb(200, 100, 0) == RGBColor { 255, 255, 255 });
    auto const mid = hlsToRgb(90, 50, 0);
    CHECK(mid.red == mid.green);
    CHECK(mid.green == mid.blue);
}

TEST_CASE("ReGISColor.rgbPercentToRgb", "[regis][color]")
{
    CHECK(rgbPercentToRgb(0, 0, 0) == RGBColor { 0, 0, 0 });
    CHECK(rgbPercentToRgb(100, 100, 100) == RGBColor { 255, 255, 255 });
    CHECK(rgbPercentToRgb(100, 0, 0) == RGBColor { 255, 0, 0 });
    // Percentages beyond 100 saturate rather than overflow.
    CHECK(rgbPercentToRgb(200, 0, 0) == RGBColor { 255, 0, 0 });
}

TEST_CASE("ReGISColor.grayToRgb", "[regis][color]")
{
    CHECK(grayToRgb(0) == RGBColor { 0, 0, 0 });
    CHECK(grayToRgb(100) == RGBColor { 255, 255, 255 });
    auto const g = grayToRgb(50);
    CHECK(g.red == g.green);
    CHECK(g.green == g.blue);
}

TEST_CASE("ReGISColor.registers.defaultsAndSet", "[regis][color]")
{
    auto regs = ReGISColorRegisters {};
    CHECK(regs.count() == ColorRegisterCount);
    CHECK(regs.at(0) == RGBColor { 0, 0, 0 }); // register 0 is black on the VT340

    regs.set(3, RGBColor { 10, 20, 30 });
    CHECK(regs.at(3) == RGBColor { 10, 20, 30 });

    // Indices wrap modulo the register count, matching the VT340's fixed map.
    CHECK(regs.at(3 + ColorRegisterCount) == RGBColor { 10, 20, 30 });

    regs.reset();
    CHECK(regs.at(3) != RGBColor { 10, 20, 30 });
}

TEST_CASE("ReGISColor.registers.findClosest", "[regis][color]")
{
    auto regs = ReGISColorRegisters {};
    // Pure black is register 0 exactly; a near-black is still closest to it.
    CHECK(regs.findClosest(RGBColor { 0, 0, 0 }) == 0);
    CHECK(regs.findClosest(RGBColor { 3, 3, 3 }) == 0);
}
