// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/KittyPlaceholderCodepoint.hpp>

#include <cstdint>
#include <span>
#include <vector>

/// Kitty graphics Unicode placeholders: cells whose text is U+10EEEE stand for the cells of a virtual
/// placement, and their colours and combining diacritics say which image, which placement and which
/// cell of it. Everything here is pure: it turns what a line holds into runs of image cells and knows
/// nothing about screens, grids or stored images.
namespace vtbackend::kitty_graphics
{

/// Number of combining characters kitty assigns a row/column number to.
inline constexpr size_t DiacriticCount = 297;

/// Maps a row/column diacritic to the number it encodes.
/// @param codepoint A code point combined with a placeholder cell.
/// @return Its 1-based position in kitty's `rowcolumn-diacritics.txt`, or 0 if it is not one of them.
[[nodiscard]] uint32_t diacriticOrdinal(char32_t codepoint) noexcept;

/// Maps a row/column number to the diacritic that encodes it: the inverse of diacriticOrdinal().
/// @param ordinal A 1-based position in kitty's `rowcolumn-diacritics.txt`.
/// @return Its diacritic, or U+0000 if @p ordinal is 0 or above DiacriticCount.
[[nodiscard]] char32_t diacriticOf(uint32_t ordinal) noexcept;

/// Maps a placeholder cell's foreground (or underline) colour to the id it encodes.
/// @param color The colour as the cell carries it.
/// @return The 24-bit RGB value for an RGB colour, the palette index for an indexed one (8 + n for
///         bright n), and 0 for the default or an undefined colour.
[[nodiscard]] uint32_t placeholderIdOf(Color color) noexcept;

/// Whether a cell is a Unicode placeholder at all.
enum class CellPresence : uint8_t
{
    Absent = 0,      ///< Ordinary text, or an empty cell.
    Placeholder = 1, ///< The cell's text starts with U+10EEEE.
};

/// What one cell of a line says about the placeholder it may be, as read off the cell.
struct PlaceholderCell
{
    /// Whether the cell is a placeholder; the other fields mean nothing when it is not.
    CellPresence presence = CellPresence::Absent;

    /// The low 24 bits of the image id, from the foreground colour.
    uint32_t idLow = 0;

    /// The placement id, from the underline colour; 0 means none.
    uint32_t placementId = 0;

    /// The 1-based ordinal of the first diacritic (the image row); 0 when unknown.
    uint32_t row = 0;

    /// The 1-based ordinal of the second diacritic (the image column); 0 when unknown.
    uint32_t column = 0;

    /// The 1-based ordinal of the third diacritic (the image id's high byte, plus 1); 0 when unknown.
    uint32_t idHigh = 0;
};

/// A run of consecutive cells of one line that show consecutive cells of one image row.
struct PlaceholderRun
{
    /// The line column of the run's first cell.
    uint16_t firstColumn = 0;

    /// The number of cells in the run.
    uint16_t length = 0;

    /// The full 32-bit image id.
    uint32_t imageId = 0;

    /// The placement id; 0 means the image's first virtual placement.
    uint32_t placementId = 0;

    /// The 0-based image row the run shows.
    uint32_t imageRow = 0;

    /// The 0-based image column the run's first cell shows.
    uint32_t imageColumn = 0;

    bool operator==(PlaceholderRun const&) const = default;
};

/// Resolves one line's placeholder cells into runs, as kitty's `screen_render_line_graphics` does:
/// a cell whose row, column or high id byte is unknown takes it from its left neighbour in the run.
/// @param cells The line's cells, left to right.
/// @return The runs, left to right.
[[nodiscard]] std::vector<PlaceholderRun> resolvePlaceholderRuns(std::span<PlaceholderCell const> cells);

} // namespace vtbackend::kitty_graphics
