// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/CaretGeometry.h>

#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>

#include <QtCore/QRectF>

namespace contour::display
{

/// Whether an IME query may index the grid at the given cursor position.
///
/// The cursor must lie strictly inside the page: a position captured against one page size must
/// never address a grid that has since shrunk, and the wrap-pending sentinel column (== page
/// width) names no cell — unlike Terminal::contains(), which admits it. Line/column are also
/// required non-negative, which the lexicographic CellLocation ordering alone would not enforce.
///
/// This is exactly vtbackend::strictlyContains(); it is named for the IME call site so the query
/// code reads intent-first. The rule itself lives once, in vtbackend, so this guard and its
/// concurrency test in Terminal_test.cpp cannot drift apart.
///
/// @param cursor   Cursor position in grid coordinates (viewport-relative line/column).
/// @param pageSize The page the grid currently holds.
/// @return true when every grid access at @p cursor is in bounds.
[[nodiscard]] constexpr bool imeCursorAddressable(vtbackend::CellLocation cursor,
                                                  vtbackend::PageSize pageSize) noexcept
{
    return vtbackend::strictlyContains(cursor, pageSize);
}

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
    // The IME rectangle is simply the cursor cell's. Named separately because the two answer different
    // questions — where to park a candidate window, versus where a cell is — and only the geometry is
    // shared; see CaretGeometry.h.
    return cellRectangle(pageMargin, cellSize, cursor, cellWidth, dpr);
}

} // namespace contour::display
