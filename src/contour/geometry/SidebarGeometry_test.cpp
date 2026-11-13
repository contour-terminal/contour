// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/geometry/SidebarGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::config::ChromeStyle;
using contour::config::ChromeTokenRows;
using contour::config::LengthUnit;
using contour::config::SidebarVisibility;
using contour::geometry::SidebarScale;

namespace
{
/// The Terminal row's sidebar tokens, in cells.
[[nodiscard]] contour::config::SidebarTokens terminalTokens()
{
    return ChromeTokenRows[static_cast<size_t>(ChromeStyle::Terminal)].sidebar;
}

/// A native row's sidebar tokens, in pixels.
[[nodiscard]] contour::config::SidebarTokens macTokens()
{
    return ChromeTokenRows[static_cast<size_t>(ChromeStyle::MacOS)].sidebar;
}

constexpr auto PixelScale = SidebarScale { .unit = LengthUnit::Pixel, .unitPixels = 1.0 };
constexpr auto CellScale = SidebarScale { .unit = LengthUnit::Cell, .unitPixels = 9.0 };
} // namespace

TEST_CASE("chrome width is the sidebar width when shown, 0 when hidden", "[geometry][sidebar]")
{
    auto const tokens = macTokens();
    CHECK(contour::geometry::sidebarWidthPixels(SidebarVisibility::Shown, tokens.width, tokens, PixelScale)
          == tokens.width);
    CHECK(contour::geometry::sidebarWidthPixels(SidebarVisibility::Hidden, tokens.width, tokens, PixelScale)
          == 0);
}

TEST_CASE("a sidebar width is kept within the row's bounds", "[geometry][sidebar]")
{
    auto const tokens = macTokens();
    CHECK(contour::geometry::sidebarUnitsOf(1, tokens, PixelScale) == tokens.minWidth);
    CHECK(contour::geometry::sidebarUnitsOf(100000, tokens, PixelScale) == tokens.maxWidth);
    CHECK(contour::geometry::sidebarUnitsOf(tokens.width + 7, tokens, PixelScale) == tokens.width + 7);
    CHECK(contour::geometry::sidebarWidthPixels(SidebarVisibility::Shown, 5, tokens, PixelScale)
          == tokens.minWidth);
}

TEST_CASE("terminal-row sidebar width snaps to whole cells", "[geometry][sidebar]")
{
    auto const tokens = terminalTokens();
    // 20 cells of 9 px is 180 px; 183 px and 176 px are the nearest drag ends to it.
    for (auto const pixels: { 176, 180, 183 })
    {
        auto const units = contour::geometry::sidebarUnitsOf(pixels, tokens, CellScale);
        CHECK(units == 20);
        CHECK(contour::geometry::sidebarWidthPixels(SidebarVisibility::Shown, units, tokens, CellScale)
              == 180);
    }
    // 190 px is nearer 21 cells (189 px) than 20.
    CHECK(contour::geometry::sidebarUnitsOf(190, tokens, CellScale) == 21);
    // Whatever the drag, the width is a whole multiple of a cell.
    for (auto const pixels: { 200, 233, 301, 399, 431 })
        CHECK(contour::geometry::sidebarWidthPixels(
                  SidebarVisibility::Shown,
                  contour::geometry::sidebarUnitsOf(pixels, tokens, CellScale),
                  tokens,
                  CellScale)
                  % 9
              == 0);
}
