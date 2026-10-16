// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/session/FontControl.hpp>

#include <vtbackend/screen/GutterSettings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>

using contour::session::configuredGutterLayout;
using contour::session::configuredGutterWidth;
using contour::session::gutterWidthFor;

TEST_CASE("FontControl.gutterWidthFor.reservesEveryColumnOfTheLayout", "[contour][geometry][gutter]")
{
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    CHECK(gutterWidthFor(vtbackend::GutterLayout {}, cell) == 0);

    auto const layout = vtbackend::gutterLayoutFor(vtbackend::GutterSettings {
        .foldMarkers = true, .lineNumbers = vtbackend::LineNumberMode::Absolute, .lineNumberWidth = 4 });
    REQUIRE(layout.totalColumns == 5);
    CHECK(gutterWidthFor(layout, cell) == 5 * 9);
}

TEST_CASE("FontControl.configuredGutterLayout.followsTheConfiguration", "[contour][geometry][gutter]")
{
    auto config = contour::config::Config {};
    // The default: one column, exactly as the fold markers always took.
    CHECK(configuredGutterLayout(config).totalColumns == 1);

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterLayout(config).totalColumns == 7);

    // No job left for the block column and no numbers: no strip, and no width taken from the page.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterLayout(config).totalColumns == 0);
}

TEST_CASE("FontControl.configuredGutterWidth.isTheConfiguredLayoutsWidthBehindItsLeadIn",
          "[contour][geometry][gutter]")
{
    // The one call every geometry site makes: the configured layout, at the cell's width, and the blank
    // lead-in that keeps it off the window's edge.
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    auto constexpr Scale = 2.0;
    auto const leadIn = contour::geometry::gutterLeadIn(Scale);
    auto config = contour::config::Config {};
    CHECK(configuredGutterWidth(config, cell, Scale) == leadIn + (1 * 9));

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterWidth(config, cell, Scale) == leadIn + (7 * 9));
    CHECK(configuredGutterWidth(config, cell, Scale)
          == leadIn + gutterWidthFor(configuredGutterLayout(config), cell));

    // No strip, and so no lead-in either: a terminal without a gutter keeps every pixel it had.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterWidth(config, cell, Scale) == 0);
}

TEST_CASE("FontControl.configuredGutterWidth.keepsTheBlockColumnClearOfTheResizeBorder",
          "[contour][geometry][gutter]")
{
    // A frameless window lays an invisible resize zone over its left edge, above everything. With no
    // margin the gutter began at the window's edge and its one-cell block column was drawn under that zone:
    // at 1.75x the zone reaches 10.5 device pixels in, and a 17-pixel cell centres the fold line 8.5 in, so
    // pointing at it showed a resize cursor and pressing it resized the window. Fitted the way every page
    // is, and hit-tested the way every pointer is, the block column now starts at the zone's first whole
    // pixel past it, and not a pixel later.
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(17), .height = vtbackend::Height(34) };
    auto constexpr Scale = 1.75;
    auto const config = contour::config::Config {}; // the default gutter: the block column alone
    auto const columnsPx = gutterWidthFor(configuredGutterLayout(config), cell);

    auto const fit = contour::geometry::fitPageToPixels(
        vtbackend::ImageSize { .width = vtbackend::Width(2560), .height = vtbackend::Height(1600) },
        cell,
        contour::geometry::Margins {},
        std::identity {},
        configuredGutterWidth(config, cell, Scale));

    auto const columnAt = [&](int xDevicePx) {
        return contour::geometry::gutterColumnAt(
            xDevicePx, fit.pageMargin.left, columnsPx, unbox<int>(cell.width));
    };
    auto const lastPixelUnderZone =
        static_cast<int>(std::ceil(contour::geometry::ResizeBorderThickness * Scale)) - 1;
    CHECK_FALSE(columnAt(lastPixelUnderZone).has_value());
    CHECK(columnAt(lastPixelUnderZone + 1) == 0);
}
