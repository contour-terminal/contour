// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>

#include <QtCore/QRectF>

namespace contour::display
{

/// Computes the IME cursor rectangle (Qt::ImCursorRectangle) in ITEM-LOCAL LOGICAL coordinates.
///
/// The cell grid renders in device pixels, inset by the page margin (see GridMetrics::map()), while
/// Qt expects the rectangle in the item's logical coordinate space — so every device-pixel quantity
/// divides by the device-pixel ratio. The item-local result is sufficient even for a pane inside a
/// split: QQuickWindow maps item coordinates to window/global space when positioning the IME
/// candidate window. A wide (double-width) character under the cursor widens the rectangle so the
/// candidate window aligns with the full glyph.
///
/// @param pageMargin Content inset within the item, in device pixels (left/top are used).
/// @param cellSize   Cell size in device pixels.
/// @param cursor     Cursor position in grid coordinates (viewport-relative line/column).
/// @param cellWidth  Width of the cell under the cursor, in cells (1, or 2 for double-width).
/// @param dpr        Device-pixel ratio (> 0).
/// @return The cursor's cell rectangle in item-local logical coordinates.
[[nodiscard]] inline QRectF imeCursorRectangle(vtrasterizer::PageMargin pageMargin,
                                               vtbackend::ImageSize cellSize,
                                               vtbackend::CellLocation cursor,
                                               uint8_t cellWidth,
                                               double dpr) noexcept
{
    auto const cellWidthPx = unbox<double>(cellSize.width);
    auto const cellHeightPx = unbox<double>(cellSize.height);
    auto const left = (pageMargin.left + (unbox<double>(cursor.column) * cellWidthPx)) / dpr;
    auto const top = (pageMargin.top + (unbox<double>(cursor.line) * cellHeightPx)) / dpr;
    return { left, top, (cellWidthPx * cellWidth) / dpr, cellHeightPx / dpr };
}

} // namespace contour::display
