// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <concepts>

namespace vtbackend
{

/// What a word walk asks of the cells it walks: their page, where a row continues the one above after a
/// soft wrap, and which cells end a word.
///
/// Both walks of a word go through it -- the double-click selection (WordWiseSelection, over a
/// SelectionHelper) and the word under the cursor (Grid::wordRangeUnderCursor, over the grid itself) --
/// so the two cannot disagree on where a word ends. A wide character's second half needs no stepping
/// over: it ends a word exactly where the character does (Grid::cellEmptyOrContainsOneOf).
template <typename T>
concept WordWalkCells = requires(T const& cells, CellLocation position, LineOffset line) {
    { cells.pageSize() } -> std::convertible_to<PageSize>;
    { cells.continuesRowAbove(line) } -> std::same_as<bool>;
    { cells.isWordDelimiter(position) } -> std::same_as<bool>;
};

/// The first cell of the word that holds @p position.
///
/// The walk goes left, into the row above only where this row continues it after a soft wrap, and stops
/// before the first cell that ends a word.
/// @param cells    The cells walked.
/// @param position A cell of the word.
/// @return The word's first cell.
template <WordWalkCells Cells>
[[nodiscard]] CellLocation wordBegin(Cells const& cells, CellLocation position)
{
    auto last = position;
    auto current = last;
    for (;;)
    {
        if (*current.column > 0)
            current.column--;
        else if (cells.continuesRowAbove(current.line))
        {
            current.line--;
            current.column = boxed_cast<ColumnOffset>(cells.pageSize().columns) - 1;
        }
        else
            break;

        if (cells.isWordDelimiter(current))
            break;
        last = current;
    }
    return last;
}

/// The last cell of the word that holds @p position.
///
/// The walk goes right, into the row below only where that row continues this one after a soft wrap, and
/// stops before the first cell that ends a word. A wide character at the word's end is in it whole.
/// @param cells    The cells walked.
/// @param position A cell of the word.
/// @return The word's last cell.
template <WordWalkCells Cells>
[[nodiscard]] CellLocation wordEnd(Cells const& cells, CellLocation position)
{
    auto const pageSize = PageSize(cells.pageSize());
    auto last = position;
    auto current = last;
    for (;;)
    {
        if (*current.column + 1 < *pageSize.columns)
            current.column++;
        else if (*current.line + 1 < *pageSize.lines && cells.continuesRowAbove(current.line + 1))
            current = CellLocation { .line = current.line + 1, .column = ColumnOffset(0) };
        else
            break;

        if (cells.isWordDelimiter(current))
            break;
        last = current;
    }
    return last;
}

} // namespace vtbackend
