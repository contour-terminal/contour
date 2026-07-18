// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QRectF>

namespace contour::display
{

/// The rectangle of a run of cells, in ITEM-LOCAL LOGICAL coordinates.
///
/// The cell grid renders in device pixels, inset by the page margin (see GridMetrics::map()), while Qt
/// expects rectangles in the item's logical coordinate space — so every device-pixel quantity divides by
/// the device-pixel ratio.
///
/// @param pageMargin Content inset within the item, in device pixels (left/top are used).
/// @param cellSize   Cell size in device pixels.
/// @param cell       Top-left cell, in grid coordinates (viewport-relative line/column).
/// @param cellWidth  How many cells wide the rectangle is (2 for a double-width glyph).
/// @param dpr        Device-pixel ratio (> 0).
/// @return The rectangle in item-local logical coordinates.
[[nodiscard]] inline QRectF cellRectangle(vtrasterizer::PageMargin pageMargin,
                                          vtbackend::ImageSize cellSize,
                                          vtbackend::CellLocation cell,
                                          uint8_t cellWidth,
                                          double dpr) noexcept
{
    auto const cellWidthPx = unbox<double>(cellSize.width);
    auto const cellHeightPx = unbox<double>(cellSize.height);
    auto const left = (pageMargin.left + (unbox<double>(cell.column) * cellWidthPx)) / dpr;
    auto const top = (pageMargin.top + (unbox<double>(cell.line) * cellHeightPx)) / dpr;
    return { left, top, (cellWidthPx * cellWidth) / dpr, cellHeightPx / dpr };
}

/// The rectangle covering whole viewport rows [@p firstLine, @p lastLine], full width.
///
/// Used to describe a region that spans lines rather than cells — a shell prompt, say — to something that
/// wants one box for it.
///
/// @param pageMargin Content inset within the item, in device pixels.
/// @param cellSize   Cell size in device pixels.
/// @param firstLine  First viewport-relative row, inclusive.
/// @param lastLine   Last viewport-relative row, inclusive. Clamped up to @p firstLine.
/// @param columns    Width of the grid, in cells.
/// @param dpr        Device-pixel ratio (> 0).
/// @return The band in item-local logical coordinates.
[[nodiscard]] inline QRectF rowBandRectangle(vtrasterizer::PageMargin pageMargin,
                                             vtbackend::ImageSize cellSize,
                                             vtbackend::LineOffset firstLine,
                                             vtbackend::LineOffset lastLine,
                                             vtbackend::ColumnCount columns,
                                             double dpr) noexcept
{
    auto const effectiveLast = std::max(firstLine, lastLine);
    auto const rowCount = unbox<double>(effectiveLast - firstLine) + 1.0;
    auto const cellHeightPx = unbox<double>(cellSize.height);

    auto const top = (pageMargin.top + (unbox<double>(firstLine) * cellHeightPx)) / dpr;
    return { pageMargin.left / dpr,
             top,
             (unbox<double>(columns) * unbox<double>(cellSize.width)) / dpr,
             (rowCount * cellHeightPx) / dpr };
}

/// An item-local logical rectangle lifted into global screen coordinates.
///
/// A pure translation: the device-pixel ratio was already divided out upstream, and assistive clients are
/// told the rectangle in the same logical screen coordinates Qt hands out — scaling here again is the
/// classic HiDPI double-scale bug.
///
/// Split out from any QQuickItem so the arithmetic is testable without a window.
///
/// @param itemLocal        The rectangle, in item-local logical coordinates.
/// @param itemOriginGlobal The item's (0,0) in global logical coordinates.
/// @return The rectangle in global logical coordinates, rounded to whole pixels.
[[nodiscard]] inline QRect toGlobalRect(QRectF itemLocal, QPointF itemOriginGlobal) noexcept
{
    return itemLocal.translated(itemOriginGlobal).toRect();
}

} // namespace contour::display
