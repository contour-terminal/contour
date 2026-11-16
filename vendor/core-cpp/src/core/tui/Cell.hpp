// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp> // For Style

#include <string>

namespace core::tui
{

/// A single terminal cell containing a grapheme cluster and styling.
///
/// Cells represent individual character positions on the terminal screen.
/// Each cell can hold a Unicode grapheme cluster (which may be multiple
/// code points) and associated styling information.
///
/// Wide characters (e.g., CJK characters, emoji) occupy two cells:
/// - The first cell contains the grapheme with width=2
/// - The second cell is a continuation cell with empty grapheme and width=0
struct Cell
{
    std::string grapheme; ///< UTF-8 grapheme cluster (empty for continuation cells)
    Style style;          ///< Visual style (colors, attributes)
    uint8_t width = 1;    ///< Display width: 0=continuation, 1=normal, 2=wide

    /// Resets the cell to a space with the given style.
    void reset(Style const& defaultStyle = {})
    {
        grapheme = " ";
        style = defaultStyle;
        width = 1;
    }

    /// Resets the cell to empty (for continuation cells).
    void resetContinuation()
    {
        grapheme.clear();
        style = {};
        width = 0;
    }

    /// Returns true if this is a continuation cell (part of a wide character).
    [[nodiscard]] bool isContinuation() const noexcept { return width == 0; }

    /// Returns true if this is a wide character cell.
    [[nodiscard]] bool isWide() const noexcept { return width == 2; }

    /// Equality comparison.
    [[nodiscard]] bool operator==(Cell const& other) const noexcept;

    /// Inequality comparison.
    [[nodiscard]] bool operator!=(Cell const& other) const noexcept { return !(*this == other); }
};

} // namespace core::tui
