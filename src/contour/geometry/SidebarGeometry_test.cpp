// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/geometry/SidebarGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

TEST_CASE("a sidebar title row needs its heading, its controls and their gaps", "[geometry][sidebar]")
{
    auto const row = contour::geometry::SidebarTitleRow {
        .lead = 12, .heading = 80, .control = 28, .controlGap = 6, .controls = 3, .trail = 9
    };
    // 12 + 80 + 3 * 28 + 2 * 6 + 9.
    CHECK(contour::geometry::sidebarTitleRowWidth(row) == 197);
    // No controls, no gaps between them.
    CHECK(contour::geometry::sidebarTitleRowWidth({ .lead = 12, .heading = 80, .controlGap = 6, .trail = 9 })
          == 101);
}

TEST_CASE("a sidebar's default width widens to hold its title row", "[geometry][sidebar]")
{
    auto const tokens = macTokens();
    // A title row the row's width holds keeps its default; none at all (no heading) does too.
    CHECK(contour::geometry::sidebarDefaultUnits(tokens, PixelScale, tokens.width - 1.0) == tokens.width);
    CHECK(contour::geometry::sidebarDefaultUnits(tokens, PixelScale, 0.0) == tokens.width);
    // A wider one widens it to the next whole unit, never past the widest it may be.
    CHECK(contour::geometry::sidebarDefaultUnits(tokens, PixelScale, tokens.width + 0.5) == tokens.width + 1);
    CHECK(contour::geometry::sidebarDefaultUnits(tokens, PixelScale, tokens.maxWidth * 2.0)
          == tokens.maxWidth);

    // In cells: 300 px over cells of 9 px is 33.3 cells, so 34.
    auto const cells = terminalTokens();
    CHECK(contour::geometry::sidebarDefaultUnits(cells, CellScale, 300.0) == std::max(cells.width, 34));
    CHECK(contour::geometry::sidebarDefaultUnits(cells, CellScale, 9.0) == cells.width);
}
