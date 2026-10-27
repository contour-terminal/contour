// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ColorContrastReadout.hpp>

#include <vtbackend/core/Color.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using contour::window::contrastReadoutOf;
using contour::window::ContrastVerdict;
using contour::window::formatContrast;
using vtbackend::RGBColor;

namespace
{
constexpr auto White = RGBColor { 0xff, 0xff, 0xff };
constexpr auto Black = RGBColor { 0x00, 0x00, 0x00 };
constexpr auto RatioTolerance = 0.01;
} // namespace

TEST_CASE("contrastReadoutOf: black on white is the widest ratio", "[contrastreadout]")
{
    auto const readout = contrastReadoutOf(Black, White);
    CHECK(readout.ratio == Catch::Approx(21.0).margin(RatioTolerance));
    CHECK(readout.verdict == ContrastVerdict::Passes);
    CHECK(formatContrast(readout) == "21.0 : 1");
}

TEST_CASE("contrastReadoutOf: the 4.5 : 1 rule splits #767676 from #777777 on white", "[contrastreadout]")
{
    auto const passing = contrastReadoutOf(RGBColor { 0x76, 0x76, 0x76 }, White);
    CHECK(passing.verdict == ContrastVerdict::Passes);
    CHECK(passing.ratio
          == Catch::Approx(vtbackend::contrastRatio(RGBColor { 0x76, 0x76, 0x76 }, White))
                 .margin(RatioTolerance));
    CHECK(formatContrast(passing) == "4.5 : 1");

    auto const failing = contrastReadoutOf(RGBColor { 0x77, 0x77, 0x77 }, White);
    CHECK(failing.verdict == ContrastVerdict::Fails);
    CHECK(failing.ratio
          == Catch::Approx(vtbackend::contrastRatio(RGBColor { 0x77, 0x77, 0x77 }, White))
                 .margin(RatioTolerance));
}

TEST_CASE("contrastReadoutOf: the rule can be asked for at another ratio", "[contrastreadout]")
{
    auto const grey = RGBColor { 0x77, 0x77, 0x77 };
    CHECK(contrastReadoutOf(grey, White).verdict == ContrastVerdict::Fails);
    CHECK(contrastReadoutOf(grey, White, 3.0).verdict == ContrastVerdict::Passes);
}

TEST_CASE("contrastReadoutOf: the order of the colours does not matter", "[contrastreadout]")
{
    auto const grey = RGBColor { 0x40, 0x60, 0x80 };
    CHECK(contrastReadoutOf(grey, White).ratio == Catch::Approx(contrastReadoutOf(White, grey).ratio));
}
