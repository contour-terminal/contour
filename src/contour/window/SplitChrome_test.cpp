// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SplitChrome.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::config::AccessibilityPreferences;
using contour::config::ChromeDensity;
using contour::config::ChromeStyle;
using contour::config::resolveChromeTokens;
using contour::window::snappedChromePixels;
using contour::window::splitChromeOf;
using vtworkspace::SplitChrome;

namespace
{
/// One row's split chrome at one density and one cell size.
struct SplitChromeCase
{
    ChromeStyle style {};
    ChromeDensity density {};
    double unitWidth = 1.0;
    double unitHeight = 1.0;
    SplitChrome expected {};
};

// The native rows count in points, so their unit is 1; the Terminal row counts in cells.
constexpr auto SplitChromeCases = std::array {
    SplitChromeCase { .style = ChromeStyle::MacOS,
                      .density = ChromeDensity::Comfortable,
                      .unitWidth = 1.0,
                      .unitHeight = 1.0,
                      .expected = { .sideBySideGap = 6, .stackedGap = 6, .leafHeader = 22, .rootInset = 6 } },
    SplitChromeCase { .style = ChromeStyle::MacOS,
                      .density = ChromeDensity::Minimal,
                      .unitWidth = 1.0,
                      .unitHeight = 1.0,
                      .expected = { .sideBySideGap = 1, .stackedGap = 1, .leafHeader = 0, .rootInset = 0 } },
    SplitChromeCase { .style = ChromeStyle::Fluent,
                      .density = ChromeDensity::Comfortable,
                      .unitWidth = 1.0,
                      .unitHeight = 1.0,
                      .expected = { .sideBySideGap = 4, .stackedGap = 4, .leafHeader = 28, .rootInset = 4 } },
    SplitChromeCase {
        .style = ChromeStyle::Terminal,
        .density = ChromeDensity::Comfortable,
        .unitWidth = 8.0,
        .unitHeight = 17.0,
        .expected = { .sideBySideGap = 8, .stackedGap = 17, .leafHeader = 17, .rootInset = 0 } },
};
} // namespace

TEST_CASE("splitChromeOf scales each token by its axis's unit", "[splitchrome]")
{
    for (auto const& row: SplitChromeCases)
    {
        CAPTURE(static_cast<int>(row.style), static_cast<int>(row.density));
        auto const tokens = resolveChromeTokens(row.style, AccessibilityPreferences {}, row.density);
        CHECK(splitChromeOf(tokens, row.unitWidth, row.unitHeight, 1.0) == row.expected);
    }
}

TEST_CASE("snappedChromePixels floors to the device grid and keeps one device pixel", "[splitchrome]")
{
    // 6 pt at 125 % is 7.5 device pixels: 7 of them are drawn, 5.6 pt -- never the 6.4 rounding would give.
    CHECK(snappedChromePixels(6.0, 1.25) == 7.0 / 1.25);
    CHECK(snappedChromePixels(6.0, 1.75) == 10.0 / 1.75);
    CHECK(snappedChromePixels(6.0, 1.5) == 6.0); // an exact product stays whole
    CHECK(snappedChromePixels(1.0, 1.75) == 1.0 / 1.75);
    CHECK(snappedChromePixels(0.4, 1.0) == 1.0);  // a gap is never floored away
    CHECK(snappedChromePixels(0.0, 1.25) == 0.0); // and no inset stays none
}

TEST_CASE("splitChromeOf counts the gaps and the inset as drawn on the device grid", "[splitchrome]")
{
    // A 7.4 x 16.2 cell on the Terminal row. At 100 % the canvas draws 7 x 16 device pixels, so the solver
    // counts 7 and 16 -- not 8 and 17, which would grow the window past the grid it asked for. At 125 %
    // the canvas draws 9 / 1.25 = 7.2 and 20 / 1.25 = 16 points, counted as 8 and 16. The header is not
    // drawn by a split yet, so it is the token rounded up either way.
    auto const tokens = resolveChromeTokens(ChromeStyle::Terminal, AccessibilityPreferences {});
    CHECK(splitChromeOf(tokens, 7.4, 16.2, 1.0)
          == SplitChrome { .sideBySideGap = 7, .stackedGap = 16, .leafHeader = 17, .rootInset = 0 });
    CHECK(splitChromeOf(tokens, 7.4, 16.2, 1.25)
          == SplitChrome { .sideBySideGap = 8, .stackedGap = 16, .leafHeader = 17, .rootInset = 0 });

    // A native row's whole-point tokens: whatever fraction a scale floors off, the count is the token.
    auto const mac = resolveChromeTokens(ChromeStyle::MacOS, AccessibilityPreferences {});
    for (auto const dpr: { 1.0, 1.25, 1.5, 1.75, 2.0 })
    {
        CAPTURE(dpr);
        CHECK(splitChromeOf(mac, 1.0, 1.0, dpr) == splitChromeOf(mac, 1.0, 1.0, 1.0));
    }
}

static_assert(
    splitChromeOf(resolveChromeTokens(ChromeStyle::MacOS, AccessibilityPreferences {}), 1.0, 1.0, 1.0)
        .leafHeader
    == 22);
