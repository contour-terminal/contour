// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// One grapheme cluster of an input method's preedit, measured.
struct PreeditCluster
{
    std::u32string text; ///< The cluster's codepoints.
    ColumnCount width; ///< The columns it covers: at least one, so that every cluster has a cell of its own.
};

/// Where on its line the preedit is drawn, and what of it fits.
struct PreeditPlacement
{
    ColumnOffset start;                   ///< The column its first cluster is drawn at.
    std::vector<PreeditCluster> clusters; ///< The clusters drawn, in order: the line's columns hold them all.
    ColumnCount columns;                  ///< The columns @ref clusters cover.
};

/// Places an input method's preedit on the line the cursor is on.
///
/// From the cursor when it fits there. When it does not -- typed at the end of a line, where even its first
/// cluster may not fit -- it moves left until its end meets the line's right edge, so the composition stays
/// in sight. Only a preedit wider than the whole line is cut: it starts at the line's first column, and the
/// first cluster that would end past the edge is left out, with everything after it.
///
/// A preedit moved left never starts on the right half of a wide glyph, which drawn from its left half would
/// cover the preedit's first column: it starts one column further left instead. At the cursor it never steps,
/// as the cursor never stands on such a column.
///
/// The candidate window anchors at the cell @ref PreeditPlacement::start names, the preedit's first cell.
///
/// @param preedit             The preedit, UTF-8.
/// @param cursorColumn        The cursor's column on its line.
/// @param lineColumns         How many columns the line has (@see preeditLineColumns).
/// @param continuesAWideGlyph Whether a column of the line is the right half of a wide glyph.
/// @return Where it is drawn; an empty preedit is drawn nowhere, at the cursor.
[[nodiscard]] PreeditPlacement placePreedit(std::string_view preedit,
                                            ColumnOffset cursorColumn,
                                            ColumnCount lineColumns,
                                            std::function<bool(ColumnOffset)> const& continuesAWideGlyph);

/// Places a preedit on a line that holds no wide glyph. @see the overload taking a wide-glyph test.
[[nodiscard]] PreeditPlacement placePreedit(std::string_view preedit,
                                            ColumnOffset cursorColumn,
                                            ColumnCount lineColumns);

/// How many columns a preedit has on a line: the page's, or half of them on a DECDWL line, every cell of
/// which is drawn two columns wide.
/// @param pageColumns The page's columns.
/// @param lineFlags   The line's flags.
/// @return The line's columns, in its own (grid) cells.
[[nodiscard]] constexpr ColumnCount preeditLineColumns(ColumnCount pageColumns, LineFlags lineFlags) noexcept
{
    return lineFlags.test(LineFlag::DoubleWidth) ? pageColumns / 2 : pageColumns;
}

} // namespace vtbackend
