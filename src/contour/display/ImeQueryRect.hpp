// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/geometry/CellRectangle.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/PreeditPlacement.hpp>

#include <vtrasterizer/GridMetrics.hpp>

#include <QtCore/QRectF>

#include <cstdint>
#include <optional>
#include <string_view>

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

/// The cell an input method's candidate window anchors at, for a cursor at @p cursor.
///
/// The cursor's own cell when it names one. A cursor at column == page width -- the sentinel
/// Terminal::contains() admits, one past the last cell -- names no cell, but is still on its line, so
/// the line's last cell stands in. (A wrap-pending cursor is not that: the grid keeps it on the last
/// cell and flags the pending wrap, so it anchors at itself.) Anywhere else outside the page there is
/// no cell to anchor at, and the caller keeps the anchor it last answered.
///
/// @param cursor   Cursor position in grid coordinates (viewport-relative line/column).
/// @param pageSize The page the grid currently holds.
/// @return The anchor cell, or nothing when @p cursor lies outside the page.
[[nodiscard]] constexpr std::optional<vtbackend::CellLocation> imeAnchorCell(
    vtbackend::CellLocation cursor, vtbackend::PageSize pageSize) noexcept
{
    if (imeCursorAddressable(cursor, pageSize))
        return cursor;

    auto const lastCell = vtbackend::CellLocation { .line = cursor.line, .column = cursor.column - 1 };
    if (cursor.column == pageSize.columns.as<vtbackend::ColumnOffset>()
        && imeCursorAddressable(lastCell, pageSize))
        return lastCell;

    return std::nullopt;
}

/// The cell a candidate window anchors at, and how many columns it spans.
struct ImeAnchor
{
    vtbackend::CellLocation cell {}; ///< The cell, in grid coordinates.
    uint8_t width = 1;               ///< How many columns it spans: 2 under a wide glyph.
};

/// Where the candidate window anchors while @p preedit is composed at @p anchor: the preedit's first cell.
///
/// That is @p anchor itself, unless the preedit does not fit from there and is drawn further left (@see
/// vtbackend::placePreedit, which the render buffer places it by too), and as wide as the preedit's first
/// cluster rather than the grid's glyph beneath it.
///
/// @param anchor      The cursor's anchor cell (@see imeAnchorCell).
/// @param preedit     What the input method is composing; empty when it is composing nothing.
/// @param lineColumns How many columns the cursor's line has.
/// @param cellWidth   The width of the grid's cell at @p anchor, for when nothing is composed.
/// @return The anchor.
[[nodiscard]] inline ImeAnchor imePreeditAnchor(vtbackend::CellLocation anchor,
                                                std::string_view preedit,
                                                vtbackend::ColumnCount lineColumns,
                                                uint8_t cellWidth)
{
    if (preedit.empty())
        return { .cell = anchor, .width = cellWidth };
    auto const placement = vtbackend::placePreedit(preedit, anchor.column, lineColumns);
    auto const width =
        placement.clusters.empty() ? uint8_t { 1 } : unbox<uint8_t>(placement.clusters.front().width);
    return { .cell = { .line = anchor.line, .column = placement.start }, .width = width };
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
    // shared; see geometry/CellRectangle.h.
    return geometry::cellRectangle(pageMargin, cellSize, cursor, cellWidth, dpr);
}

} // namespace contour::display
