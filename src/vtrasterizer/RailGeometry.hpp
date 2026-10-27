// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the block rail is drawn in its strip, and how far the gutter's cells and the rail's marks are moved
/// to sit beside it: pure geometry over GridMetrics, so the strip is testable without a render target.

#include <vtbackend/core/BlockRailKinds.hpp>
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/GutterCells.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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

/// The few rectangles a rail row, a chip band or a hover ring is drawn as, held inline: built per cell per
/// frame, so a frame allocates nothing for them (m1).
struct RailRects
{
    std::array<RailRect, 4> rects {}; ///< The rectangles, the first @ref count of them in use.
    std::size_t count = 0;            ///< How many are.

    /// Appends @p rect; there is room for four, the most a row, a band or a ring is drawn as.
    void add(RailRect rect) noexcept { rects.at(count++) = rect; }

    /// @return The rectangles in use.
    [[nodiscard]] std::span<RailRect const> all() const noexcept { return std::span(rects).first(count); }
};

/// The rectangles of a hovered block's ring: the edges of @p box, @p ring pixels thick, inside it.
///
/// The top and bottom edges run whole, the sides between them -- except along an edge the block continues
/// past
/// (@p continues): the page cuts the block there, and a closed edge would say it ends, so the edge is left
/// out and the sides reach the box's own end.
/// @param box The block's rows, across the page.
/// @param ring The ring's thickness in pixels; none for a ring that does not fit twice in the box.
/// @param continues The edges the block runs past.
/// @return The rectangles; none without a ring.
[[nodiscard]] inline RailRects hoverRingRects(RailRect box,
                                              int ring,
                                              vtbackend::BlockContinuations continues) noexcept
{
    auto rects = RailRects {};
    ring = std::min(ring, std::min(box.width, box.height) / 2);
    if (ring <= 0)
        return rects;

    auto const opensAbove = continues.test(vtbackend::BlockContinues::Above);
    auto const opensBelow = continues.test(vtbackend::BlockContinues::Below);
    if (!opensAbove)
        rects.add(RailRect { .x = box.x, .y = box.y, .width = box.width, .height = ring });
    if (!opensBelow)
        rects.add(
            RailRect { .x = box.x, .y = box.y + box.height - ring, .width = box.width, .height = ring });

    auto const sidesTop = opensAbove ? box.y : box.y + ring;
    auto const sidesBottom = opensBelow ? box.y + box.height : box.y + box.height - ring;
    auto const sidesHeight = sidesBottom - sidesTop;
    rects.add(RailRect { .x = box.x, .y = sidesTop, .width = ring, .height = sidesHeight });
    rects.add(
        RailRect { .x = box.x + box.width - ring, .y = sidesTop, .width = ring, .height = sidesHeight });
    return rects;
}

/// How many cells wide a rail mark is drawn: LineFlag::DoubleWidth, as a two-cell block column's glyph is.
inline constexpr auto MarkCells = 2;

/// The bar's left edge, in device pixels: @c barInset into the strip that ends at column 0.
/// @param metrics The grid's metrics.
/// @return The x coordinate.
[[nodiscard]] constexpr int railBarLeft(GridMetrics const& metrics) noexcept
{
    return metrics.pageMargin.left - metrics.rail.strip + metrics.rail.barInset;
}

/// How a rail mark is drawn.
enum class MarkShape : uint8_t
{
    Glyph = 0, ///< The glyph alone, in the mark's colour.
    Disc,      ///< A disc of the mark's colour, the glyph knocked out of it in the page's.
    Cells,     ///< Not in the UI font: the private-use glyph, in cells (the user mark's ◆).
};

/// One rail mark's face.
struct RailMarkFace
{
    vtbackend::RailMark mark = vtbackend::RailMark::None; ///< The mark.
    char32_t glyph {};                                    ///< What it says; 0 for none.
    MarkShape shape = MarkShape::Glyph;                   ///< How it is drawn.
};

/// The canvas's marks, by RailMark: ✗ on a danger disc, the running ▸ and the hovered ✓ alone.
inline constexpr auto RailMarkFaces = std::array {
    RailMarkFace { .mark = vtbackend::RailMark::None, .glyph = 0, .shape = MarkShape::Cells },
    RailMarkFace { .mark = vtbackend::RailMark::Check, .glyph = U'✓', .shape = MarkShape::Glyph },
    RailMarkFace { .mark = vtbackend::RailMark::Cross, .glyph = U'✗', .shape = MarkShape::Disc },
    RailMarkFace { .mark = vtbackend::RailMark::Running, .glyph = U'▸', .shape = MarkShape::Glyph },
    RailMarkFace { .mark = vtbackend::RailMark::UserMark, .glyph = 0, .shape = MarkShape::Cells },
};
static_assert(RailMarkFaces.size() == static_cast<std::size_t>(vtbackend::RailMark::UserMark) + 1);

/// The face of @p mark.
/// @param mark The mark.
/// @return Its row in RailMarkFaces.
[[nodiscard]] constexpr RailMarkFace const& railMarkFace(vtbackend::RailMark mark) noexcept
{
    return RailMarkFaces[static_cast<std::size_t>(mark)];
}

/// The box a rail mark of @p metrics is drawn in on the row at @p rowTop: a square of the mark's edge (the
/// row's height at most), centred on the bar and on the row.
/// @param metrics The grid's metrics.
/// @param rowTop The row's top, in device pixels.
/// @return The box.
[[nodiscard]] constexpr RailRect railMarkDiscBox(GridMetrics const& metrics, int rowTop) noexcept
{
    auto const rowHeight = metrics.cellSize.height.as<int>();
    auto const edge = std::min(metrics.rail.chips.markEdge, rowHeight);
    auto const centre = railBarLeft(metrics) + (metrics.rail.bar / 2);
    return RailRect {
        .x = centre - (edge / 2), .y = rowTop + ((rowHeight - edge) / 2), .width = edge, .height = edge
    };
}

/// Whether @p mark is drawn in the UI font, centred on the bar at @c size.railMark -- rather than as its
/// two-cell private-use glyph, as the user mark is, and as every mark is where the row has no chip metrics.
/// @param mark The mark.
/// @param metrics The grid's metrics.
/// @return Whether it is.
[[nodiscard]] constexpr bool drawsMarkInUiFont(vtbackend::RailMark mark, GridMetrics const& metrics) noexcept
{
    return railMarkFace(mark).shape != MarkShape::Cells && metrics.rail.chips.markEdge > 0;
}

/// The box @p cell's mark covers on its row at @p rowTop: its disc's (railMarkDiscBox()), or, for a mark in
/// cells, the whole row two cells wide.
/// @param cell The rail cell, which carries a mark.
/// @param metrics The grid's metrics.
/// @param rowTop The row's top.
/// @return The box.
[[nodiscard]] constexpr RailRect railMarkCover(vtbackend::RenderGutterCell const& cell,
                                               GridMetrics const& metrics,
                                               int rowTop) noexcept
{
    if (drawsMarkInUiFont(cell.mark, metrics))
        return railMarkDiscBox(metrics, rowTop);
    auto const width = MarkCells * metrics.cellSize.width.as<int>();
    return RailRect { .x = railBarLeft(metrics) + (metrics.rail.bar / 2) - (width / 2),
                      .y = rowTop,
                      .width = width,
                      .height = metrics.cellSize.height.as<int>() };
}

/// The rectangles @p cell's rail segment is drawn as on its row: the bar, kept @c endInset clear of the end
/// of a block it starts or ends, and at each such end a cap row one pixel narrower on either side, which is
/// as much of @c radius as rectangles can draw. On a row carrying a mark the bar stops at the mark's box and
/// resumes below it (railMarkCover()), where the block goes on above or below the row.
/// @param cell The rail cell.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset, in device pixels.
/// @return The rectangles; none for a row with no segment.
[[nodiscard]] inline RailRects railRects(vtbackend::RenderGutterCell const& cell,
                                         GridMetrics const& metrics,
                                         int smoothScrollOffset)
{
    using vtbackend::RailSegment;
    if (cell.rail == RailSegment::None)
        return {};

    auto const& rail = metrics.rail;
    auto const rowTop = metrics.mapTopLeft(cell.lineOffset, vtbackend::ColumnOffset(0), smoothScrollOffset).y;
    auto const rowBottom = rowTop + metrics.cellSize.height.as<int>();
    auto const startsHere = cell.rail == RailSegment::Head || cell.rail == RailSegment::Single;
    auto const endsHere = cell.rail == RailSegment::Tail || cell.rail == RailSegment::Single;
    auto const x = railBarLeft(metrics);

    if (cell.mark != vtbackend::RailMark::None)
    {
        // The mark stands in for the bar's end it sits on; the bar runs on to the rows beside it.
        auto const box = railMarkCover(cell, metrics, rowTop);
        auto rects = RailRects {};
        if (!startsHere && box.y > rowTop)
            rects.add(RailRect { .x = x, .y = rowTop, .width = rail.bar, .height = box.y - rowTop });
        if (!endsHere && box.y + box.height < rowBottom)
            rects.add(RailRect { .x = x,
                                 .y = box.y + box.height,
                                 .width = rail.bar,
                                 .height = rowBottom - (box.y + box.height) });
        return rects;
    }

    auto const top = rowTop + (startsHere ? rail.endInset : 0);
    auto const bottom = rowBottom - (endsHere ? rail.endInset : 0);

    auto rects = RailRects {};
    rects.add(RailRect { .x = x, .y = top, .width = rail.bar, .height = bottom - top });
    auto const rounded = rail.radius > 0 && rail.bar > 2;
    if (rounded && startsHere)
        rects.add(RailRect { .x = x + 1, .y = top - 1, .width = rail.bar - 2, .height = 1 });
    if (rounded && endsHere)
        rects.add(RailRect { .x = x + 1, .y = bottom, .width = rail.bar - 2, .height = 1 });
    return rects;
}

/// The rectangles a chip's band is drawn as: its columns across its row, its corners cut by a pixel -- as
/// much of @c radius.chip as rectangles can draw, as the rail's caps are.
/// @param band The band.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset, in device pixels.
/// @return The middle, then the left and the right end column.
[[nodiscard]] inline RailRects chipBandRects(vtbackend::RenderChipBand const& band,
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
    return RailRects {
        .rects = { RailRect { .x = left.x + 1, .y = left.y, .width = right - left.x - 2, .height = height },
                   RailRect { .x = left.x, .y = left.y + 1, .width = 1, .height = height - 2 },
                   RailRect { .x = right - 1, .y = left.y + 1, .width = 1, .height = height - 2 } },
        .count = 3,
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
