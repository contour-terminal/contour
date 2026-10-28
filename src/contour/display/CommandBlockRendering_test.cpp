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
#include <contour/test/DisplayPixels.hpp>

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
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/HistorySeed.hpp>

using contour::test::DisplayHarness;
using namespace contour::test::pixel;
using namespace std::string_view_literals;

namespace
{

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
            // The command's text and the chip both read on the band at the text ratio: the copied row's own
            // ink is lifted where the scheme's (solarized-light's is 3.7:1) would not.
            CHECK(vtbackend::contrastRatio(text.ink, fill) >= TextContrast);
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

/// The first column of the divider's left rule that is sampled: the page's top-left corner holds the
/// terminal's own marks (the cursor's outline), which are not the divider's.
constexpr auto RuleFirstColumn = 3;

TEST_CASE("display: a restored pane's divider is dimmed, and the line after it is not",
          "[display][restore][divider]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    using namespace std::chrono_literals;
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        auto const endedAt = std::chrono::local_days { std::chrono::year { 2026 } / 10 / 25 } + 9h + 41min;
        auto const divider =
            vthost::RestoreDivider { .sessionEndedAt = endedAt,
                                     .today = std::chrono::floor<std::chrono::days>(endedAt) };
        vthost::seedRestoredHistory(h->session->terminal(), {}, divider);
        // The line after the divider says the same words, undimmed: the reference for what dim changes.
        auto const text = vthost::restoredDividerText(divider);
        h->feedAndSettle(text);

        auto const frame = frameOf(*h);
        auto const g = geometryOf(*h);
        auto const textCells =
            static_cast<int>(std::ranges::count_if(text, [](char byte) { return (byte & 0xC0) != 0x80; }));
        // The divider centres its words between rules, one space either side.
        auto const rule = (g.pageColumns - textCells - 2) / 2;
        REQUIRE(rule > RuleFirstColumn);
        auto const wordsFirst = rule + 1;
        auto const wordsLast = rule + textCells;

        auto const ground = regionColors(frame.image, g.cells(10, 0, g.pageColumns - 1)).ground;
        auto const rulesLeft = regionColors(frame.image, g.cells(0, RuleFirstColumn, rule - 1));
        auto const rulesRight =
            regionColors(frame.image, g.cells(0, g.pageColumns - rule, g.pageColumns - 1));
        auto const words = regionColors(frame.image, g.cells(0, wordsFirst, wordsLast));
        auto const plain = regionColors(frame.image, g.cells(1, 0, textCells - 1));
        auto const foreground = h->session->terminal().colorPalette().defaultForeground;
        INFO("ground " << hexOf(ground) << " words " << hexOf(words.ink) << " plain " << hexOf(plain.ink)
                       << " rules " << hexOf(rulesLeft.ink) << " " << hexOf(rulesRight.ink));

        // The rules are drawn on both sides of the words, and the words are drawn between them.
        CHECK(rulesLeft.ink != rulesLeft.ground);
        CHECK(rulesRight.ink != rulesRight.ground);
        CHECK(words.ink != words.ground);
        // The line after is in the scheme's own ink; the divider's ink is the dimmed one: farther from it,
        // and nearer the ground.
        CHECK(nearlyEqual(plain.ink, foreground, 24));
        CHECK(words.ink != plain.ink);
        CHECK(distanceBetween(words.ink, ground) < distanceBetween(plain.ink, ground));
        CHECK(distanceBetween(rulesLeft.ink, ground) < distanceBetween(plain.ink, ground));
        // Nothing else on the page is drawn: the divider is one row and the line after it another.
        CHECK(regionColors(frame.image, g.cells(2, 0, g.pageColumns - 1)).groundPixels
              == regionColors(frame.image, g.cells(2, 0, g.pageColumns - 1)).pixels);
    }
}
