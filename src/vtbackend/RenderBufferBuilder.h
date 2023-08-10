#pragma once

#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>

#include <optional>

namespace terminal
{

/**
 * RenderBufferBuilder<Cell> renders the current screen state into a RenderBuffer.
 */
template <typename Cell>
class RenderBufferBuilder
{
  public:
    RenderBufferBuilder(Terminal const& terminal,
                        RenderBuffer& output,
                        line_offset base,
                        bool reverseVideo,
                        highlight_search_matches highlightSearchMatches,
                        InputMethodData inputMethodData,
                        std::optional<cell_location> theCursorPosition,
                        bool includeSelection);

    /// Renders a single grid cell.
    /// This call is guaranteed to be invoked sequencially, from top line
    /// to the bottom line and from left page margin to the right page margin,
    /// for every non-trivial line.
    /// A trivial line is rendered using renderTrivialLine().
    ///
    /// @see renderTrivialLine
    void renderCell(Cell const& cell, line_offset line, column_offset column);
    void startLine(line_offset line) noexcept;
    void endLine() noexcept;

    /// Renders a trivial line.
    ///
    /// This call is guaranteed to be invoked sequencially from page top
    /// to page bottom for every trivial line in order.
    /// As this function is only invoked for trivial lines, all other lines
    /// with their grid cells are to be rendered using renderCell().
    ///
    /// @see renderCell
    void renderTrivialLine(trivial_line_buffer const& lineBuffer, line_offset lineOffset);

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    [[nodiscard]] bool isCursorLine(line_offset line) const noexcept;

    [[nodiscard]] std::optional<RenderCursor> renderCursor() const;

    [[nodiscard]] static RenderCell makeRenderCellExplicit(color_palette const& colorPalette,
                                                           std::u32string graphemeCluster,
                                                           ColumnCount width,
                                                           cell_flags flags,
                                                           rgb_color fg,
                                                           rgb_color bg,
                                                           color ul,
                                                           line_offset line,
                                                           column_offset column);

    [[nodiscard]] static RenderCell makeRenderCellExplicit(color_palette const& colorPalette,
                                                           char32_t codepoint,
                                                           cell_flags flags,
                                                           rgb_color fg,
                                                           rgb_color bg,
                                                           color ul,
                                                           line_offset line,
                                                           column_offset column);

    /// Constructs a RenderCell for the given screen Cell.
    [[nodiscard]] static RenderCell makeRenderCell(color_palette const& colorPalette,
                                                   hyperlink_storage const& hyperlinks,
                                                   Cell const& cell,
                                                   rgb_color fg,
                                                   rgb_color bg,
                                                   line_offset line,
                                                   column_offset column);

    /// Constructs the final foreground/background colors to be displayed on the screen.
    ///
    /// This call takes cursor-position, hyperlink-states, selection, and reverse-video mode into account.
    [[nodiscard]] rgb_color_pair makeColorsForCell(cell_location,
                                                   cell_flags cellFlags,
                                                   color foregroundColor,
                                                   color backgroundColor) const noexcept;

    [[nodiscard]] RenderLine createRenderLine(trivial_line_buffer const& lineBuffer,
                                              line_offset lineOffset) const;

    [[nodiscard]] RenderAttributes createRenderAttributes(
        cell_location gridPosition, graphics_attributes graphicsAttributes) const noexcept;

    [[nodiscard]] bool tryRenderInputMethodEditor(cell_location screenPosition, cell_location gridPosition);

    ColumnCount renderUtf8Text(cell_location screenPosition,
                               graphics_attributes attributes,
                               std::string_view text,
                               bool allowMatchSearchPattern);

    template <typename T>
    void matchSearchPattern(T const& cellText);

    /// Tests if the given screen line offset does contain a cursor (either ANSI cursor or vi cursor, if
    /// shown) and returns false otherwise, which guarantees that no cursor is to be rendered
    /// on the given line offset.
    [[nodiscard]] bool gridLineContainsCursor(line_offset screenLineOffset) const noexcept;

    // clang-format off
    enum class State { Gap, Sequence };
    // clang-format on

    RenderBuffer& _output;
    Terminal const& _terminal;
    std::optional<cell_location> _cursorPosition;
    line_offset _baseLine;
    bool _reverseVideo;
    highlight_search_matches _highlightSearchMatches;
    InputMethodData _inputMethodData;
    bool _includeSelection;
    ColumnCount _inputMethodSkipColumns = ColumnCount(0);

    int _prevWidth = 0;
    bool _prevHasCursor = false;
    line_offset _lineNr = line_offset(0);
    bool _useCursorlineColoring = false;

    // Offset into the search pattern that has been already matched.
    size_t _searchPatternOffset = 0;
};

} // namespace terminal
