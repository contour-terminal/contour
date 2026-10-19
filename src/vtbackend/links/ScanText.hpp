// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

class Grid;
class Line;

/// Whether a physical row continues the logical line begun on the row above.
///
/// A logical line is what was actually written; the rows below its head exist only because the window
/// happened to be too narrow. Links are matched against logical lines, so a URL broken across two rows is
/// still one link.
enum class LineContinuation : uint8_t
{
    No = 0,  ///< This row starts a new logical line.
    Yes = 1, ///< This row is a wrapped continuation of the row above.
};

/// The most cells one ScanText holds: about three 60×200 pages. It bounds every regex input, and so the time
/// and memory one match takes. No engine's stack depends on it (§13 of the link-detection design): libstdc++
/// runs the link patterns on its breadth-first executor, whose stack is bounded by the pattern; libc++
/// matches iteratively; MSVC gives up with `error_stack`, which the matcher catches.
inline constexpr std::size_t MaxScanCells = 36864;

/// One physical row as the link scanner reads it (§4.1).
struct ScanRow
{
    /// What the patterns run on: the row's text with Unicode separators, wide punctuation and OSC 8 cells
    /// blanked to ASCII spaces. Byte for byte as long as @ref original, so one offset addresses both.
    std::string text;
    /// The row as printed; empty when it equals @ref text (nothing was blanked).
    std::string original;
    /// One entry per cell: the byte in @ref text its content begins at. A wide character's continuation cell
    /// repeats its leader's entry, so the entries never decrease.
    std::vector<uint32_t> cellByteStart;
    LineOffset line;                                      ///< The grid line this row was read from.
    LineContinuation continuation = LineContinuation::No; ///< Whether it continues the row above.
};

/// Reads one grid row for the link scanner.
/// @param line The row.
/// @param offset Its grid line offset.
/// @param continuation Whether it continues the logical line above.
/// @return The row's scan text and cell map.
[[nodiscard]] ScanRow makeScanRow(Line const& line, LineOffset offset, LineContinuation continuation);

/// A row of plain text, one cell per codepoint, blanked as makeScanRow() blanks a grid row (separators and
/// wide punctuation; plain text has no OSC 8 cells): for tests and the settings page's live preview, which
/// have text but no grid.
/// @param text UTF-8 text.
/// @param offset The grid line to attribute it to.
/// @param continuation Whether it continues the row above.
/// @return The row.
[[nodiscard]] ScanRow makePlainScanRow(std::string_view text,
                                       LineOffset offset = LineOffset(0),
                                       LineContinuation continuation = LineContinuation::No);

/// Whether a logical line goes on past one end of a ScanText (the text was clipped to the rows at hand or
/// to the budget). A match touching a clipped end is dropped rather than reported truncated.
enum class ScanEdge : uint8_t
{
    Whole = 0, ///< The logical line ends here.
    Clipped,   ///< It goes on beyond what this ScanText holds.
};

/// Both ends of a ScanText.
struct ScanClip
{
    ScanEdge front = ScanEdge::Whole; ///< Above the first row.
    ScanEdge back = ScanEdge::Whole;  ///< Below the last row.

    bool operator==(ScanClip const&) const = default;
};

/// One logical line, consecutive rows joined across wraps, as the patterns see it.
class ScanText
{
  public:
    /// Joins @p rows, which are consecutive and one logical line (the first may be a continuation whose head
    /// was clipped away).
    /// @param rows The rows, top first; at least one (an empty span is a precondition violation).
    /// @param clip Which ends were clipped.
    ScanText(std::span<ScanRow const> rows, ScanClip clip);

    /// What the patterns run on.
    [[nodiscard]] std::string_view text() const noexcept { return _text; }
    /// The bytes [@p begin, @p end) as printed.
    [[nodiscard]] std::string_view original(std::size_t begin, std::size_t end) const noexcept;
    /// How many rows it joins.
    [[nodiscard]] std::size_t rowCount() const noexcept { return _rowLines.size(); }
    /// The grid line row @p row was read from.
    [[nodiscard]] LineOffset lineOf(std::size_t row) const noexcept { return _rowLines[row]; }
    /// How many cells row @p row has.
    [[nodiscard]] std::size_t columnsOf(std::size_t row) const noexcept;
    /// Which ends were clipped.
    [[nodiscard]] ScanClip clip() const noexcept { return _clip; }
    /// The first cell whose content begins at or after @p byte.
    [[nodiscard]] std::size_t firstCellAt(std::size_t byte) const noexcept;
    /// The last cell whose content begins before @p byteEnd, a wide character's continuation cell included.
    [[nodiscard]] std::size_t lastCellBefore(std::size_t byteEnd) const noexcept;
    /// The row and column of @p cell, an index over the whole logical line.
    [[nodiscard]] ScanPosition positionOf(std::size_t cell) const noexcept;
    /// The cell index of @p position.
    [[nodiscard]] std::size_t cellIndexOf(ScanPosition position) const noexcept;

  private:
    std::string _text;
    std::string _original; ///< Empty when no row was blanked.
    std::vector<uint32_t> _cellByteStart;
    std::vector<std::size_t> _rowCellEnds; ///< Cumulative and exclusive, one per row.
    std::vector<LineOffset> _rowLines;
    ScanClip _clip;
};

/// Groups @p rows into logical lines, joining wrapped continuations onto the row that heads them. A logical
/// line longer than @p maxCells is cut into several ScanTexts, clipped where they were cut.
/// @param rows Rows in ascending, consecutive grid-line order.
/// @param continuesBelow Whether the row after the last one continues it.
/// @param maxCells The budget of one ScanText (@ref MaxScanCells).
/// @return One or more ScanTexts per logical line, top first.
[[nodiscard]] std::vector<ScanText> joinLogicalLines(std::span<ScanRow const> rows,
                                                     LineContinuation continuesBelow,
                                                     std::size_t maxCells = MaxScanCells);

/// The rows a scan of a stretch of grid reads: the stretch itself, extended to whole logical lines.
struct ScanExtent
{
    LineOffset first;                                       ///< The topmost row to read.
    LineOffset last;                                        ///< The bottommost row to read.
    LineContinuation continuesBelow = LineContinuation::No; ///< Whether the row after @ref last continues it.
};

/// Extends the rows [@p first, @p last] to whole logical lines: up over the rows that continue into the
/// first, down over the ones that continue the last, so a link wrapped across an edge is matched in full. It
/// goes no further than @p maxCells worth of rows either way, beyond which a ScanText cuts the text anyway.
/// @param grid The grid to walk.
/// @param first The topmost row of the stretch.
/// @param last The bottommost row of the stretch.
/// @param maxCells The budget in cells (@ref MaxScanCells); at least one row is always allowed.
/// @return The extended rows, and whether the logical line goes on below them.
[[nodiscard]] ScanExtent scanExtentOf(Grid const& grid,
                                      LineOffset first,
                                      LineOffset last,
                                      std::size_t maxCells = MaxScanCells);

/// Reads the rows [@p first, @p last] of @p grid for the scanner, each marked as a continuation when the grid
/// wrapped it.
/// @param grid The grid to read.
/// @param first The topmost row.
/// @param last The bottommost row, at or below @p first.
/// @return One ScanRow per grid row, top first, consecutive.
[[nodiscard]] std::vector<ScanRow> scanRowsOf(Grid const& grid, LineOffset first, LineOffset last);

} // namespace vtbackend
