// SPDX-License-Identifier: Apache-2.0
//
// The pixel helpers the display-gated in-grid visual tests share: a harness per (scheme, chrome row), colour
// sampling over a grabbed frame, and the cell geometry the display itself names. @see
// CommandBlockRendering_test.cpp and InGridVisualsRendering_test.cpp.
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/DisplayHarness.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <vtrasterizer/GridMetrics.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointF>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define REQUIRE_DISPLAY_OR_SKIP()                                                          \
    do                                                                                     \
    {                                                                                      \
        if (!contour::test::displayTestsEnabled(core::LiveEnvironment {}))                 \
        {                                                                                  \
            WARN("Skipping display test: set CONTOUR_TEST_DISPLAY=1 with a real display"); \
            return;                                                                        \
        }                                                                                  \
    } while (0)

namespace contour::test::pixel
{

using contour::test::DisplayHarness;
using contour::test::HarnessWindow;
using namespace std::string_view_literals;

/// The WCAG ratio text must reach against what it sits on: the terminal's own, so the test and the product
/// cannot disagree about what "legible" means.
constexpr auto TextContrast = vtbackend::MinimumTextContrast;

/// The WCAG ratio a mark (a rail, a disc, a glyph) must reach against what it sits on.
constexpr auto MarkContrast = vtbackend::MinimumMarkContrast;

/// The bundled colour schemes the cases loop: a dark one and two light ones, one of them the cream
/// solarized-light whose status inks are the hardest to read.
constexpr auto SchemeNames = std::array { "contour-dark"sv, "solarized-light"sv, "gruvbox-light"sv };

/// The chrome rows the cases loop: the native row, whose rail and chips the renderer draws as pixels, and
/// the Terminal row, whose block column is glyph cells and whose chips are text over a band.
constexpr auto ChromeRows =
    std::array { contour::config::UiStyle::Native, contour::config::UiStyle::Terminal };

/// One scheme on one chrome row.
struct Variant
{
    std::string_view scheme;        ///< A bundled scheme's name.
    contour::config::UiStyle style; ///< The chrome row.
};

/// What the failure message names the variant by.
[[nodiscard]] inline std::string nameOf(Variant const& variant)
{
    return std::format("{} on the {} row",
                       variant.scheme,
                       variant.style == contour::config::UiStyle::Terminal ? "Terminal" : "native");
}

/// Every scheme on every chrome row.
[[nodiscard]] inline std::vector<Variant> allVariants()
{
    auto variants = std::vector<Variant> {};
    for (auto const scheme: SchemeNames)
        for (auto const style: ChromeRows)
            variants.push_back(Variant { .scheme = scheme, .style = style });
    return variants;
}

/// The harness of @p variant: its scheme, its chrome row, and the block column's exit-status colouring on.
/// @param variant The scheme and chrome row.
/// @param adjust  Adjusts the configuration after the variant has been applied, for what a case is about.
/// @param seed    Fills the session's terminal before the display's first frame (@see DisplayHarness).
[[nodiscard]] inline std::unique_ptr<DisplayHarness> makeHarness(
    Variant const& variant,
    std::function<void(contour::config::Config&)> const& adjust = {},
    std::function<void(contour::session::TerminalSession&)> const& seed = {})
{
    auto harness = std::make_unique<DisplayHarness>(
        HarnessWindow::Shown,
        [&](contour::config::Config& config) {
            config.uiStyle = variant.style;
            auto& profile = config.profiles.value().at("main");
            auto colors =
                contour::config::SimpleColorConfig { .colorScheme = std::string { variant.scheme } };
            REQUIRE(vtbackend::defaultColorPalettes(variant.scheme, colors.colors));
            profile.colors.value() = colors;
            if (adjust)
                adjust(config);
        },
        seed);
    // The page's own colour is painted beneath the terminal (the application window's QML), which the
    // harness's bare window stands in for: black would put a light scheme's ink on a ground it never has.
    auto const background = harness->session->terminal().colorPalette().defaultBackground;
    harness->window->setColor(QColor(background.red, background.green, background.blue));
    return harness;
}

[[nodiscard]] inline vtbackend::RGBColor rgbOf(QRgb pixel) noexcept
{
    return vtbackend::RGBColor { static_cast<uint8_t>(qRed(pixel)),
                                 static_cast<uint8_t>(qGreen(pixel)),
                                 static_cast<uint8_t>(qBlue(pixel)) };
}

/// The median colour of the @p radius pixels around @p centre: robust against the anti-aliasing at a shape's
/// edge, which a single pixel is not.
[[nodiscard]] inline vtbackend::RGBColor medianAround(QImage const& frame, QPoint centre, int radius = 1)
{
    auto reds = std::vector<uint8_t> {};
    auto greens = std::vector<uint8_t> {};
    auto blues = std::vector<uint8_t> {};
    for (auto const y: std::views::iota(centre.y() - radius, centre.y() + radius + 1))
        for (auto const x: std::views::iota(centre.x() - radius, centre.x() + radius + 1))
            if (frame.rect().contains(x, y))
            {
                auto const pixel = frame.pixel(x, y);
                reds.push_back(static_cast<uint8_t>(qRed(pixel)));
                greens.push_back(static_cast<uint8_t>(qGreen(pixel)));
                blues.push_back(static_cast<uint8_t>(qBlue(pixel)));
            }
    REQUIRE_FALSE(reds.empty());
    auto const middle = [](std::vector<uint8_t>& values) {
        std::ranges::sort(values);
        return values[values.size() / 2];
    };
    return vtbackend::RGBColor { middle(reds), middle(greens), middle(blues) };
}

/// Whether two colours are the same to within @p tolerance on each channel: what sampled, anti-aliased
/// pixels can promise about a colour the renderer was handed.
[[nodiscard]] inline bool nearlyEqual(vtbackend::RGBColor a,
                                      vtbackend::RGBColor b,
                                      int tolerance = 6) noexcept
{
    return std::abs(int(a.red) - int(b.red)) <= tolerance
           && std::abs(int(a.green) - int(b.green)) <= tolerance
           && std::abs(int(a.blue) - int(b.blue)) <= tolerance;
}

[[nodiscard]] inline std::string hexOf(vtbackend::RGBColor color)
{
    return std::format("#{:02x}{:02x}{:02x}", color.red, color.green, color.blue);
}

/// How far apart two colours are, in the sum of the channels' differences: for finding the pixel that stands
/// out most from a ground.
[[nodiscard]] inline int distanceBetween(vtbackend::RGBColor a, vtbackend::RGBColor b) noexcept
{
    return std::abs(int(a.red) - int(b.red)) + std::abs(int(a.green) - int(b.green))
           + std::abs(int(a.blue) - int(b.blue));
}

/// What the pixels of a region are made of: the colour most of them have (the ground the region's ink sits
/// on), and the colour farthest from it (the ink at its strongest, where a stem fills a whole pixel).
struct RegionColors
{
    vtbackend::RGBColor ground {}; ///< The most common colour.
    vtbackend::RGBColor ink {};    ///< The colour farthest from the ground.
    int groundPixels = 0;          ///< How many pixels have the ground colour.
    int pixels = 0;                ///< How many pixels the region has.
};

[[nodiscard]] inline RegionColors regionColors(QImage const& frame, QRect const& region)
{
    auto const area = region.intersected(frame.rect());
    auto counts = std::map<QRgb, int> {};
    for (auto const y: std::views::iota(area.top(), area.bottom() + 1))
        for (auto const x: std::views::iota(area.left(), area.right() + 1))
            ++counts[frame.pixel(x, y)];
    REQUIRE_FALSE(counts.empty());

    auto const common = std::ranges::max_element(counts, {}, &std::pair<QRgb const, int>::second);
    auto result = RegionColors { .ground = rgbOf(common->first),
                                 .ink = rgbOf(common->first),
                                 .groundPixels = common->second,
                                 .pixels = area.width() * area.height() };
    auto farthest = 0;
    for (auto const& [pixel, count]: counts)
    {
        (void) count;
        auto const distance = distanceBetween(rgbOf(pixel), result.ground);
        if (distance > farthest)
        {
            farthest = distance;
            result.ink = rgbOf(pixel);
        }
    }
    return result;
}

/// A frame of the harness with the render buffer the terminal would hand the renderer for it.
struct Frame
{
    QImage image;                   ///< What the window showed.
    vtbackend::RenderBuffer buffer; ///< What the terminal drew it from.
};

/// The pixels of a row's cells, left of column 0 negative.
struct CellGeometry
{
    vtrasterizer::GridMetrics metrics; ///< The renderer's metrics, in device pixels.
    int cellWidth = 0;                 ///< A cell's width in device pixels.
    int cellHeight = 0;                ///< A cell's height in device pixels.
    int pageColumns = 0;               ///< The page's width in columns.

    /// @param row A screen row.
    /// @return Its top edge.
    [[nodiscard]] int rowTop(int row) const noexcept { return metrics.pageMargin.top + (row * cellHeight); }

    /// @param column A screen column.
    /// @return Its left edge.
    [[nodiscard]] int columnLeft(int column) const noexcept
    {
        return metrics.pageMargin.left + (column * cellWidth);
    }

    /// @param row A screen row.
    /// @param from The first column.
    /// @param to The last column, included.
    /// @return The pixels of those columns of that row.
    [[nodiscard]] QRect cells(int row, int from, int to) const noexcept
    {
        return QRect { columnLeft(from), rowTop(row), (to - from + 1) * cellWidth, cellHeight };
    }
};

[[nodiscard]] inline CellGeometry geometryOf(DisplayHarness const& h)
{
    auto const metrics = h.display->gridMetrics();
    return CellGeometry { .metrics = metrics,
                          .cellWidth = unbox<int>(metrics.cellSize.width),
                          .cellHeight = unbox<int>(metrics.cellSize.height),
                          .pageColumns = unbox<int>(h.session->terminal().pageSize().columns) };
}

/// Grabs a frame and the buffer behind it, once the terminal has settled.
[[nodiscard]] inline Frame frameOf(DisplayHarness const& h)
{
    auto frame = Frame { .image = h.pump(), .buffer = {} };
    REQUIRE_FALSE(frame.image.isNull());
    // What the terminal published for the frame just grabbed: the buffer the renderer drew it from.
    frame.buffer = h.session->terminal().renderBuffer().get();
    return frame;
}

/// Sends a mouse move to the display at item-local logical position @p pos, as the windowing system would.
inline void moveMouse(contour::display::TerminalDisplay* display, QPointF pos)
{
    QMouseEvent move(
        QEvent::MouseMove, pos, display->mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(display, &move);
}

/// The item-local logical position of the middle of the cell at (@p row, @p column).
[[nodiscard]] inline QPointF cellCentre(DisplayHarness const& h, int row, int column)
{
    auto const g = geometryOf(h);
    auto const dpr = h.display->devicePixelRatio();
    return { (g.columnLeft(column) + (g.cellWidth / 2.0)) / dpr,
             (g.rowTop(row) + (g.cellHeight / 2.0)) / dpr };
}

/// The part of the frame that holds the grid, which excludes the status line beneath it.
[[nodiscard]] inline QRect pageAreaOf(DisplayHarness const& h, QImage const& frame)
{
    auto const g = geometryOf(h);
    return QRect { 0, 0, frame.width(), g.rowTop(unbox<int>(h.session->terminal().pageSize().lines)) };
}

/// The bounding box of every pixel inside @p region that differs between @p before and @p after.
[[nodiscard]] inline QRect changedBounds(QImage const& before, QImage const& after, QRect const& region)
{
    REQUIRE(before.size() == after.size());
    auto const area = region.intersected(before.rect());
    auto bounds = QRect {};
    for (auto const y: std::views::iota(area.top(), area.bottom() + 1))
        for (auto const x: std::views::iota(area.left(), area.right() + 1))
            if (before.pixel(x, y) != after.pixel(x, y))
                bounds = bounds.isNull() ? QRect { x, y, 1, 1 } : bounds.united(QRect { x, y, 1, 1 });
    return bounds;
}

} // namespace contour::test::pixel
