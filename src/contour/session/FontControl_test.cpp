// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/session/FontControl.hpp>

#include <vtbackend/screen/GutterSettings.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("FontControl.configuredGutterWidth.isTheConfiguredLayoutsWidth", "[contour][geometry][gutter]")
{
    // The one call every geometry site makes: the configured layout, at the cell's width.
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    auto config = contour::config::Config {};
    CHECK(configuredGutterWidth(config, cell) == 1 * 9);

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterWidth(config, cell) == 7 * 9);
    CHECK(configuredGutterWidth(config, cell) == gutterWidthFor(configuredGutterLayout(config), cell));

    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterWidth(config, cell) == 0);
}
