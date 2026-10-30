// SPDX-License-Identifier: Apache-2.0
#include <contour/config/HighlightPair.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using contour::config::HighlightPair;
using contour::config::legibleHighlight;
using vtbackend::contrastRatio;
using vtbackend::MinimumTextContrast;
using vtbackend::RGBColor;

namespace
{
constexpr auto White = RGBColor { 0xFF, 0xFF, 0xFF };
constexpr auto Black = RGBColor { 0x00, 0x00, 0x00 };
/// How far the hue may move, in degrees: the 8-bit grid and the gamut clamp allow a little.
constexpr auto HueToleranceDegrees = 4.0;

/// The HSL hue of @p color in degrees; 0 for a grey.
double hueOf(RGBColor color)
{
    auto const r = color.red / 255.0;
    auto const g = color.green / 255.0;
    auto const b = color.blue / 255.0;
    auto const hi = std::max({ r, g, b });
    auto const delta = hi - std::min({ r, g, b });
    if (delta == 0.0)
        return 0.0;
    auto hue = 0.0;
    if (hi == r)
        hue = std::fmod((g - b) / delta, 6.0);
    else if (hi == g)
        hue = ((b - r) / delta) + 2.0;
    else
        hue = ((r - g) / delta) + 4.0;
    return std::fmod((hue * 60.0) + 360.0, 360.0);
}

double hueGap(RGBColor a, RGBColor b)
{
    auto const gap = std::fabs(hueOf(a) - hueOf(b));
    return std::min(gap, 360.0 - gap);
}

double ratioOf(HighlightPair const& pair)
{
    return contrastRatio(pair.text, pair.fill);
}
} // namespace

TEST_CASE("legibleHighlight: Breeze's blue keeps its hue and reaches 4.5:1", "[chrometokens][highlight]")
{
    auto const breeze = HighlightPair { .fill = RGBColor { 0x3D, 0xAE, 0xE9 }, .text = White };
    REQUIRE(ratioOf(breeze) < MinimumTextContrast);
    auto const chosen = legibleHighlight(breeze);
    CHECK(ratioOf(chosen) >= MinimumTextContrast);
    CHECK(hueGap(chosen.fill, breeze.fill) <= HueToleranceDegrees);
    CHECK(chosen.text == Black);
}

TEST_CASE("legibleHighlight: a pair that already reads is returned unchanged", "[chrometokens][highlight]")
{
    auto const passing = std::array {
        HighlightPair { .fill = RGBColor { 0x00, 0x00, 0x80 }, .text = White },
        HighlightPair { .fill = RGBColor { 0xFF, 0xFF, 0x00 }, .text = Black },
        HighlightPair { .fill = Black, .text = White },
    };
    for (auto const& pair: passing)
    {
        REQUIRE(ratioOf(pair) >= MinimumTextContrast);
        CHECK(legibleHighlight(pair) == pair);
    }
}

TEST_CASE("legibleHighlight: the ink that reads better wins", "[chrometokens][highlight]")
{
    // A light hue under white: black.
    auto const light = legibleHighlight({ .fill = RGBColor { 0xA0, 0xC0, 0xF0 }, .text = White });
    CHECK(light.text == Black);
    // A dark hue under black: white.
    auto const dark = legibleHighlight({ .fill = RGBColor { 0x40, 0x40, 0x90 }, .text = Black });
    CHECK(dark.text == White);
    CHECK(ratioOf(dark) >= MinimumTextContrast);
}

TEST_CASE("legibleHighlight: extreme inputs read", "[chrometokens][highlight]")
{
    auto const fills = std::array { White,
                                    Black,
                                    RGBColor { 0xFF, 0xFF, 0x00 },
                                    RGBColor { 0xFF, 0x00, 0x00 },
                                    RGBColor { 0x00, 0xFF, 0x00 },
                                    RGBColor { 0x00, 0x00, 0xFF },
                                    RGBColor { 0x80, 0x80, 0x80 } };
    for (auto const fill: fills)
        for (auto const text: { White, Black })
        {
            INFO("fill " << int(fill.red) << "," << int(fill.green) << "," << int(fill.blue));
            auto const chosen = legibleHighlight({ .fill = fill, .text = text });
            CHECK(ratioOf(chosen) >= MinimumTextContrast);
        }
    // Saturated yellow under white keeps its hue with black text.
    auto const yellow = legibleHighlight({ .fill = RGBColor { 0xFF, 0xFF, 0x00 }, .text = White });
    CHECK(hueGap(yellow.fill, RGBColor { 0xFF, 0xFF, 0x00 }) <= HueToleranceDegrees);
}
