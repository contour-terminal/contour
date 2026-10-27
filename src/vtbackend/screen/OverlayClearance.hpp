// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vector>

namespace vtbackend
{

/// Where an overlay drawn over the grid may go without covering anything: each drawn row's blank run at its
/// right end, and the rows it must never cover. What the link pill is placed by (spec R-g).
///
/// The rows count from the display's first drawn row, the status line included, so a frontend maps a row to
/// pixels with one formula. A status line's row is drawn chrome, never blank.
struct OverlayClearance
{
    /// The page's width in cells.
    int columns = 0;

    /// One entry per drawn row, top first: how many cells at its right end are blank, which is to say a
    /// default cell -- no text or a plain space, the default rendition, no hyperlink, no image
    /// (Line::trimmedColumns()). A coloured space is not blank.
    std::vector<int> trailingBlankColumns;

    /// The drawn rows an overlay must not cover, ascending and distinct: the live prompt's and the cursor's,
    /// each only where it is on the page on display.
    std::vector<int> avoidRows;

    [[nodiscard]] bool operator==(OverlayClearance const&) const = default;
};

} // namespace vtbackend
