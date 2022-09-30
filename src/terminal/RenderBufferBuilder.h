#pragma once

#include <terminal/RenderBuffer.h>
#include <terminal/Terminal.h>

#include <optional>

namespace terminal
{

enum class HighlightSearchMatches
{
    No,
    Yes
};

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
                        InputMethodData inputMethodData);

    /// Renders a single grid cell.
    /// This call is guaranteed to be invoked sequencially, from top line
    /// to the bottom line and from left page margin to the right page margin,
    /// for every non-trivial line.
    /// A trivial line is rendered using renderTrivialLine().
    ///
    /// @see renderTrivialLine
    void renderCell(Cell const& _cell, LineOffset _line, ColumnOffset _column);
    void startLine(LineOffset _line) noexcept;
    void endLine() noexcept;

    /// Renders a trivial line.
    ///
    /// This call is guaranteed to be invoked sequencially from page top
    /// to page bottom for every trivial line in order.
    /// As this function is only invoked for trivial lines, all other lines
    /// with their grid cells are to be rendered using renderCell().
    ///
    /// @see renderCell
    void renderTrivialLine(TrivialLineBuffer const& _lineBuffer, LineOffset _lineNo);

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    [[nodiscard]] std::optional<RenderCursor> renderCursor() const;

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                                           std::u32string graphemeCluster,
                                                           ColumnCount width,
                                                           CellFlags flags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset _line,
                                                           ColumnOffset _column);

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                                           char32_t codepoint,
                                                           CellFlags flags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset _line,
                                                           ColumnOffset _column);

    /// Constructs a RenderCell for the given screen Cell.
    [[nodiscard]] static RenderCell makeRenderCell(ColorPalette const& _colorPalette,
                                                   HyperlinkStorage const& _hyperlinks,
                                                   Cell const& _cell,
                                                   RGBColor fg,
                                                   RGBColor bg,
                                                   LineOffset _line,
                                                   ColumnOffset _column);

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

    ColumnCount renderUtf8Text(CellLocation screenPosition,
                               GraphicsAttributes attributes,
                               std::string_view text,
                               bool allowMatchSearchPattern);

    template <typename T>
    void matchSearchPattern(T const& cellText);

    // clang-format off
    enum class State { Gap, Sequence };
    // clang-format on

    RenderBuffer& output;
    Terminal const& terminal;
    CellLocation cursorPosition;
    LineOffset baseLine;
    bool reverseVideo;
    HighlightSearchMatches _highlightSearchMatches;
    InputMethodData _inputMethodData;
    ColumnCount _inputMethodSkipColumns = ColumnCount(0);

    int prevWidth = 0;
    bool prevHasCursor = false;
    State state = State::Gap;
    LineOffset lineNr = LineOffset(0);
    bool isNewLine = false;

    // Offset into the search pattern that has been already matched.
    size_t searchPatternOffset = 0;
};

} // namespace terminal
