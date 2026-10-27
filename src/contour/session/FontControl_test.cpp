// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/session/FontControl.hpp>

#include <vtbackend/screen/GutterSettings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>

using contour::session::configuredGutterExtent;
using contour::session::configuredGutterLayout;
using contour::session::configuredGutterWidth;
using contour::session::GutterChrome;
using contour::session::gutterChromeFor;
using contour::session::gutterWidthFor;

namespace
{
/// The chrome of the Terminal row, whose block column is today's glyph cells.
constexpr auto GlyphChrome = GutterChrome {};
} // namespace

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
    // The default: the block column alone, two cells wide.
    CHECK(configuredGutterLayout(config, GlyphChrome).totalColumns == 2);

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterLayout(config, GlyphChrome).totalColumns == 8);

    config.gutter.value().blockColumnWidth = 1;
    CHECK(configuredGutterLayout(config, GlyphChrome).totalColumns == 7);

    // No job left for the block column and no numbers: no strip, and no width taken from the page.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterLayout(config, GlyphChrome).totalColumns == 0);
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
    CHECK(configuredGutterWidth(config, cell, Scale, GlyphChrome) == leadIn + (2 * 9));

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterWidth(config, cell, Scale, GlyphChrome) == leadIn + (8 * 9));
    CHECK(configuredGutterWidth(config, cell, Scale, GlyphChrome)
          == leadIn + gutterWidthFor(configuredGutterLayout(config, GlyphChrome), cell));

    // No strip, and so no lead-in either: a terminal without a gutter keeps every pixel it had.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterWidth(config, cell, Scale, GlyphChrome) == 0);
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
    auto const columnsPx = gutterWidthFor(configuredGutterLayout(config, GlyphChrome), cell);

    auto const fit = contour::geometry::fitPageToPixels(
        vtbackend::ImageSize { .width = vtbackend::Width(2560), .height = vtbackend::Height(1600) },
        cell,
        contour::geometry::Margins {},
        std::identity {},
        configuredGutterWidth(config, cell, Scale, GlyphChrome));

    auto const lastPixelUnderZone =
        static_cast<int>(std::ceil(contour::geometry::ResizeBorderThickness * Scale)) - 1;
    CHECK(fit.pageMargin.left - columnsPx == lastPixelUnderZone + 1);

    // A press on the lead-in counts as one on the column. Where the zone lies over it, the zone takes the
    // press first; a framed window, or a pane away from the window's edge, has no zone there.
    auto const columnAt = [&](int xDevicePx) {
        return contour::geometry::gutterColumnAt(xDevicePx,
                                                 fit.pageMargin.left,
                                                 configuredGutterExtent(config, cell, Scale, GlyphChrome),
                                                 unbox<int>(cell.width));
    };
    CHECK(columnAt(0) == 0);
    CHECK(columnAt(lastPixelUnderZone + 1) == 0);
}

TEST_CASE("FontControl.configuredGutterExtent.splitsTheWidthIntoLeadInAndColumns",
          "[contour][geometry][gutter]")
{
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    auto constexpr Scale = 2.0;
    auto config = contour::config::Config {};

    auto const extent = configuredGutterExtent(config, cell, Scale, GlyphChrome);
    CHECK(extent.leadIn == contour::geometry::gutterLeadIn(Scale));
    CHECK(extent.columns == gutterWidthFor(configuredGutterLayout(config, GlyphChrome), cell));
    CHECK(extent.leadIn + extent.columns == configuredGutterWidth(config, cell, Scale, GlyphChrome));

    // No columns, no lead-in.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    auto const none = configuredGutterExtent(config, cell, Scale, GlyphChrome);
    CHECK(none.leadIn == 0);
    CHECK(none.columns == 0);
}

TEST_CASE("FontControl.gutterChromeFor.followsTheChromeRow", "[contour][geometry][gutter]")
{
    using contour::config::ChromeStyle;
    using vtbackend::BlockColumnForm;
    CHECK(gutterChromeFor(ChromeStyle::MacOS)
          == GutterChrome { .form = BlockColumnForm::Rail,
                            .stripPoints = 22,
                            .barPoints = 3,
                            .barInsetPoints = 8,
                            .barRadiusPoints = 2,
                            .markPoints = 13,
                            .barEndPoints = 2,
                            .foldChipHitPoints = 24,
                            .chips = contour::config::detail::NativeBlockChips });
    CHECK(gutterChromeFor(ChromeStyle::Terminal).form == BlockColumnForm::Glyphs);
    CHECK(gutterChromeFor(ChromeStyle::Terminal).chips == contour::config::BlockChipTokens {});
}

TEST_CASE("FontControl.configuredGutterWidth.reservesTheRailStripAfterTheCells",
          "[contour][geometry][gutter]")
{
    // Line numbers and timestamps on: under a native row the cells are the timestamps and the numbers, and
    // the block column is the strip after them, its points scaled to device pixels; under the Terminal row
    // the block column is cells, as it always was.
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    auto constexpr Scale = 2.0;
    auto config = contour::config::Config {};
    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Absolute;
    config.gutter.value().timestamps = true;
    auto const leadIn = contour::geometry::gutterLeadIn(Scale);
    auto const timestampAndNumberCells = 8 + 1 + 6; // "%H:%M:%S" and its gap, then six digits

    auto const mac = gutterChromeFor(contour::config::ChromeStyle::MacOS);
    CHECK(configuredGutterWidth(config, cell, Scale, mac) == leadIn + (timestampAndNumberCells * 9) + 44);
    auto const extent = configuredGutterExtent(config, cell, Scale, mac);
    CHECK(extent.columns == timestampAndNumberCells * 9);
    CHECK(extent.rail == 44);

    auto const terminal = gutterChromeFor(contour::config::ChromeStyle::Terminal);
    CHECK(configuredGutterWidth(config, cell, Scale, terminal)
          == leadIn + ((timestampAndNumberCells + 2) * 9)); // the default two-cell block column
    CHECK(configuredGutterExtent(config, cell, Scale, terminal).rail == 0);
}

TEST_CASE("FontControl.gutterRailMetricsFor.isTheStripThePageFitReserves", "[contour][geometry][gutter]")
{
    // The renderer's strip and the page fit's are one width, both rounded from the row's points.
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    auto constexpr Scale = 1.5;
    auto const config = contour::config::Config {};
    auto const mac = gutterChromeFor(contour::config::ChromeStyle::MacOS);
    auto const layout = contour::session::configuredGutterLayout(config, mac);
    auto const rail = contour::session::gutterRailMetricsFor(layout, mac, Scale);
    CHECK(rail
          == vtrasterizer::GutterRailMetrics { .strip = 33,
                                               .bar = 5,
                                               .barInset = 12,
                                               .radius = 3,
                                               .endInset = 3,
                                               .chips = contour::session::blockChipMetricsFor(mac, Scale) });
    CHECK(rail.strip == configuredGutterExtent(config, cell, Scale, mac).rail);

    // The Terminal row has no strip, so the renderer moves nothing.
    auto const terminal = gutterChromeFor(contour::config::ChromeStyle::Terminal);
    CHECK(contour::session::gutterRailMetricsFor(
              contour::session::configuredGutterLayout(config, terminal), terminal, Scale)
          == vtrasterizer::GutterRailMetrics {});
}

TEST_CASE("FontControl.blockChipMetricsFor.areTheCanvasPillsAtTheScale", "[contour][geometry][gutter]")
{
    // The canvas's pills in device pixels: the status pills padded 1 x 8 at radius 9, the fold chip 0 x 10,
    // 20 tall at radius 10 with a hairline ring; type sizes stay in points, as the grid's do.
    using vtbackend::PillStyle;
    auto const mac = gutterChromeFor(contour::config::ChromeStyle::MacOS);
    auto const metrics = contour::session::blockChipMetricsFor(mac, 2.0);
    auto const& failed = metrics.pills[static_cast<std::size_t>(PillStyle::Failed)];
    CHECK(
        failed
        == vtrasterizer::PillMetrics { .fontSize = 11, .weight = 600, .padX = 16, .padY = 2, .radius = 18 });
    auto const& fold = metrics.pills[static_cast<std::size_t>(PillStyle::Fold)];
    CHECK(fold
          == vtrasterizer::PillMetrics {
              .fontSize = 11, .weight = 400, .padX = 20, .padY = 0, .height = 40, .radius = 20, .ring = 1 });
    CHECK(metrics.pills[static_cast<std::size_t>(PillStyle::Success)]
          == vtrasterizer::PillMetrics { .fontSize = 11, .weight = 400 });
    CHECK(metrics.markEdge == 26);
    CHECK(metrics.markFontSize == 9);
    CHECK(metrics.markWeight == 700);
    CHECK(metrics.hoverRing == 2);

    // At 1x the 0.5-pt ring is still a pixel: a hairline never vanishes.
    CHECK(
        contour::session::blockChipMetricsFor(mac, 1.0).pills[static_cast<std::size_t>(PillStyle::Fold)].ring
        == 1);

    // The Terminal row draws its chips in cells: no pill metrics at all.
    auto const terminal = gutterChromeFor(contour::config::ChromeStyle::Terminal);
    CHECK(contour::session::gutterRailMetricsFor(
              contour::session::configuredGutterLayout(contour::config::Config {}, terminal), terminal, 2.0)
              .chips
          == vtrasterizer::BlockChipMetrics {});
}
