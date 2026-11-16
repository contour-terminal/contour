// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Box.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/Rect.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

// Forward declarations
struct Theme;

/// A bounded drawing context for rendering components.
///
/// Canvas provides a coordinate-translated view into a Buffer for a specific
/// rectangular region. All drawing operations use coordinates relative to
/// the Canvas's top-left corner, and are automatically clipped to the
/// Canvas's bounds.
///
/// Canvases can be subdivided into smaller regions using subcanvas(),
/// enabling hierarchical layout of components.
class Canvas
{
  public:
    /// Constructs a Canvas for the given buffer region.
    /// @param buffer The buffer to draw into.
    /// @param area The region of the buffer this canvas represents (in buffer coordinates).
    /// @param theme The theme for styling.
    Canvas(Buffer& buffer, Rect area, Theme const& theme);

    // --- Drawing Operations ---

    /// Puts a single grapheme at the given position.
    /// @param row Row relative to this canvas (0-based).
    /// @param col Column relative to this canvas (0-based).
    /// @param grapheme UTF-8 grapheme cluster to draw.
    /// @param style Style to apply.
    void put(int row, int col, std::string_view grapheme, Style const& style);

    /// Writes a string starting at the given position.
    /// Handles wide characters automatically.
    /// @param row Row relative to this canvas (0-based).
    /// @param col Column relative to this canvas (0-based).
    /// @param text UTF-8 text to write.
    /// @param style Style to apply.
    /// @return Number of columns consumed.
    int putString(int row, int col, std::string_view text, Style const& style);

    /// Fills a rectangular region with a character.
    /// @param area Region to fill (relative to this canvas).
    /// @param ch Character to fill with (ASCII).
    /// @param style Style to apply.
    void fill(Rect area, char ch, Style const& style);

    /// Clears the entire canvas area.
    /// @param style Style to apply (background color).
    void clear(Style const& style = {});

    // --- Box Drawing ---

    /// Draws a box border within the given area.
    /// @param area The area for the box (relative to this canvas).
    /// @param border Border style to use.
    /// @param style Style for the border.
    void drawBox(Rect area, BorderStyle border, Style const& style);

    /// Draws a box border with a title.
    /// @param area The area for the box (relative to this canvas).
    /// @param border Border style to use.
    /// @param style Style for the border.
    /// @param title Title text to display in the top border.
    /// @param align Title alignment.
    void drawBox(Rect area,
                 BorderStyle border,
                 Style const& style,
                 std::string_view title,
                 TitleAlign align = TitleAlign::Left);

    /// Draws a horizontal line.
    /// @param row Row for the line.
    /// @param startCol Starting column.
    /// @param width Width in columns.
    /// @param ch Character to use (e.g., '-' or unicode box char).
    /// @param style Style to apply.
    void drawHLine(int row, int startCol, int width, std::string_view ch, Style const& style);

    /// Draws a vertical line.
    /// @param col Column for the line.
    /// @param startRow Starting row.
    /// @param height Height in rows.
    /// @param ch Character to use (e.g., '|' or unicode box char).
    /// @param style Style to apply.
    void drawVLine(int col, int startRow, int height, std::string_view ch, Style const& style);

    // --- Image Rendering ---

    /// @brief Draws a pre-encoded sixel image covering the specified cell region.
    ///
    /// The sixel data is stored in the buffer and will be emitted during screen flush.
    /// Cell content in the covered area is not rendered (the image takes precedence).
    ///
    /// @param row Top-left row (canvas-local, 0-based).
    /// @param col Top-left column (canvas-local, 0-based).
    /// @param columnSpan Number of terminal columns the image covers.
    /// @param lineSpan Number of terminal lines the image covers.
    /// @param encodedSixel Pre-encoded sixel data (without DCS framing).
    void drawImage(int row, int col, int columnSpan, int lineSpan, std::string_view encodedSixel);

    // --- Hyperlinks ---

    /// @brief Registers an OSC 8 hyperlink over a horizontal run of already-drawn cells.
    ///
    /// Call after the covered cells have been drawn; the framing is emitted around them at
    /// flush time. The link occupies no cells of its own, so it does not affect display
    /// widths or column arithmetic. An empty @p uri is ignored, and the run is clipped to
    /// this canvas so a subcanvas cannot register a link outside its own area.
    ///
    /// @param row Row (canvas-local, 0-based).
    /// @param col Starting column (canvas-local, 0-based).
    /// @param columnSpan Number of columns the link covers.
    /// @param uri Absolute URI, e.g. `file://host/home/me/src`.
    void addHyperlink(int row, int col, int columnSpan, std::string_view uri);

    // --- Row Marks ---

    /// @brief Anchors a zero-width raw sequence to the cell at (@p row, @p column).
    ///
    /// The inline screen writes @p sequence through TerminalOutput::writeRaw, at this cell's
    /// column, whenever it repaints a cell of this row from @p column up to (not including) the
    /// next mark's column: on the first frame, on a full redraw, and on every partial repaint
    /// that touches that segment. A row whose marks (column and sequence, in order) differ from
    /// the previous frame's repaints whole, so each of its marks is written again. Cells left of
    /// a row's first mark carry none. The fullscreen and fixed viewports ignore marks.
    ///
    /// Terminals carry zone state across rows: a repaint of a row with no mark of its own leaves
    /// that row's cells in whatever zone the terminal last saw. A component that needs per-cell
    /// zone correctness must therefore place a mark at column 0 of every row it can repaint.
    ///
    /// Marks sharing a column on one row: the later-registered one wins, and the earlier is never
    /// written. Never written either: marks on rows covered by an image, on rows beyond the
    /// content height, at a column at or beyond the width, or on a wide character's continuation
    /// column.
    ///
    /// Coordinates are translated like put(); a mark outside this canvas is dropped.
    ///
    /// @param row Row (canvas-local, 0-based).
    /// @param column Column (canvas-local, 0-based).
    /// @param sequence Raw bytes to emit; they must not move the cursor.
    void mark(int row, int column, std::string sequence);

    // --- Cursor ---

    /// Sets the cursor position (relative to this canvas).
    /// The cursor position is stored in the underlying buffer.
    /// @param row Row relative to this canvas.
    /// @param col Column relative to this canvas.
    void setCursor(int row, int col);

    /// Hides the cursor.
    void hideCursor();

    // --- Subcanvas ---

    /// Creates a sub-canvas for a region within this canvas.
    /// The returned canvas is clipped to both the given area and this canvas's bounds.
    /// @param area Region for the sub-canvas (relative to this canvas).
    /// @return A new Canvas for the specified region.
    [[nodiscard]] Canvas subcanvas(Rect area) const;

    // --- Properties ---

    /// Returns the width of this canvas in columns.
    [[nodiscard]] int width() const noexcept { return _area.width; }

    /// Returns the height of this canvas in rows.
    [[nodiscard]] int height() const noexcept { return _area.height; }

    /// Returns the size of this canvas.
    [[nodiscard]] Size size() const noexcept { return _area.size(); }

    /// Returns the canvas area (always starts at 0,0 in local coordinates).
    [[nodiscard]] Rect area() const noexcept
    {
        return Rect { .x = 0, .y = 0, .width = _area.width, .height = _area.height };
    }

    /// Returns the theme.
    [[nodiscard]] Theme const& theme() const noexcept { return _theme; }

    /// Returns the underlying buffer (for advanced use).
    [[nodiscard]] Buffer& buffer() noexcept { return _buffer; }

    [[nodiscard]] Buffer const& buffer() const noexcept { return _buffer; }

  private:
    Buffer& _buffer;
    Rect _area; ///< This canvas's region in buffer coordinates
    Theme const& _theme;

    /// Translates local row to buffer row.
    [[nodiscard]] int toBufferRow(int localRow) const noexcept { return _area.y + localRow; }

    /// Translates local column to buffer column.
    [[nodiscard]] int toBufferCol(int localCol) const noexcept { return _area.x + localCol; }

    /// Checks if local coordinates are within this canvas's bounds.
    [[nodiscard]] bool inBounds(int row, int col) const noexcept
    {
        return row >= 0 && row < _area.height && col >= 0 && col < _area.width;
    }

    /// Clips a local rectangle to this canvas's bounds.
    [[nodiscard]] Rect clipToLocal(Rect localArea) const noexcept;

    /// Translates a local rectangle to buffer coordinates.
    [[nodiscard]] Rect toBufferRect(Rect localArea) const noexcept;
};

/// @brief Renders text with highlighted match positions grapheme-by-grapheme.
///
/// Used by popup components to render fuzzy-matched text with highlighted characters.
/// Match positions are grapheme cluster indices (not byte offsets).
///
/// @param canvas The canvas to render to.
/// @param row Row to render at.
/// @param col Starting column.
/// @param text UTF-8 text to render.
/// @param normalStyle Style for non-matched characters.
/// @param matchStyle Style for matched characters.
/// @param matchPositions Grapheme indices of matched characters.
/// @return Number of columns consumed.
int putStringWithHighlights(Canvas& canvas,
                            int row,
                            int col,
                            std::string_view text,
                            Style const& normalStyle,
                            Style const& matchStyle,
                            std::vector<size_t> const& matchPositions);

} // namespace core::tui
