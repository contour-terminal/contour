// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TitleBandGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <string_view>

using namespace contour::window;
using contour::config::ChromeStyle;
using contour::config::Material;

namespace
{
constexpr auto Mac = TitleBandTokens {
    .style = ChromeStyle::MacOS, .material = Material::Glass, .height = 38, .cellHeight = 0
};
constexpr auto Tui = TitleBandTokens {
    .style = ChromeStyle::Terminal, .material = Material::None, .height = 17, .cellHeight = 17
};
constexpr auto Flat34 = TitleBandTokens {
    .style = ChromeStyle::ContourLinux, .material = Material::Solid, .height = 34, .cellHeight = 0
};

[[nodiscard]] TitleBandGeometry geometryOf(
    TitleBandTokens tokens, WindowMode mode, double dpr, double nativeHeight, double fullScreenTop = 0)
{
    return resolveTitleBandGeometry(TitleBandGeometryInputs { .tokens = tokens,
                                                              .mode = mode,
                                                              .devicePixelRatio = dpr,
                                                              .nativeHeight = nativeHeight,
                                                              .fullScreenTop = fullScreenTop });
}
} // namespace

TEST_CASE("geometry.nativeRows", "[titlebandgeometry]")
{
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 28).height == 38);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 52).height == 52);
    // No native title bar: the token, as off macOS.
    CHECK(geometryOf(Mac, WindowMode::Windowed, 1, 0).height == 38);
}

TEST_CASE("geometry.devicePixelSnap", "[titlebandgeometry]")
{
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 38.25).height == 38.5);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 1, 38.25).height == 39);
    for (auto const dpr: { 1.0, 2.0 })
        for (auto const native: { 0.0, 27.5, 28.0, 32.25, 38.0, 38.25, 52.0 })
        {
            CAPTURE(dpr, native);
            auto const scaled = geometryOf(Mac, WindowMode::Windowed, dpr, native).height * dpr;
            CHECK(scaled == std::floor(scaled));
        }
    // Without a native title bar nothing is snapped, so a Flat band keeps its extent at a fractional scale.
    CHECK(geometryOf(Flat34, WindowMode::Windowed, 1.25, 0).height == 34);
}

TEST_CASE("geometry.revealTop", "[titlebandgeometry]")
{
    // Below AppKit's revealed strip, a window of its own that takes the clicks over it: its top plus the
    // native title bar's height.
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 28, 24).revealTop == 24 + 28);
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 40, 0).revealTop == 40);
    CHECK(geometryOf(Mac, WindowMode::Windowed, 2, 28, 24).revealTop == 0);
    // Window-relative, never above the window's own top.
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 28, -5).revealTop == 28);
    // No native title bar (Linux, Windows): nothing to rest below.
    CHECK(geometryOf(Mac, WindowMode::FullScreen, 2, 0, 0).revealTop == 0);
}

TEST_CASE("geometry.terminalRow", "[titlebandgeometry]")
{
    static_assert(TerminalRowTrafficLightFit == TrafficLightFit::GrowBandToCells);
    CHECK(geometryOf(Tui, WindowMode::Windowed, 2, 28) == TitleBandGeometry { .height = 34, .revealTop = 0 });
    CHECK(geometryOf(Tui, WindowMode::Windowed, 2, 0).height == 17);
}

TEST_CASE("geometry.bandMinimumWidth", "[titlebandgeometry]")
{
    struct Row
    {
        std::string_view name;
        BandWidths widths;
        double expected;
    };
    // The chip's last step is the same wide whatever the daemon is called: the daemon is gone by then.
    constexpr auto ChipAtLastStep = 120.0;
    constexpr auto Rows = std::array {
        // Traffic lights lead; the new-tab button trails.
        Row { .name = "macOS: traffic lights lead, the new-tab button trails",
              .widths = { .leading = 78,
                          .trailing = 44,
                          .chipMargins = 16,
                          .chip = ChipAtLastStep,
                          .dragReserve = 24,
                          .tabStripMargin = 4,
                          .tabWell = 48 },
              .expected = 78 + 44 + 16 + ChipAtLastStep + 24 + 4 + 48 },
        // Caption buttons trail.
        Row { .name = "Fluent: caption buttons trail",
              .widths = { .leading = 12,
                          .trailing = 190,
                          .chipMargins = 16,
                          .chip = ChipAtLastStep,
                          .dragReserve = 24,
                          .tabStripMargin = 4,
                          .tabWell = 48 },
              .expected = 12 + 190 + 16 + ChipAtLastStep + 24 + 4 + 48 },
        // The header bar: window buttons at the end, the menu and the toggle at the start.
        Row { .name = "ContourLinux: the header bar",
              .widths = { .leading = 96,
                          .trailing = 150,
                          .chipMargins = 16,
                          .chip = ChipAtLastStep,
                          .dragReserve = 24,
                          .tabStripMargin = 4,
                          .tabWell = 48 },
              .expected = 96 + 150 + 16 + ChipAtLastStep + 24 + 4 + 48 },
        // Whole cells.
        Row { .name = "Terminal: whole cells",
              .widths = { .leading = 16,
                          .trailing = 64,
                          .chipMargins = 16,
                          .chip = 96,
                          .dragReserve = 16,
                          .tabStripMargin = 8,
                          .tabWell = 32 },
              .expected = 16 + 64 + 16 + 96 + 16 + 8 + 32 },
        // A long daemon name changes nothing the minimum counts (the chip is at its last step); a shown
        // drift chip widens the trailing part.
        Row { .name = "ContourLinux: a long daemon name changes nothing, a shown drift chip widens the "
                      "trailing part",
              .widths = { .leading = 96,
                          .trailing = 150 + 90,
                          .chipMargins = 16,
                          .chip = ChipAtLastStep,
                          .dragReserve = 24,
                          .tabStripMargin = 4,
                          .tabWell = 48 },
              .expected = 96 + 150 + 90 + 16 + ChipAtLastStep + 24 + 4 + 48 },
        // No chip (a window without a daemon, or the sidebar shown), no tabs in the band.
        Row { .name = "ContourLinux: no chip, no tabs in the band",
              .widths = { .leading = 96,
                          .trailing = 150,
                          .chipMargins = 0,
                          .chip = 0,
                          .dragReserve = 24,
                          .tabStripMargin = 4,
                          .tabWell = 0 },
              .expected = 96 + 150 + 24 + 4 },
        // A part that measured negative counts as nothing.
        Row { .name = "a negative part counts as nothing",
              .widths = { .leading = -5, .trailing = 40, .dragReserve = 10 },
              .expected = 50 },
    };
    for (auto const& row: Rows)
    {
        INFO(row.name);
        CHECK(bandMinimumWidth(row.widths) == row.expected);
    }
}

TEST_CASE("geometry.bandMinimumWidthFrom", "[titlebandgeometry]")
{
    // Every part has a key, and a part the band does not name counts as 0.
    CHECK(bandMinimumWidthFrom([](std::string_view) { return 1.0; })
          == static_cast<double>(BandWidthParts.size()));
    CHECK(bandMinimumWidthFrom([](std::string_view key) { return key == "tabWell" ? 48.0 : 0.0; }) == 48);
    CHECK(bandMinimumWidthFrom([](std::string_view) { return 0.0; }) == 0);
}

TEST_CASE("geometry.minimumWidthWith", "[titlebandgeometry]")
{
    struct Row
    {
        std::string_view name;
        int hintMinimum;
        double bandMinimum;
        int sidebarWidth;
        BandPlacement placement;
        int expected;
    };
    constexpr auto Rows = std::array {
        Row { .name = "the hint stands where it is the wider",
              .hintMinimum = 400,
              .bandMinimum = 300,
              .sidebarWidth = 0,
              .placement = BandPlacement::FullWidth,
              .expected = 400 },
        Row { .name = "the band stands where it is the wider",
              .hintMinimum = 200,
              .bandMinimum = 333,
              .sidebarWidth = 0,
              .placement = BandPlacement::FullWidth,
              .expected = 333 },
        Row { .name = "a fraction rounds up, never down",
              .hintMinimum = 200,
              .bandMinimum = 333.2,
              .sidebarWidth = 0,
              .placement = BandPlacement::FullWidth,
              .expected = 334 },
        Row { .name = "a whole number is not rounded up",
              .hintMinimum = 200,
              .bandMinimum = 333.0,
              .sidebarWidth = 0,
              .placement = BandPlacement::FullWidth,
              .expected = 333 },
        Row { .name = "a band beside the sidebar adds the sidebar",
              .hintMinimum = 200,
              .bandMinimum = 333.2,
              .sidebarWidth = 180,
              .placement = BandPlacement::BesideSidebar,
              .expected = 514 },
        Row { .name = "a band over the sidebar does not",
              .hintMinimum = 200,
              .bandMinimum = 333.2,
              .sidebarWidth = 180,
              .placement = BandPlacement::FullWidth,
              .expected = 334 },
        Row { .name = "the hint wins over a band beside the sidebar",
              .hintMinimum = 900,
              .bandMinimum = 333.2,
              .sidebarWidth = 180,
              .placement = BandPlacement::BesideSidebar,
              .expected = 900 },
        Row { .name = "no band parts: the hint",
              .hintMinimum = 200,
              .bandMinimum = 0,
              .sidebarWidth = 180,
              .placement = BandPlacement::BesideSidebar,
              .expected = 200 },
    };
    for (auto const& row: Rows)
    {
        INFO(row.name);
        CHECK(minimumWidthWith(row.hintMinimum, row.bandMinimum, row.sidebarWidth, row.placement)
              == row.expected);
    }
}

TEST_CASE("geometry.bandPlacementFromInt", "[titlebandgeometry]")
{
    CHECK(bandPlacementFromInt(0) == BandPlacement::FullWidth);
    CHECK(bandPlacementFromInt(1) == BandPlacement::BesideSidebar);
    CHECK(bandPlacementFromInt(static_cast<int>(BandPlacementCount)) == std::nullopt);
    CHECK(bandPlacementFromInt(-1) == std::nullopt);
}
