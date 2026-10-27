// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

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
/// The candidate window anchors at the cell @ref PreeditPlacement::start names, the preedit's first cell.
///
/// @param preedit      The preedit, UTF-8.
/// @param cursorColumn The cursor's column on its line.
/// @param lineColumns  How many columns the line has.
/// @return Where it is drawn; an empty preedit is drawn nowhere, at the cursor.
[[nodiscard]] PreeditPlacement placePreedit(std::string_view preedit,
                                            ColumnOffset cursorColumn,
                                            ColumnCount lineColumns);

} // namespace vtbackend
