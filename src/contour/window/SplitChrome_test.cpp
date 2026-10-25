// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SplitChrome.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::config::AccessibilityPreferences;
using contour::config::ChromeDensity;
using contour::config::ChromeStyle;
using contour::config::resolveChromeTokens;
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
        CHECK(splitChromeOf(tokens, row.unitWidth, row.unitHeight) == row.expected);
    }
}

TEST_CASE("splitChromeOf never undershoots a fractional cell", "[splitchrome]")
{
    // A 7.4 x 16.2 cell: one cell of gap is 8 wide and 17 high, never 7 and 16, so the solver it feeds
    // stays an upper bound of what the canvas draws.
    auto const tokens = resolveChromeTokens(ChromeStyle::Terminal, AccessibilityPreferences {});
    CHECK(splitChromeOf(tokens, 7.4, 16.2)
          == SplitChrome { .sideBySideGap = 8, .stackedGap = 17, .leafHeader = 17, .rootInset = 0 });
}

static_assert(
    splitChromeOf(resolveChromeTokens(ChromeStyle::MacOS, AccessibilityPreferences {}), 1.0, 1.0).leafHeader
    == 22);
