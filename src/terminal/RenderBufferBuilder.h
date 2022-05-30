#pragma once

#include <terminal/RenderBuffer.h>
#include <terminal/Terminal.h>

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
    RenderBufferBuilder(Terminal const& terminal, RenderBuffer& output);

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
    void renderTrivialLine(TriviallyStyledLineBuffer const& _lineBuffer, LineOffset _lineNo);

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    std::optional<RenderCursor> renderCursor() const;

    static RenderCell makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                             std::u32string graphemeCluster,
                                             ColumnCount width,
                                             CellFlags flags,
                                             RGBColor fg,
                                             RGBColor bg,
                                             Color ul,
                                             LineOffset _line,
                                             ColumnOffset _column);

    static RenderCell makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                             char32_t codepoint,
                                             CellFlags flags,
                                             RGBColor fg,
                                             RGBColor bg,
                                             Color ul,
                                             LineOffset _line,
                                             ColumnOffset _column);

    /// Constructs a RenderCell for the given screen Cell.
    static RenderCell makeRenderCell(ColorPalette const& _colorPalette,
                                     HyperlinkStorage const& _hyperlinks,
                                     Cell const& _cell,
                                     RGBColor fg,
                                     RGBColor bg,
                                     LineOffset _line,
                                     ColumnOffset _column);

    /// Constructs the final foreground/background colors to be displayed on the screen.
    ///
    /// This call takes cursor-position, hyperlink-states, selection, and reverse-video mode into account.
    std::tuple<RGBColor, RGBColor> makeColorsForCell(CellLocation,
                                                     CellFlags cellFlags,
                                                     Color foregroundColor,
                                                     Color backgroundColor);

    // clang-format off
    enum class State { Gap, Sequence };
    // clang-format on

    RenderBuffer& output;
    Terminal const& terminal;
    CellLocation cursorPosition;

    bool reverseVideo = terminal.isModeEnabled(terminal::DECMode::ReverseVideo);
    int prevWidth = 0;
    bool prevHasCursor = false;
    State state = State::Gap;
    LineOffset lineNr = LineOffset(0);
    bool isNewLine = false;
};

} // namespace terminal
