// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Cell.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

class TerminalOutput;

/// @brief An image region stored in the buffer.
///
/// Represents a pre-encoded sixel image covering a rectangular region of terminal cells.
struct ImageRegion
{
    Rect cellArea;               ///< Which cells this image covers (buffer coordinates).
    std::string encodedSixel;    ///< Pre-encoded sixel data (without DCS framing).
    std::size_t contentHash = 0; ///< Hash of encodedSixel for fast diff comparison.
};

/// @brief A hyperlink region stored in the buffer.
///
/// The out-of-band counterpart to ImageRegion. Covered cells hold ordinary graphemes and
/// styles; only the OSC 8 framing is stored here and emitted around them at flush time.
/// Keeping the URI off the cell is what makes a hyperlink zero-width by construction, so
/// display-width and column hit-testing arithmetic never has to know about it.
///
/// A region spans a whole run, which is what keeps a gradient-coloured path — drawn as one
/// Cell per grapheme cluster — a single logical link rather than N adjacent ones.
struct HyperlinkRegion
{
    Rect cellArea;      ///< Which cells this link covers (buffer coordinates).
    std::string uri;    ///< Absolute URI, e.g. "file://host/home/me/src".
    std::string linkId; ///< Stable OSC 8 `id=` derived from uri (empty = emit no id).
};

/// @brief A zero-width raw sequence anchored to a buffer cell.
///
/// Out-of-band like ImageRegion and HyperlinkRegion: the sequence occupies no cell and costs
/// no cursor column. The inline screen re-emits it whenever it repaints a cell between this
/// mark's column and the next mark's on the same row, which is what keeps a terminal that tags
/// each written cell with the state in force (such as an OSC 133 semantic zone) correct across
/// partial repaints.
struct RowMark
{
    int row;              ///< Buffer row (0-based).
    int column;           ///< Buffer column (0-based) the mark's segment starts at.
    std::string sequence; ///< Raw bytes written through TerminalOutput::writeRaw.
};

/// A 2D grid of terminal cells representing a virtual screen buffer.
///
/// The buffer stores cells in row-major order and provides operations
/// for reading, writing, and clearing cells. Coordinates are 0-based.
///
/// The buffer also tracks cursor position and visibility state for
/// final cursor placement after rendering.
class CORE_TUI_CORE_API Buffer
{
  public:
    /// Constructs an empty buffer (0x0).
    Buffer() = default;

    /// Constructs a buffer with the given dimensions.
    /// @param rows Number of rows.
    /// @param cols Number of columns.
    Buffer(int rows, int cols);

    // --- Dimensions ---

    /// Returns the number of rows.
    [[nodiscard]] int rows() const noexcept { return _rows; }

    /// Returns the number of columns.
    [[nodiscard]] int cols() const noexcept { return _cols; }

    /// Returns the size as a Size struct.
    [[nodiscard]] Size size() const noexcept { return { .width = _cols, .height = _rows }; }

    /// Returns true if the buffer has no cells.
    [[nodiscard]] bool empty() const noexcept { return _cells.empty(); }

    /// Resizes the buffer to new dimensions.
    /// Existing content is preserved where possible; new cells are cleared.
    /// @param rows New number of rows.
    /// @param cols New number of columns.
    void resize(int rows, int cols);

    // --- Cell Access ---

    /// Returns true if the given coordinates are within bounds.
    [[nodiscard]] bool inBounds(int row, int col) const noexcept
    {
        return row >= 0 && row < _rows && col >= 0 && col < _cols;
    }

    /// Returns the cell at @p position, or nullptr outside the buffer.
    ///
    /// The one accessor: a read or write through it outside the buffer is a null check and nothing
    /// else, never an exception, so the headless TUI builds with exceptions off.
    /// @param position The cell's column (x) and row (y), both 0-based.
    /// @return The cell, or nullptr when @p position is out of bounds.
    [[nodiscard]] Cell* cellAt(Point position) noexcept;

    /// Returns the cell at @p position, or nullptr outside the buffer.
    /// @param position The cell's column (x) and row (y), both 0-based.
    /// @return The cell, or nullptr when @p position is out of bounds.
    [[nodiscard]] Cell const* cellAt(Point position) const noexcept;

    // --- Drawing Operations ---

    /// Clears all cells to spaces with the given style.
    void clear(Style const& style = {});

    /// Clears a rectangular region to spaces with the given style.
    void clearRect(Rect area, Style const& style = {});

    /// Writes a string starting at (row, col).
    /// Handles wide characters by marking continuation cells.
    /// @param row Starting row (0-based).
    /// @param col Starting column (0-based).
    /// @param text UTF-8 text to write.
    /// @param style Style to apply.
    /// @param maxWidth Maximum number of display columns the caller is permitted to write,
    ///                 measured from @p col. A grapheme cluster is skipped entirely if it (or
    ///                 its continuation cells) would exceed this budget — this prevents a wide
    ///                 cluster at the edge of a sub-region from spilling into adjacent cells.
    ///                 Defaults to the full buffer width (no extra limit).
    /// @return Number of columns consumed.
    int putString(int row,
                  int col,
                  std::string_view text,
                  Style const& style,
                  int maxWidth = std::numeric_limits<int>::max());

    /// Fills a rectangular region with a character.
    /// @param area The region to fill.
    /// @param ch Character to fill with (ASCII).
    /// @param style Style to apply.
    void fill(Rect area, char ch, Style const& style);

    // --- Cursor State ---

    /// Sets the cursor position.
    void setCursor(int row, int col) noexcept;

    /// Returns the cursor position.
    [[nodiscard]] Point cursor() const noexcept { return _cursor; }

    /// Sets cursor visibility.
    void setCursorVisible(bool visible) noexcept { _cursorVisible = visible; }

    /// Returns true if the cursor should be visible.
    [[nodiscard]] bool cursorVisible() const noexcept { return _cursorVisible; }

    // --- Iteration ---

    /// Raw access to cell data (for diffing).
    [[nodiscard]] std::vector<Cell> const& cells() const noexcept { return _cells; }

    // --- Image Regions ---

    /// @brief Adds an image region to the buffer.
    /// @param cellArea The cell area the image covers (in buffer coordinates).
    /// @param encodedSixel Pre-encoded sixel data (without DCS framing).
    void addImage(Rect cellArea, std::string encodedSixel);

    /// @brief Returns all image regions.
    [[nodiscard]] std::span<ImageRegion const> images() const noexcept;

    /// @brief Clears all image regions.
    void clearImages() noexcept;

    // --- Hyperlink Regions ---

    /// @brief Registers an OSC 8 hyperlink covering @p cellArea.
    ///
    /// Call after the covered cells have been drawn. Empty URIs are ignored, so callers can
    /// pass the result of a URI builder unconditionally.
    ///
    /// @param cellArea The cell run the link covers (in buffer coordinates).
    /// @param uri Absolute URI, e.g. `file://host/home/me/src`.
    void addHyperlink(Rect cellArea, std::string uri);

    /// @brief Returns all hyperlink regions, in registration order.
    [[nodiscard]] std::span<HyperlinkRegion const> hyperlinks() const noexcept;

    /// @brief Clears all hyperlink regions.
    void clearHyperlinks() noexcept;

    // --- Row Marks ---

    /// @brief Anchors a zero-width raw sequence to the cell at (@p row, @p column).
    /// @param row Buffer row (0-based).
    /// @param column Buffer column (0-based).
    /// @param sequence Raw bytes to emit; they must not move the cursor.
    void addMark(int row, int column, std::string sequence);

    /// @brief Returns all row marks, in registration order.
    [[nodiscard]] std::span<RowMark const> marks() const noexcept;

    /// @brief Clears all row marks.
    void clearMarks() noexcept;

    /// Writes buffer content to a terminal output stream.
    ///
    /// Iterates rows/cells and emits text with styling, handling wide characters
    /// and continuation cells. Uses carriageReturn + clearToEndOfLine per row.
    /// @param out The terminal output to write to.
    void writeTo(TerminalOutput& out) const;

  private:
    std::vector<Cell> _cells;
    int _rows = 0;
    int _cols = 0;
    Point _cursor { .x = 0, .y = 0 };
    bool _cursorVisible = true;
    std::vector<ImageRegion> _images;
    std::vector<HyperlinkRegion> _hyperlinks;
    std::vector<RowMark> _marks;

    /// Converts (row, col) to a linear index.
    [[nodiscard]] std::size_t index(int row, int col) const noexcept
    {
        return (static_cast<std::size_t>(row) * static_cast<std::size_t>(_cols))
               + static_cast<std::size_t>(col);
    }
};

} // namespace core::tui
