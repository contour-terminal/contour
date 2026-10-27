// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the block rail is drawn in its strip, and how far the gutter's cells and the rail's marks are moved
/// to sit beside it: pure geometry over GridMetrics, so the strip is testable without a render target.

#include <vtbackend/core/BlockRailKinds.hpp>
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/GutterCells.hpp>

#include <vector>

namespace vtrasterizer
{

/// One rectangle of the rail, in device pixels.
struct RailRect
{
    int x = 0;      ///< Left edge.
    int y = 0;      ///< Top edge.
    int width = 0;  ///< Width.
    int height = 0; ///< Height.

    bool operator==(RailRect const&) const = default;
};

/// How many cells wide a rail mark is drawn: LineFlag::DoubleWidth, as a two-cell block column's glyph is.
inline constexpr auto MarkCells = 2;

/// The bar's left edge, in device pixels: @c barInset into the strip that ends at column 0.
/// @param metrics The grid's metrics.
/// @return The x coordinate.
[[nodiscard]] constexpr int railBarLeft(GridMetrics const& metrics) noexcept
{
    return metrics.pageMargin.left - metrics.rail.strip + metrics.rail.barInset;
}

/// The rectangles @p cell's rail segment is drawn as on its row: the bar, kept @c endInset clear of the end
/// of a block it starts or ends, and at each such end a cap row one pixel narrower on either side, which is
/// as much of @c radius as rectangles can draw. A row carrying a mark draws no bar: the mark stands in its
/// place.
/// @param cell The rail cell.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset, in device pixels.
/// @return The rectangles; none for a row with no segment, or with a mark.
[[nodiscard]] inline std::vector<RailRect> railRects(vtbackend::RenderGutterCell const& cell,
                                                     GridMetrics const& metrics,
                                                     int smoothScrollOffset)
{
    using vtbackend::RailSegment;
    if (cell.rail == RailSegment::None || cell.mark != vtbackend::RailMark::None)
        return {};

    auto const& rail = metrics.rail;
    auto const rowTop = metrics.mapTopLeft(cell.lineOffset, vtbackend::ColumnOffset(0), smoothScrollOffset).y;
    auto const startsHere = cell.rail == RailSegment::Head || cell.rail == RailSegment::Single;
    auto const endsHere = cell.rail == RailSegment::Tail || cell.rail == RailSegment::Single;
    auto const top = rowTop + (startsHere ? rail.endInset : 0);
    auto const bottom = rowTop + metrics.cellSize.height.as<int>() - (endsHere ? rail.endInset : 0);
    auto const x = railBarLeft(metrics);

    auto rects = std::vector { RailRect { .x = x, .y = top, .width = rail.bar, .height = bottom - top } };
    auto const rounded = rail.radius > 0 && rail.bar > 2;
    if (rounded && startsHere)
        rects.push_back(RailRect { .x = x + 1, .y = top - 1, .width = rail.bar - 2, .height = 1 });
    if (rounded && endsHere)
        rects.push_back(RailRect { .x = x + 1, .y = bottom, .width = rail.bar - 2, .height = 1 });
    return rects;
}

/// The rectangles a chip's band is drawn as: its columns across its row, its corners cut by a pixel -- as
/// much of @c radius.chip as rectangles can draw, as the rail's caps are.
/// @param band The band.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset, in device pixels.
/// @return The middle, then the left and the right end column.
[[nodiscard]] inline std::vector<RailRect> chipBandRects(vtbackend::RenderChipBand const& band,
                                                         GridMetrics const& metrics,
                                                         int smoothScrollOffset)
{
    auto const left = metrics.mapTopLeft(band.lineOffset, band.columns.fromColumn, smoothScrollOffset);
    auto const right = metrics
                           .mapTopLeft(band.lineOffset,
                                       band.columns.toColumn + vtbackend::ColumnOffset(1),
                                       smoothScrollOffset)
                           .x;
    auto const height = metrics.cellSize.height.as<int>();
    return std::vector {
        RailRect { .x = left.x + 1, .y = left.y, .width = right - left.x - 2, .height = height },
        RailRect { .x = left.x, .y = left.y + 1, .width = 1, .height = height - 2 },
        RailRect { .x = right - 1, .y = left.y + 1, .width = 1, .height = height - 2 },
    };
}

/// The box @p cell's mark is drawn in: two cells wide -- the mark is flagged LineFlag::DoubleWidth, so it
/// sizes itself to the row rather than to one cell's width -- centred on the bar, top-aligned with its row.
/// @param cell The rail cell.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset, in device pixels.
/// @return The box.
[[nodiscard]] constexpr RailRect railMarkBox(vtbackend::RenderGutterCell const& cell,
                                             GridMetrics const& metrics,
                                             int smoothScrollOffset) noexcept
{
    auto const width = MarkCells * metrics.cellSize.width.as<int>();
    auto const centre = railBarLeft(metrics) + (metrics.rail.bar / 2);
    return RailRect {
        .x = centre - (width / 2),
        .y = metrics.mapTopLeft(cell.lineOffset, vtbackend::ColumnOffset(0), smoothScrollOffset).y,
        .width = width,
        .height = metrics.cellSize.height.as<int>(),
    };
}

/// How far left the grid's origin moves for a mark drawn at column 0 to land in its railMarkBox().
/// @param metrics The grid's metrics.
/// @return The shift, in device pixels.
[[nodiscard]] constexpr int railMarkShift(GridMetrics const& metrics) noexcept
{
    return metrics.pageMargin.left - (railBarLeft(metrics) + (metrics.rail.bar / 2))
           + ((MarkCells * metrics.cellSize.width.as<int>()) / 2);
}

/// How far left the gutter's cells -- the timestamps and line numbers -- are drawn: past the strip, so the
/// strip lies between them and column 0. 0 where there is no strip, and the glyph column draws as it did.
/// @param metrics The grid's metrics.
/// @return The shift, in device pixels.
[[nodiscard]] constexpr int cellGutterShift(GridMetrics const& metrics) noexcept
{
    return metrics.rail.strip;
}

/// @p metrics with the grid's origin moved @p shift pixels left: what a cell drawn at a column of it lands
/// beside.
/// @param metrics The grid's metrics.
/// @param shift The shift, in device pixels.
/// @return The moved metrics.
[[nodiscard]] constexpr GridMetrics shiftedLeft(GridMetrics metrics, int shift) noexcept
{
    metrics.pageMargin.left -= shift;
    return metrics;
}

/// The private-use glyph @p mark is drawn as (docs/vt-extensions/private-use-glyphs.md).
/// @param mark The mark.
/// @return Its codepoint; 0 for none.
[[nodiscard]] constexpr char32_t railMarkCodepoint(vtbackend::RailMark mark) noexcept
{
    switch (mark)
    {
        case vtbackend::RailMark::None: return 0;
        case vtbackend::RailMark::Check: return 0x10F010;
        case vtbackend::RailMark::Cross: return 0x10F011;
        case vtbackend::RailMark::Running: return 0x10F002;
        case vtbackend::RailMark::UserMark: return 0x10F003;
    }
    return 0;
}

/// The cell @p cell's mark is drawn as: its glyph at column 0 of its row, in the mark's ink, flagged
/// LineFlag::DoubleWidth by the terminal. Drawn with the grid's origin moved by railMarkShift().
/// @param cell A rail cell with a mark.
/// @return The cell.
[[nodiscard]] inline vtbackend::RenderCell railMarkCell(vtbackend::RenderGutterCell const& cell)
{
    return detail::standaloneCell(
        railMarkCodepoint(cell.mark),
        vtbackend::CellLocation { .line = cell.lineOffset, .column = vtbackend::ColumnOffset(0) },
        cell.attributes);
}

} // namespace vtrasterizer
