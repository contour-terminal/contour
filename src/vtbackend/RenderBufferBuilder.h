// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>

#include <gsl/pointers>

#include <optional>

namespace vtbackend
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
                        LineOffset base,
                        bool reverseVideo,
                        HighlightSearchMatches highlightSearchMatches,
                        InputMethodData inputMethodData,
                        std::optional<CellLocation> theCursorPosition,
                        bool includeSelection);

    /// Renders a single grid cell.
    /// This call is guaranteed to be invoked sequencially, from top line
    /// to the bottom line and from left page margin to the right page margin,
    /// for every non-trivial line.
    /// A trivial line is rendered using renderTrivialLine().
    ///
    /// @see renderTrivialLine
    void renderCell(Cell const& cell, LineOffset line, ColumnOffset column);
    void startLine(LineOffset line) noexcept;
    void endLine() noexcept;

    /// Renders a trivial line.
    ///
    /// This call is guaranteed to be invoked sequencially from page top
    /// to page bottom for every trivial line in order.
    /// As this function is only invoked for trivial lines, all other lines
    /// with their grid cells are to be rendered using renderCell().
    ///
    /// @see renderCell
    void renderTrivialLine(TrivialLineBuffer const& lineBuffer, LineOffset lineOffset);

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    [[nodiscard]] bool isCursorLine(LineOffset line) const noexcept;

    [[nodiscard]] std::optional<RenderCursor> renderCursor() const;

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                           std::u32string graphemeCluster,
                                                           ColumnCount width,
                                                           CellFlags flags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset line,
                                                           ColumnOffset column);

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                           char32_t codepoint,
                                                           CellFlags flags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset line,
                                                           ColumnOffset column);

    /// Constructs a RenderCell for the given screen Cell.
    [[nodiscard]] static RenderCell makeRenderCell(ColorPalette const& colorPalette,
                                                   HyperlinkStorage const& hyperlinks,
                                                   Cell const& cell,
                                                   RGBColor fg,
                                                   RGBColor bg,
                                                   LineOffset line,
                                                   ColumnOffset column);

    /// Constructs the final foreground/background colors to be displayed on the screen.
    ///
    /// This call takes cursor-position, hyperlink-states, selection, and reverse-video mode into account.
    [[nodiscard]] RGBColorPair makeColorsForCell(CellLocation,
                                                 CellFlags cellFlags,
                                                 Color foregroundColor,
                                                 Color backgroundColor) const noexcept;

    [[nodiscard]] RenderLine createRenderLine(TrivialLineBuffer const& lineBuffer,
                                              LineOffset lineOffset) const;

    [[nodiscard]] RenderAttributes createRenderAttributes(
        CellLocation gridPosition, GraphicsAttributes graphicsAttributes) const noexcept;

    [[nodiscard]] bool tryRenderInputMethodEditor(CellLocation screenPosition, CellLocation gridPosition);

    ColumnCount renderUtf8Text(CellLocation screenPosition,
                               GraphicsAttributes attributes,
                               std::string_view text,
                               bool allowMatchSearchPattern);

    template <typename T>
    void matchSearchPattern(T const& cellText);

    /// Tests if the given screen line offset does contain a cursor (either ANSI cursor or vi cursor, if
    /// shown) and returns false otherwise, which guarantees that no cursor is to be rendered
    /// on the given line offset.
    [[nodiscard]] bool gridLineContainsCursor(LineOffset screenLineOffset) const noexcept;

    // clang-format off
    enum class State { Gap, Sequence };
    // clang-format on

    gsl::not_null<RenderBuffer*> _output;
    gsl::not_null<Terminal const*> _terminal;
    std::optional<CellLocation> _cursorPosition;
    LineOffset _baseLine;
    bool _reverseVideo;
    HighlightSearchMatches _highlightSearchMatches;
    InputMethodData _inputMethodData;
    bool _includeSelection;
    ColumnCount _inputMethodSkipColumns = ColumnCount(0);

    int _prevWidth = 0;
    bool _prevHasCursor = false;
    LineOffset _lineNr = LineOffset(0);
    bool _useCursorlineColoring = false;

    // Offset into the search pattern that has been already matched.
    size_t _searchPatternOffset = 0;
};

} // namespace vtbackend
