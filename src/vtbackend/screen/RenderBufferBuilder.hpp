// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <gsl/pointers>

#include <optional>
#include <span>
#include <vector>

namespace vtbackend
{

/// What one frame knows about the links on the screen a RenderBufferBuilder renders: which are under the
/// pointer, and where the detected ones are. The caller decides it per screen, as only the page on display
/// has the pointer over it and detected links on it; a status line gets none of it.
struct FrameLinks
{
    /// The OSC 8 link the pointer is over (Terminal::hyperlinkIdUnderPointer()); none for a screen the
    /// pointer is not over.
    HyperlinkId hoveredHyperlink {};
    /// The links detected on the screen rendered (Terminal::detectedLinks()); nullptr for a screen whose rows
    /// the index does not name. Valid for the pass, which holds the terminal lock.
    LinkIndex const* detectedLinks = nullptr;
    /// The detected link the pointer is over (Terminal::detectedLinkAt() at the pointer's cell); none for a
    /// screen the pointer is not over.
    LinkId hoveredDetectedLink {};
};

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
    /// @param links The links of this frame on @p screen: the ones under the pointer, which the builder draws
    ///              in the hover role, and the detected ones; none of either for a screen that has none.
    /// @param display Which display @p screen is drawn on: the main display maps its rows through the
    ///                viewport, a status line draws its rows as they are.
    RenderBufferBuilder(Terminal const& terminal,
                        Screen const& screen,
                        RenderBuffer& output,
                        LineOffset base,
                        bool reverseVideo,
                        ColorLookupTable colorLookupTable,
                        HighlightSearchMatches highlightSearchMatches,
                        InputMethodData inputMethodData,
                        std::optional<CellLocation> theCursorPosition,
                        bool includeSelection,
                        FrameLinks links,
                        RenderedDisplay display);

    /// Why the search's query could not be matched while this pass painted its highlights, so the terminal
    /// can end the query (R-f): the builder reads the terminal as const, and cannot.
    /// @return SearchPatternError::TooComplex where the regex engine gave up; nothing otherwise.
    [[nodiscard]] std::optional<SearchPatternError> searchFailure() const noexcept { return _searchFailure; }

    /// Renders grid row @p line as host chrome copying it: its own colours, SGR and hyperlinks, but no
    /// cursor, selection, search match or yank highlight -- those describe the row where it really is.
    ///
    /// For the sticky command header, which draws it over a band of its own: only the columns that show
    /// are copied (Line::trimmedColumns()), and a cell on the page's default background keeps it, so the
    /// band shows through. The row's rendition (DECDWL/DECDHL) is not copied, and an OSC 66 block shows
    /// only the part on this row.
    ///
    /// Appended to @p output, one cell per copied column, left to right, as one shaping group; nothing for
    /// a blank row. Appended rather than returned, so the header copies into storage its frame keeps.
    ///
    /// @param output Where the cells go, after whatever it already holds.
    /// @param terminal The terminal whose palette, colour mode and blink phase apply. A link on the row is
    ///                 drawn at rest: the header is no hover target.
    /// @param line The grid row to copy.
    /// @param detectedLinks The links detected on @p line (currentSpansOn()), which take the role its OSC 8
    ///                      links take; none for a row written since its scan. The header's row lies above
    ///                      the viewport, so it has them only while it lies inside detection's scan and
    ///                      retention window (LinkDetectorTuning::scanMarginPages, retentionMarginPages): a
    ///                      header copied from further up shows its links undecorated.
    /// @param screenRow The screen row the copy is drawn on.
    /// @param columns How many columns to copy at most; a wide glyph that would straddle the limit is
    ///                dropped whole.
    /// @see Terminal::fillStickyHeader
    static void renderDetachedLine(std::vector<RenderCell>& output,
                                   Terminal const& terminal,
                                   Line const& line,
                                   std::span<LinkRowSpan const> detectedLinks,
                                   LineOffset screenRow,
                                   ColumnCount columns);

    /// Renders a single grid cell.
    /// This call is guaranteed to be invoked sequentially, from top line
    /// to the bottom line and from left page margin to the right page margin,
    /// for every non-trivial line.
    /// A trivial line is rendered using renderTrivialLine().
    ///
    /// @see renderTrivialLine
    void renderCell(ConstCellProxy cell, LineOffset line, ColumnOffset column);
    /// Not noexcept: the cursor-line decision it makes goes through the viewport's coordinate
    /// translation, which builds the fold projection lazily and therefore allocates.
    void startLine(LineOffset line, LineFlags flags, ContextId contextId);
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
                           ContextId contextId,
                           std::u32string_view textOverride = {});

    /// This call is guaranteed to be invoked when the the full page has been rendered.
    void finish() noexcept {}

  private:
    /// Not noexcept, for the reason startLine() gives.
    [[nodiscard]] bool isCursorLine(LineOffset line) const;

    [[nodiscard]] std::optional<RenderCursor> renderCursor() const;

    /// The grid position of the cell drawn at @p screenPosition: through the viewport on the main display,
    /// the same position on a status line, which has no viewport.
    ///
    /// Not noexcept, for the reason startLine() gives.
    /// @param screenPosition Where the cell is drawn, relative to the screen's first row.
    /// @return Where the cell is in the screen's grid.
    [[nodiscard]] CellLocation gridPositionOf(CellLocation screenPosition) const;

    /// The row grid line @p line is drawn on: the inverse of gridPositionOf().
    ///
    /// Not noexcept, for the reason startLine() gives.
    /// @param line A line of the screen's grid.
    /// @return The row it is drawn on, relative to the screen's first row.
    [[nodiscard]] LineOffset screenLineOf(LineOffset line) const;

    /// Whether grid line @p line is drawn at all: inside the viewport on the main display, inside the
    /// screen's own rows on a status line.
    ///
    /// Not noexcept, for the reason startLine() gives.
    /// @param line A line of the screen's grid.
    /// @return true if the line is drawn.
    [[nodiscard]] bool isGridLineDrawn(LineOffset line) const;

    /// Whether the main page's marks -- its selection, its yank highlight and the links detected on it --
    /// apply to the screen drawn: on the main display only. A status line is a display of its own (DEC STD
    /// 070 ch. 14), which none of them covers, whatever coordinates its rows share with the page's.
    /// @return true on the main display.
    [[nodiscard]] bool takesTheMainPagesMarks() const noexcept { return _display == RenderedDisplay::Main; }

    /// The tint for @p id, resolved once per LINE -- not per cell, and only when @ref
    /// _contextTintingPossible said a tint could apply at all, which is what keeps the ordinary
    /// session (no scheme sets a tint) at one bool test per line.
    [[nodiscard]] std::optional<RGBColor> tintFor(ContextId id) const noexcept;

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
                                                   ConstCellProxy cell,
                                                   LineFlags lineFlags,
                                                   RGBColor fg,
                                                   RGBColor bg,
                                                   LineOffset line,
                                                   ColumnOffset column);

    /// The link role an OSC 8 hyperlink's cells take this frame.
    ///
    /// A link the terminal no longer holds (evicted from the hyperlink cache) is dead -- nothing opens it --
    /// so it gets no decoration. Looking a link up also refreshes it in the cache, keeping the links on
    /// screen from ageing out. The last answer is remembered, so a run of one link's cells costs one lookup.
    /// @param hyperlink The cell's hyperlink; none gives LinkDecorationRole::None.
    /// @return Hover for the link under the pointer, Normal for any other live one, None for a dead one.
    [[nodiscard]] LinkDecorationRole osc8RoleOf(HyperlinkId hyperlink) const noexcept;

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

    /// How a cell is painted for the search.
    enum class SearchHighlight : uint8_t
    {
        None = 0,     //!< No match covers the cell.
        Match,        //!< A match covers it.
        FocusedMatch, //!< The match the cursor stands on covers it.
    };

    /// One match of the search pattern within reach of the rows being rendered.
    struct SearchMatch
    {
        CellLocation first {};                             //!< The cell the match begins at.
        CellLocation last {};                              //!< The last column it covers.
        SearchHighlight highlight = SearchHighlight::None; //!< How its cells are painted.
    };

    /// Works out which cells of grid row @p gridLine the search highlights, into _rowSearchHighlights.
    ///
    /// The matches are the search query's own (SearchQuery::matchesIn), asked once per pass for the segment
    /// holding the row: rows arrive top to bottom, and the rows of one segment ask once between them. A
    /// match that ends past its segment -- a literal across a cut -- is kept until its last row is painted.
    /// A query the engine finds too complex paints nothing, and is not asked again this pass.
    /// @param gridLine The grid row about to be rendered cell by cell.
    void prepareSearchHighlights(LineOffset gridLine);

    /// Paints the cell just emitted for @p column as a search match, where one covers it.
    /// @param column The column of the cell, on the row prepareSearchHighlights() last prepared.
    void paintSearchHighlight(ColumnOffset column);

    /// Works out which cells of grid row @p gridLine detected links decorate, into _rowLinkRoles, as
    /// prepareSearchHighlights() does for the search. A row written since its scan gets none: it holds other
    /// text at the columns its links were found at, until it is scanned again.
    /// @param gridLine The grid row about to be rendered.
    void prepareLinkOverlay(LineOffset gridLine);

    /// The role a detected link gives @p column of the row prepareLinkOverlay() last prepared.
    /// @param column A grid column, not the display column DECDWL doubles.
    /// @return The role; None where no detected link draws anything.
    [[nodiscard]] LinkDecorationRole detectedRoleAt(ColumnOffset column) const noexcept;

    /// The role a cell of the row prepareLinkOverlay() last prepared takes: its OSC 8 link's, else a detected
    /// link's. A cell holding an OSC 8 link holds no detected one: the scan does not read it.
    /// @param hyperlink The cell's OSC 8 link; none for a cell without one, which costs no lookup.
    /// @param column The cell's grid column, not the display column DECDWL doubles.
    /// @return The role; None where nothing is drawn.
    [[nodiscard]] LinkDecorationRole linkRoleAt(HyperlinkId hyperlink, ColumnOffset column) const noexcept;

    /// Tests if the given screen line offset does contain a cursor (either ANSI cursor or vi cursor, if
    /// shown) and returns false otherwise, which guarantees that no cursor is to be rendered
    /// on the given line offset.
    ///
    /// Not noexcept, for the reason startLine() gives.
    [[nodiscard]] bool gridLineContainsCursor(LineOffset screenRow) const;

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
    HyperlinkId _hoveredHyperlink; ///< The OSC 8 link under the pointer, as the frame was asked to draw it.
    LinkDecorationMode _linkMode;  ///< hyperlink_decoration.mode, fixed for the pass.
    FollowModifiers _followModifiers; ///< Whether the follow modifiers are held, fixed for the pass.
    LinkPointer _hoverPointer; ///< Where the pointer counts as being over its link, fixed for the pass.
    /// The role of a detected link's cells while the pointer is elsewhere, fixed for the pass.
    LinkDecorationRole _detectedRestRole;
    /// The role of the cells of the detected link under the pointer, fixed for the pass.
    LinkDecorationRole _detectedHoverRole;
    /// The links detected on the screen rendered; nullptr when it has none, or when neither role above draws
    /// anything, which spares every row its lookup.
    LinkIndex const* _detectedLinks;
    LinkId _hoveredDetectedLink; ///< The detected link under the pointer, as the frame was asked to draw it.
    RenderedDisplay _display;    ///< The display the screen is drawn on.
    mutable HyperlinkId _lastRoleLink {};                            ///< The link osc8RoleOf() answered last.
    mutable LinkDecorationRole _lastRole = LinkDecorationRole::None; ///< The role it answered for it.
    ColumnCount _inputMethodSkipColumns = ColumnCount(0);

    int _prevWidth = 0;
    bool _prevHasCursor = false;
    LineOffset _lineNr = LineOffset(0);
    bool _useCursorlineColoring = false;

    /// The OSC 3008 tint in force on the line currently being rendered, resolved once per line.
    std::optional<RGBColor> _currentContextTint {};

    /// Whether any tint could apply at all -- the scheme sets one, the configuration allows it, and
    /// this is the primary screen. Decided once per pass so the per-line path is a single branch.
    ///
    /// The alternate screen is excluded on purpose. The equality guard in makeColors() would make the
    /// tint PATCHY there rather than uniform -- a full-screen application paints most of its canvas
    /// with its own background, so only the cells it left at default would tint, and a striped vim
    /// reads as a rendering fault. The application's background IS the application; the information is
    /// constant for the whole buffer anyway, so there is nothing per-line to convey; and the status
    /// line is the better channel for it, being already immune to an application's own colours.
    bool _contextTintingPossible = false;

    /// How opaque a blinking (SGR 5) cell is drawn this pass: the slow blinker's phase at the terminal's
    /// clock, which is the frame's. Read once per pass, so every cell blinks in the same phase and the
    /// cell loop computes no cosine.
    float _blinkOpacity;

    /// How opaque a rapidly blinking (SGR 6) cell is drawn this pass, read as @ref _blinkOpacity is.
    float _rapidBlinkOpacity;

    /// The search matches found so far this pass that may still reach the row being rendered.
    std::vector<SearchMatch> _searchMatches;

    /// The segment whose matches were last added to _searchMatches this pass.
    std::optional<SearchSegment> _searchSegment;

    /// Why the search query could not be matched this pass; nothing while it could. @see searchFailure
    std::optional<SearchPatternError> _searchFailure;

    /// How each cell of the row being rendered is painted for the search, by column; empty for none.
    std::vector<SearchHighlight> _rowSearchHighlights;

    /// The role detected links give each cell of the row being rendered, by grid column; empty when none
    /// draws anything there, which is what keeps a row on the batched path and a cell to one test.
    std::vector<LinkDecorationRole> _rowLinkRoles;

    // Current line flags being rendered.
    LineFlags _currentLineFlags = LineFlag::None;
};

} // namespace vtbackend
