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
 * RenderBufferBuilder renders the current screen state into a RenderBuffer.
 */
class RenderBufferBuilder
{
  public:
    /// @param colorLookupTable The color mode (DECSTGLT) that applies to the screen being rendered.
    ///                         Status-line screens are host-owned chrome and pass AnsiSgr, so an
    ///                         application's DECATC assignments cannot repaint them.
    /// @param screen The screen being rendered. It is NOT always the terminal's current screen: a
    ///               status line and a non-displayed page are rendered through the same builder, so
    ///               anything that needs to look at neighbouring cells must go through this rather
    ///               than re-resolving via Terminal::currentScreen().
    RenderBufferBuilder(Terminal const& terminal,
                        Screen const& screen,
                        RenderBuffer& output,
                        LineOffset base,
                        bool reverseVideo,
                        ColorLookupTable colorLookupTable,
                        HighlightSearchMatches highlightSearchMatches,
                        InputMethodData inputMethodData,
                        std::optional<CellLocation> theCursorPosition,
                        bool includeSelection);

    /// Renders a single grid cell.
    /// This call is guaranteed to be invoked sequentially, from top line
    /// to the bottom line and from left page margin to the right page margin,
    /// for every non-trivial line.
    /// A trivial line is rendered using renderTrivialLine().
    ///
    /// @see renderTrivialLine
    void renderCell(ConstCellProxy cell, LineOffset line, ColumnOffset column);
    void startLine(LineOffset line, LineFlags flags) noexcept;
    void endLine() noexcept;

    /// Renders a trivial line.
    ///
    /// This call is guaranteed to be invoked sequentially from page top
    /// to page bottom for every trivial line in order.
    /// As this function is only invoked for trivial lines, all other lines
    /// with their grid cells are to be rendered using renderCell().
    ///
    /// @see renderCell
    void renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                           LineOffset lineOffset,
                           LineFlags flags,
                           std::u32string_view textOverride = {});

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    [[nodiscard]] bool isCursorLine(LineOffset line) const noexcept;

    [[nodiscard]] std::optional<RenderCursor> renderCursor() const;

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                           std::u32string graphemeCluster,
                                                           ColumnCount width,
                                                           CellFlags flags,
                                                           LineFlags lineFlags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset line,
                                                           ColumnOffset column);

    [[nodiscard]] static RenderCell makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                           char32_t codepoint,
                                                           CellFlags flags,
                                                           LineFlags lineFlags,
                                                           RGBColor fg,
                                                           RGBColor bg,
                                                           Color ul,
                                                           LineOffset line,
                                                           ColumnOffset column);

    /// Constructs a RenderCell for the given screen Cell.
    [[nodiscard]] static RenderCell makeRenderCell(ColorPalette const& colorPalette,
                                                   HyperlinkStorage const& hyperlinks,
                                                   ConstCellProxy cell,
                                                   LineFlags lineFlags,
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
                                              LineOffset lineOffset,
                                              std::u32string_view textOverride = {}) const;

    [[nodiscard]] RenderAttributes createRenderAttributes(
        CellLocation gridPosition, GraphicsAttributes graphicsAttributes) const noexcept;

    [[nodiscard]] bool tryRenderInputMethodEditor(CellLocation screenPosition, CellLocation gridPosition);

    /// Emits one render cell per column for text taken from the grid.
    ///
    /// The grid has already decided where each cell begins and how wide it is, so this only counts
    /// the columns off. Its counterpart @c renderUtf8Text is for text whose cell boundaries nobody
    /// has determined yet, and measures them; using that one on grid text is what let the renderer
    /// and the grid disagree about a cluster's width (GitHub #1752).
    ///
    /// @param screenPosition Where the first cell goes.
    /// @param textAttributes The line's uniform SGR.
    /// @param text One codepoint per column, as @c Line::trivialBuffer produces it.
    void renderGridText(CellLocation screenPosition,
                        GraphicsAttributes textAttributes,
                        std::u32string_view text);

    /// Emits render cells for text whose cell boundaries are not yet known -- the IME preedit
    /// string -- segmenting it into grapheme clusters and measuring each.
    /// @see renderGridText for text that comes from the grid.
    /// @return How many columns were covered.
    ColumnCount renderUtf8Text(CellLocation screenPosition,
                               GraphicsAttributes attributes,
                               std::string_view text);

    /// The display column a grid column draws at.
    ///
    /// DECDWL (@c LineFlag::DoubleWidth) doubles every cell's width, so cell N of such a line draws
    /// at display column 2N. Lives here because every emitter needs it and four copies of the
    /// expression is four places to miss when the mapping grows a case.
    [[nodiscard]] ColumnOffset displayColumn(ColumnOffset column) const noexcept
    {
        return _currentLineFlags.test(LineFlag::DoubleWidth) ? column * 2 : column;
    }

    template <typename T>
    void matchSearchPattern(T const& cellText);

    /// Tests if the given screen line offset does contain a cursor (either ANSI cursor or vi cursor, if
    /// shown) and returns false otherwise, which guarantees that no cursor is to be rendered
    /// on the given line offset.
    [[nodiscard]] bool gridLineContainsCursor(LineOffset screenLineOffset) const noexcept;

    // clang-format off
    enum class State : uint8_t { Gap, Sequence };
    // clang-format on

    gsl::not_null<RenderBuffer*> _output;
    gsl::not_null<Terminal const*> _terminal;
    gsl::not_null<Screen const*> _screen;
    std::optional<CellLocation> _cursorPosition;
    LineOffset _baseLine;
    bool _reverseVideo;
    ColorLookupTable _colorLookupTable;
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

    // Current line flags being rendered.
    LineFlags _currentLineFlags = LineFlag::None;
};

} // namespace vtbackend
