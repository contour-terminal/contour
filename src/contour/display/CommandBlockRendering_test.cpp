// SPDX-License-Identifier: Apache-2.0
//
// Display-gated pixel tests for the in-grid visuals of the command blocks: the status rail, the block chips
// and their pills, the hovered block's tint and ring, and the sticky band drawn over the first row. A REAL
// TerminalDisplay bound to a REAL session is driven through the RHI loop, and what these cases measure is the
// grabbed frame: colours sampled from the pixels at the places the display's own geometry names, never at
// hard-coded offsets. The chrome snapshots stub the terminal and the render-command tests never see a pixel,
// so this is where a rail that was drawn in the page's colour, a pill whose label fades into its fill or a
// tint that never reaches the screen would be found.
//
// Like every [display] case these SKIP unless CONTOUR_TEST_DISPLAY=1 opts into a real windowing system
// (scripts/run-display-tests.sh runs them under a headless weston).

#include <contour/config/Actions.hpp>
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
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtrasterizer/RailGeometry.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>

#include <QtCore/QEvent>
#include <QtCore/QPointF>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using contour::test::DisplayHarness;
using contour::test::HarnessWindow;
using namespace std::string_view_literals;

namespace
{

#define REQUIRE_DISPLAY_OR_SKIP()                                                          \
    do                                                                                     \
    {                                                                                      \
        if (!contour::test::displayTestsEnabled(core::LiveEnvironment {}))                 \
        {                                                                                  \
            WARN("Skipping display test: set CONTOUR_TEST_DISPLAY=1 with a real display"); \
            return;                                                                        \
        }                                                                                  \
    } while (0)

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
[[nodiscard]] std::string nameOf(Variant const& variant)
{
    return std::format("{} on the {} row",
                       variant.scheme,
                       variant.style == contour::config::UiStyle::Terminal ? "Terminal" : "native");
}

/// Every scheme on every chrome row.
[[nodiscard]] std::vector<Variant> allVariants()
{
    auto variants = std::vector<Variant> {};
    for (auto const scheme: SchemeNames)
        for (auto const style: ChromeRows)
            variants.push_back(Variant { .scheme = scheme, .style = style });
    return variants;
}

/// The harness of @p variant: its scheme, its chrome row, and the block column's exit-status colouring on.
[[nodiscard]] std::unique_ptr<DisplayHarness> makeHarness(Variant const& variant)
{
    auto harness =
        std::make_unique<DisplayHarness>(HarnessWindow::Shown, [&](contour::config::Config& config) {
            config.uiStyle = variant.style;
            auto& profile = config.profiles.value().at("main");
            auto colors =
                contour::config::SimpleColorConfig { .colorScheme = std::string { variant.scheme } };
            REQUIRE(vtbackend::defaultColorPalettes(variant.scheme, colors.colors));
            profile.colors.value() = colors;
        });
    // The page's own colour is painted beneath the terminal (the application window's QML), which the
    // harness's bare window stands in for: black would put a light scheme's ink on a ground it never has.
    auto const background = harness->session->terminal().colorPalette().defaultBackground;
    harness->window->setColor(QColor(background.red, background.green, background.blue));
    return harness;
}

[[nodiscard]] vtbackend::RGBColor rgbOf(QRgb pixel) noexcept
{
    return vtbackend::RGBColor { static_cast<uint8_t>(qRed(pixel)),
                                 static_cast<uint8_t>(qGreen(pixel)),
                                 static_cast<uint8_t>(qBlue(pixel)) };
}

/// The median colour of the @p radius pixels around @p centre: robust against the anti-aliasing at a shape's
/// edge, which a single pixel is not.
[[nodiscard]] vtbackend::RGBColor medianAround(QImage const& frame, QPoint centre, int radius = 1)
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
[[nodiscard]] bool nearlyEqual(vtbackend::RGBColor a, vtbackend::RGBColor b, int tolerance = 6) noexcept
{
    return std::abs(int(a.red) - int(b.red)) <= tolerance
           && std::abs(int(a.green) - int(b.green)) <= tolerance
           && std::abs(int(a.blue) - int(b.blue)) <= tolerance;
}

[[nodiscard]] std::string hexOf(vtbackend::RGBColor color)
{
    return std::format("#{:02x}{:02x}{:02x}", color.red, color.green, color.blue);
}

/// How far apart two colours are, in the sum of the channels' differences: for finding the pixel that stands
/// out most from a ground.
[[nodiscard]] int distanceBetween(vtbackend::RGBColor a, vtbackend::RGBColor b) noexcept
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

[[nodiscard]] RegionColors regionColors(QImage const& frame, QRect const& region)
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

[[nodiscard]] CellGeometry geometryOf(DisplayHarness const& h)
{
    auto const metrics = h.display->gridMetrics();
    return CellGeometry { .metrics = metrics,
                          .cellWidth = unbox<int>(metrics.cellSize.width),
                          .cellHeight = unbox<int>(metrics.cellSize.height),
                          .pageColumns = unbox<int>(h.session->terminal().pageSize().columns) };
}

/// Grabs a frame and the buffer behind it, once the terminal has settled.
[[nodiscard]] Frame frameOf(DisplayHarness const& h)
{
    auto frame = Frame { .image = h.pump(), .buffer = {} };
    REQUIRE_FALSE(frame.image.isNull());
    // What the terminal published for the frame just grabbed: the buffer the renderer drew it from.
    frame.buffer = h.session->terminal().renderBuffer().get();
    return frame;
}

/// Sends a mouse move to the display at item-local logical position @p pos, as the windowing system would.
void moveMouse(contour::display::TerminalDisplay* display, QPointF pos)
{
    QMouseEvent move(
        QEvent::MouseMove, pos, display->mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(display, &move);
}

/// The item-local logical position of the middle of the cell at (@p row, @p column).
[[nodiscard]] QPointF cellCentre(DisplayHarness const& h, int row, int column)
{
    auto const g = geometryOf(h);
    auto const dpr = h.display->devicePixelRatio();
    return { (g.columnLeft(column) + (g.cellWidth / 2.0)) / dpr,
             (g.rowTop(row) + (g.cellHeight / 2.0)) / dpr };
}

/// Three blocks, each on rows of its own: a succeeded `ok` (head row 0, output rows 1-3), a failed `bad` that
/// exited 2 (head row 4, output rows 5-7) and a `slow` that has not ended (head row 8, output rows 9-10).
[[nodiscard]] std::string threeBlocks()
{
    return vtbackend::test::commandRun("ok", { "one", "two", "three" }, 0)
           + vtbackend::test::commandRun("bad", { "one", "two", "three" }, 2)
           + vtbackend::test::commandStarted("slow") + "one\r\ntwo\r\n";
}

constexpr auto OkRow = 0;     ///< The head row of the block that succeeded.
constexpr auto BadRow = 4;    ///< The head row of the block that failed.
constexpr auto SlowRow = 8;   ///< The head row of the block that still runs.
constexpr auto BlockRows = 4; ///< The rows of a finished block above.

/// The gutter cell of the rail on screen row @p row, if the frame has one.
[[nodiscard]] std::optional<vtbackend::RenderGutterCell> railCellAt(vtbackend::RenderBuffer const& buffer,
                                                                    int row)
{
    for (auto const& cell: buffer.gutter)
        if (cell.lineOffset == vtbackend::LineOffset(row) && cell.rail != vtbackend::RailSegment::None)
            return cell;
    return std::nullopt;
}

/// The columns and the row one block chip (or, on the Terminal row, one chip band) covers.
struct ChipRegion
{
    int row = 0;         ///< The screen row.
    int firstColumn = 0; ///< Its first column.
    int lastColumn = 0;  ///< Its last column, included.
};

/// Where the frame draws block chips: the native row's pills, or the Terminal row's bands under their text.
[[nodiscard]] std::vector<ChipRegion> chipRegions(vtbackend::RenderBuffer const& buffer)
{
    auto regions = std::vector<ChipRegion> {};
    for (auto const& chip: buffer.chips)
        regions.push_back(ChipRegion { .row = unbox<int>(chip.lineOffset),
                                       .firstColumn = unbox<int>(chip.columns.fromColumn),
                                       .lastColumn = unbox<int>(chip.columns.toColumn) });
    for (auto const& band: buffer.chipBands)
        regions.push_back(ChipRegion { .row = unbox<int>(band.lineOffset),
                                       .firstColumn = unbox<int>(band.columns.fromColumn),
                                       .lastColumn = unbox<int>(band.columns.toColumn) });
    return regions;
}

/// The part of the frame that holds the grid, which excludes the status line beneath it.
[[nodiscard]] QRect pageAreaOf(DisplayHarness const& h, QImage const& frame)
{
    auto const g = geometryOf(h);
    return QRect { 0, 0, frame.width(), g.rowTop(unbox<int>(h.session->terminal().pageSize().lines)) };
}

/// The bounding box of every pixel inside @p region that differs between @p before and @p after.
[[nodiscard]] QRect changedBounds(QImage const& before, QImage const& after, QRect const& region)
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

} // namespace

namespace
{

/// The page's own colour: the middle of a row nothing was drawn on.
[[nodiscard]] vtbackend::RGBColor pageColorOf(QImage const& image, CellGeometry const& g, int blankRow)
{
    return medianAround(image,
                        QPoint { g.columnLeft(g.pageColumns / 2), g.rowTop(blankRow) + (g.cellHeight / 2) });
}

/// What a chip's pixels are: the colour of its fill (the page's own where it has none), and the ink that
/// stands out most from it.
struct ChipPixels
{
    vtbackend::RGBColor fill {}; ///< What the label sits on.
    vtbackend::RGBColor ink {};  ///< The label's strongest pixel.
    bool filled = false;         ///< Whether the chip has a fill of its own.
};

/// Reads @p region of @p image, a chip drawn over @p page.
///
/// A pill is narrower than the columns it was placed in, so the most common colour in them is the page's: the
/// fill is the commonest colour that is not the page's, when it makes up a fair share of what is not.
[[nodiscard]] ChipPixels chipPixelsOf(QImage const& image, QRect const& region, vtbackend::RGBColor page)
{
    auto counts = std::map<QRgb, int> {};
    auto others = 0;
    for (auto const y: std::views::iota(region.top(), region.bottom() + 1))
        for (auto const x: std::views::iota(region.left(), region.right() + 1))
            if (auto const pixel = image.pixel(x, y); rgbOf(pixel) != page)
            {
                ++counts[pixel];
                ++others;
            }
    auto result = ChipPixels { .fill = page, .ink = page, .filled = false };
    if (counts.empty())
        return result;
    auto const common = std::ranges::max_element(counts, {}, &std::pair<QRgb const, int>::second);
    constexpr auto FillShare = 4; // a quarter of the pixels that are not the page's
    if (common->second * FillShare >= others)
    {
        result.fill = rgbOf(common->first);
        result.filled = true;
    }
    auto farthest = 0;
    for (auto const& [pixel, count]: counts)
    {
        (void) count;
        if (auto const distance = distanceBetween(rgbOf(pixel), result.fill); distance > farthest)
        {
            farthest = distance;
            result.ink = rgbOf(pixel);
        }
    }
    return result;
}

/// Checks that every chip of @p frame reads: its label reaches the text ratio against what it sits on.
/// @return How many chips it checked.
[[nodiscard]] std::size_t checkChipsLegible(DisplayHarness const& h, Frame const& frame, int blankRow)
{
    auto const g = geometryOf(h);
    auto const page = pageColorOf(frame.image, g, blankRow);
    auto const regions = chipRegions(frame.buffer);
    for (auto const& region: regions)
    {
        auto const pixels =
            chipPixelsOf(frame.image, g.cells(region.row, region.firstColumn, region.lastColumn), page);
        INFO("row " << region.row << " columns " << region.firstColumn << ".." << region.lastColumn
                    << " fill " << hexOf(pixels.fill) << " ink " << hexOf(pixels.ink) << " page "
                    << hexOf(page));
        CHECK(pixels.ink != pixels.fill);
        CHECK(vtbackend::contrastRatio(pixels.ink, pixels.fill) >= TextContrast);
    }
    return regions.size();
}

} // namespace

TEST_CASE("display: a block's rail carries its status colour, and its mark reads against the page",
          "[display][blocks][rail]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        if (variant.style != contour::config::UiStyle::Native)
            continue; // the Terminal row has no rail: its block column is glyph cells
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        h->feedAndSettle(threeBlocks());
        auto const frame = frameOf(*h);
        auto const g = geometryOf(*h);
        auto const& rail = g.metrics.rail;
        REQUIRE(rail.bar > 0);

        auto const barCentre = vtrasterizer::railBarLeft(g.metrics) + (rail.bar / 2);
        // Beside the bar: the strip's own page colour, on whichever side the bar leaves room for.
        auto const besideX = rail.barInset >= 4 ? vtrasterizer::railBarLeft(g.metrics) - 2
                                                : vtrasterizer::railBarLeft(g.metrics) + rail.bar + 2;

        auto sampled = std::vector<vtbackend::RGBColor> {};
        for (auto const head: { OkRow, BadRow, SlowRow })
        {
            auto const bodyRow = head + 1;
            auto const cell = railCellAt(frame.buffer, bodyRow);
            REQUIRE(cell.has_value());
            REQUIRE(cell->rail == vtbackend::RailSegment::Body);
            auto const y = g.rowTop(bodyRow) + (g.cellHeight / 2);
            auto const bar = medianAround(frame.image, QPoint { barCentre, y });
            auto const beside = medianAround(frame.image, QPoint { besideX, y });
            INFO("block at row " << head << ": bar " << hexOf(bar) << " expected " << hexOf(cell->railColor)
                                 << " beside " << hexOf(beside));
            CHECK(nearlyEqual(bar, cell->railColor));
            CHECK(vtbackend::contrastRatio(bar, beside) >= MarkContrast);
            sampled.push_back(bar);
        }
        // Three outcomes, three colours: a rail painted in the page's colour or one colour for all would
        // collapse them.
        CHECK(sampled[0] != sampled[1]);
        CHECK(sampled[1] != sampled[2]);

        // The marks: the failed block's cross and the running block's arrow stand out from the page beside.
        for (auto const head: { BadRow, SlowRow })
        {
            auto const cell = railCellAt(frame.buffer, head);
            REQUIRE(cell.has_value());
            REQUIRE(cell->mark != vtbackend::RailMark::None);
            auto const cover = vtrasterizer::railMarkCover(*cell, g.metrics, g.rowTop(head));
            auto const beside =
                medianAround(frame.image, QPoint { besideX, g.rowTop(head) + (g.cellHeight / 2) });
            auto const markPixels =
                regionColors(frame.image, QRect { cover.x, cover.y, cover.width, cover.height });
            INFO("mark on row " << head << ": ink " << hexOf(markPixels.ink) << " beside " << hexOf(beside));
            CHECK(vtbackend::contrastRatio(markPixels.ink, beside) >= MarkContrast);
        }
    }
}

TEST_CASE("display: a block's status chip and pill read against what they sit on", "[display][blocks][chip]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        h->feedAndSettle(threeBlocks());
        auto const frame = frameOf(*h);
        // The two finished blocks and the running one each say how they ended.
        CHECK(checkChipsLegible(*h, frame, /*blankRow=*/20) >= 3);
    }
}

TEST_CASE("display: the hovered block is tinted and ringed, and nothing outside it changes",
          "[display][blocks][hover]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        if (variant.style != contour::config::UiStyle::Native)
            continue; // only a native row draws the tint and the ring
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        // The text cursor animates between frames, which would read as a change outside the block.
        h->feedAndSettle("\033[?25l"sv);
        h->feedAndSettle(threeBlocks());
        auto const before = frameOf(*h);
        REQUIRE_FALSE(before.buffer.blockHighlight.has_value());

        moveMouse(h->display.get(), cellCentre(*h, BadRow + 2, 30));
        auto const after = frameOf(*h);
        REQUIRE(after.buffer.blockHighlight.has_value());

        auto const g = geometryOf(*h);
        auto const block = QRect { 0, g.rowTop(BadRow), after.image.width(), BlockRows * g.cellHeight };
        auto const changed = changedBounds(before.image, after.image, pageAreaOf(*h, after.image));
        INFO("changed " << changed.x() << "," << changed.y() << " " << changed.width() << "x"
                        << changed.height() << ", block " << block.y() << ".." << block.bottom());
        REQUIRE(changed.isValid());
        // Inside the block and not outside it.
        CHECK(block.contains(changed));

        // The tint: an empty cell of the block's output rows is not the colour it was.
        auto const emptyCell = QPoint { g.columnLeft(60), g.rowTop(BadRow + 2) + (g.cellHeight / 2) };
        CHECK(medianAround(after.image, emptyCell) != medianAround(before.image, emptyCell));

        // The ring: along the block's top edge the pixels are the ring's, not the tint's.
        auto const ringPixel = QPoint { g.columnLeft(60), g.rowTop(BadRow) };
        CHECK(after.image.pixel(ringPixel) != after.image.pixel(emptyCell));
        CHECK(after.image.pixel(ringPixel) != before.image.pixel(ringPixel));

        // And the rows of the block beside it are as they were.
        auto const neighbour = QPoint { g.columnLeft(60), g.rowTop(OkRow + 1) + (g.cellHeight / 2) };
        CHECK(after.image.pixel(neighbour) == before.image.pixel(neighbour));
    }
}

TEST_CASE("display: the sticky band drawn over the first row reads at the text ratio",
          "[display][blocks][sticky]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        auto output = std::vector<std::string> {};
        for (auto const line: std::views::iota(0, 40))
            output.push_back(std::format("line {}", line));
        h->feedAndSettle(vtbackend::test::commandRun("bad", output, 2)
                         + vtbackend::test::commandRun("ok", output, 0));

        auto seen = std::vector<vtbackend::CommandBlockOutcome> {};
        for ([[maybe_unused]] auto const page: std::views::iota(0, 8))
        {
            REQUIRE((*h->session)(contour::actions::ScrollPageUp {}));
            auto const frame = frameOf(*h);
            if (!frame.buffer.stickyHeader.has_value())
                continue;
            auto const& header = *frame.buffer.stickyHeader;
            // With no pane header or title band to host it, the band falls back to the grid's first row.
            REQUIRE(header.presentation == vtbackend::StickyHeaderPresentation::InGrid);
            if (std::ranges::find(seen, header.outcome) == seen.end())
                seen.push_back(header.outcome);

            auto const g = geometryOf(*h);
            auto const band =
                QRect { g.columnLeft(0), g.rowTop(0), g.pageColumns * g.cellWidth, g.cellHeight - 1 };
            auto const fill = regionColors(frame.image, band).ground;
            INFO("outcome " << int(header.outcome) << " band " << hexOf(fill) << " expected "
                            << hexOf(header.background));
            CHECK(nearlyEqual(fill, header.background));

            auto const spanOf = [&](std::vector<vtbackend::RenderCell> const& cells) {
                auto first = g.pageColumns;
                auto last = 0;
                for (auto const& cell: cells)
                {
                    first = std::min(first, unbox<int>(cell.position.column));
                    last = std::max(last, unbox<int>(cell.position.column) + int(cell.width) - 1);
                }
                return QRect {
                    g.columnLeft(first), g.rowTop(0), (last - first + 1) * g.cellWidth, g.cellHeight - 1
                };
            };
            REQUIRE_FALSE(header.cells.empty());
            REQUIRE_FALSE(header.chip.empty());
            auto const text = regionColors(frame.image, spanOf(header.cells));
            auto const chip = regionColors(frame.image, spanOf(header.chip));
            INFO("text ink " << hexOf(text.ink) << " chip ink " << hexOf(chip.ink));
            // The command's text is the scheme's own ink, as legible as the grid's text beside it
            // (solarized-light's is 3.7:1 by design); the chip is the band's own ink, lifted to the text
            // ratio.
            auto const& palette = h->session->terminal().colorPalette();
            auto const gridContrast =
                vtbackend::contrastRatio(palette.defaultForeground, palette.defaultBackground);
            CHECK(vtbackend::contrastRatio(text.ink, fill) >= std::min(TextContrast, gridContrast) - 0.5);
            CHECK(vtbackend::contrastRatio(chip.ink, fill) >= TextContrast);
        }
        // Both blocks' bands were seen, the failed one's chip in its lifted red among them.
        CHECK(std::ranges::find(seen, vtbackend::CommandBlockOutcome::Failure) != seen.end());
        CHECK(std::ranges::find(seen, vtbackend::CommandBlockOutcome::Success) != seen.end());
    }
}

TEST_CASE("display: a chip on a double-width command row sits on the drawn half", "[display][blocks][decdwl]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        // DECDWL on the prompt row: the command typed after it is drawn at twice the width. It succeeded: a
        // failed block's longer chip does not fit the half page beside the text, and collapses to the rail.
        h->feedAndSettle(
            std::format("\033]133;A\033\\\033#6$ \033]133;B\033\\xxxx\r\n\033]133;C\033\\boom\r\n{}{}",
                        vtbackend::test::commandEnd(0),
                        vtbackend::test::PromptStart));
        auto const frame = frameOf(*h);
        auto const g = geometryOf(*h);
        auto const textColumns = core::locked(h->session->terminal(), [&] {
            return unbox<int>(h->session->terminal()
                                  .primaryScreen()
                                  .grid()
                                  .lineAt(vtbackend::LineOffset(0))
                                  .trimmedColumns());
        });
        REQUIRE(textColumns > 0);

        auto onRowZero = 0;
        auto const page = pageColorOf(frame.image, g, /*blankRow=*/20);
        for (auto const& region: chipRegions(frame.buffer))
        {
            if (region.row != 0)
                continue;
            ++onRowZero;
            // Past the doubled text, and on whole doubled columns.
            CHECK(region.firstColumn >= 2 * textColumns);
            CHECK(region.lastColumn < g.pageColumns);
            // Anchored to the right of the DRAWN columns, which a doubled row spreads over the whole page: in
            // the cells' own columns it would sit in the page's left half, over the doubled text.
            CHECK(region.lastColumn >= (g.pageColumns * 3) / 4);
            auto const pixels =
                chipPixelsOf(frame.image, g.cells(0, region.firstColumn, region.lastColumn), page);
            INFO("fill " << hexOf(pixels.fill) << " ink " << hexOf(pixels.ink));
            CHECK(vtbackend::contrastRatio(pixels.ink, pixels.fill) >= TextContrast);
            if (pixels.filled)
            {
                // The doubled text's own columns hold none of the chip's fill.
                auto const textArea = g.cells(0, 0, (2 * textColumns) - 1);
                auto const under = chipPixelsOf(frame.image, textArea, page);
                CHECK_FALSE((under.filled && under.fill == pixels.fill));
            }
        }
        CHECK(onRowZero >= 1);
    }
}
