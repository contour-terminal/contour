// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

namespace contour::display
{

/// The viewport presented as ONE flat string, rows joined by a newline — the shape
/// QAccessibleTextInterface speaks.
///
/// Assistive technology addresses text by a single integer offset; a terminal is a grid. The whole
/// translation is this arithmetic, so it lives here rather than inside a QAccessibleInterface that cannot
/// be constructed without a window.
///
/// A row of N columns occupies N + 1 offsets: the cells, then the newline that separates it from the row
/// below. The trailing row has one too, so every row is the same width in offset space — which is what
/// makes both directions plain division and modulo.
///
/// @param cell    A viewport-relative cell.
/// @param columns The grid width, in cells.
/// @return The flat offset of that cell.
[[nodiscard]] constexpr int flatOffsetOf(vtbackend::CellLocation cell,
                                         vtbackend::ColumnCount columns) noexcept
{
    return (cell.line.value * (columns.value + 1)) + cell.column.value;
}

/// The inverse of @ref flatOffsetOf.
///
/// An offset landing exactly on a row's newline maps to the column one past the end of that row, which is
/// where a caret sitting at the end of a line belongs.
///
/// @param offset  A flat offset.
/// @param columns The grid width, in cells.
/// @return The cell it addresses.
[[nodiscard]] constexpr vtbackend::CellLocation cellAtFlatOffset(int offset,
                                                                 vtbackend::ColumnCount columns) noexcept
{
    auto const stride = columns.value + 1;
    return { .line = vtbackend::LineOffset(offset / stride),
             .column = vtbackend::ColumnOffset(offset % stride) };
}

/// How many offsets the whole viewport spans, newlines included.
///
/// @param pageSize The grid size.
/// @return One past the last valid offset.
[[nodiscard]] constexpr int flatTextLength(vtbackend::PageSize pageSize) noexcept
{
    return pageSize.lines.value * (pageSize.columns.value + 1);
}

} // namespace contour::display
