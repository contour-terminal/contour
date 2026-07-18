// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Capabilities.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Charset.h>
#include <vtbackend/Color.h>
#include <vtbackend/CommandBlocks.h>
#include <vtbackend/Cursor.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/KittyGraphics.h>
#include <vtbackend/MessageParser.h>
#include <vtbackend/PromptRegion.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/VTType.h>

#include <vtparser/ParserExtension.h>

#include <crispy/algorithm.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/width.h>

#include <gsl/pointers>

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

class SemanticBlockTracker;
class SixelImageBuilder;
class Terminal;
struct CommandBlockInfo;
struct Settings;

namespace regis
{
    struct ReGISContext;
    class ReGISRasterizer;
    class ReGISTextRasterizer;
    class ReGISEvents;
} // namespace regis

// {{{ TODO: move me somewhere more appropriate
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class Item : uint8_t
    {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class Action : uint8_t
    {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using Value = std::variant<std::monostate, unsigned, ImageSize>;
} // namespace XtSmGraphics

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class HorizontalTabClear : uint8_t
{
    /// Ps = 0 (default)
    AllTabs,

    /// Ps = 3
    UnderCursor,
};

/// Input: CSI 16 t
///
///  Input: CSI 14 t (for text area size)
///  Input: CSI 14; 2 t (for full window size)
/// Output: CSI 14 ; width ; height ; t
/// The area an XTWINOPS report is asked about.
enum class RequestPixelSize : uint8_t // TODO: rename RequestPixelSize to RequestArea?
{
    /// A single character cell. `CSI 16 t`.
    CellArea,

    /// The grid of cells the application writes to. `CSI 14 t`, `CSI 18 t`.
    TextArea,

    /// The window, including whatever chrome the frontend draws around the text area. `CSI 14 ; 2 t`.
    WindowArea,

    /// The screen the window is displayed on -- xterm's "display". `CSI 15 t`, `CSI 19 t`.
    ScreenArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString : uint8_t
{
    SGR,
    DECSCL,
    DECSCUSR,
    DECSCA,
    DECSACE,
    DECELF,
    DECLFKC,
    DECSMKR,
    DECSTBM,
    DECSLRM,
    DECSLPP,
    DECSCPP,
    DECSNLS,
    DECSASD,
    DECSSDT,
};

/// Renders @p color as the X11 color specification a color query is answered with.
///
/// Each of Contour's 8-bit channels is widened to the 16 bits X11 specifies by repeating its byte, so
/// 0xAB is reported as "abab" -- exactly as xterm reports a color it was given as "rgb:ab/ab/ab".
///
/// The digits are lower-case, as xterm's are: an application comparing the answer to the specification
/// it sent (as esctest does, and as any round-tripping application would) reads upper-case digits as a
/// different color.
[[nodiscard]] inline std::string colorSpecification(RGBColor const& color)
{
    auto const widen = [](uint8_t channel) {
        return (static_cast<unsigned>(channel) << 8) | channel;
    };
    return std::format("rgb:{:04x}/{:04x}/{:04x}", widen(color.red), widen(color.green), widen(color.blue));
}

enum class ApplyResult : uint8_t
{
    Ok,
    Invalid,
    Unsupported,
};
// }}}

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
class Screen final: public SequenceHandler, public capabilities::StaticDatabase
{
  public:
    /// @param terminal            reference to the terminal this display belongs to.
    /// @param margin              margin of this display.
    /// @param pageSize            page size of this display. This is passed because it does not necessarily
    ///                            need to match the terminal's main display page size.
    /// @param reflowOnResize      whether or not to perform virtual line text reflow on resuze.
    /// @param maxHistoryLineCount maximum number of lines that are can be scrolled back to via Viewport.
    /// @param name                name of this screen, used for logging purposes.
    Screen(Terminal& terminal,
           gsl::not_null<Margin*> margin,
           PageSize pageSize,
           bool reflowOnResize,
           MaxHistoryLineCount maxHistoryLineCount,
           std::string_view name);

    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) noexcept = delete;
    Screen& operator=(Screen&&) noexcept = delete;
    ~Screen() override; // SequenceHandler is the only virtual base

    [[nodiscard]] Cursor& cursor() noexcept { return _cursor; }
    [[nodiscard]] Cursor const& cursor() const noexcept { return _cursor; }
    [[nodiscard]] Cursor const& savedCursorState() const noexcept { return _savedCursor; }
    void resetSavedCursorState() { _savedCursor = {}; }

    using StaticDatabase::numericCapability;
    [[nodiscard]] unsigned numericCapability(capabilities::Code cap) const override;

    [[nodiscard]] LineFlags lineFlags(LineOffset line) const noexcept { return _grid.lineAt(line).flags(); }

    // {{{ SequenceHandlers
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view text, size_t cellCount) override;
    void writeTextEnd() override;
    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& seq) override;
    void processAPC(std::string_view body) override;

  private:
    /// Handles one kitty graphics command (the APC body after its leading 'G').
    void processKittyGraphics(std::string_view body);

    /// Answers a kitty graphics command, unless it asked to be left alone (`q=`).
    void replyKittyGraphics(kitty_graphics::Command const& command, std::string_view status);

    /// Places @p image at the cursor, sized per the command's `c=`/`r=` or derived from its pixels.
    void renderKittyImage(kitty_graphics::Command const& command, std::shared_ptr<Image const> const& image);

  public:
    // }}}

    void writeTextFromExternal(std::string_view text);

    /// Renders the full screen by passing every grid cell to the callback.
    ///
    /// @param extraLines  Additional lines to render beyond the page size (e.g. for smooth scrolling).
    template <typename Renderer>
    RenderPassHints render(Renderer&& render,
                           ScrollOffset scrollOffset = {},
                           HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes,
                           LineCount extraLines = LineCount(0)) const
    {
        return _grid.render(std::forward<Renderer>(render), scrollOffset, highlightSearchMatches, extraLines);
    }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    [[nodiscard]] std::string renderMainPageText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    [[nodiscard]] std::string screenshot(std::function<std::string(LineOffset)> const& postLine = {}) const;

    void crlf() { linefeed(margin().horizontal.from); }
    void crlfIfWrapPending();

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();
    void setMark();

    // Erase, sparing cells that carry @p protectedFlag. The default (CharacterProtected) is DEC/DECSCA
    // protection, so the DECSEL/DECSED/DECSERA handlers erase selectively with no argument. The regular
    // ED/EL/ECH erases pass CharacterProtectedISO to spare ISO 6429 (SPA/EPA) guarded cells instead.

    // DECSEL
    void selectiveEraseToBeginOfLine(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseToEndOfLine(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseLine(LineOffset line, CellFlag protectedFlag = CellFlag::CharacterProtected);

    // DECSED
    void selectiveEraseToBeginOfScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseToEndOfScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);
    void selectiveEraseScreen(CellFlag protectedFlag = CellFlag::CharacterProtected);

    void selectiveEraseArea(Rect area, CellFlag protectedFlag = CellFlag::CharacterProtected);

    void selectiveErase(LineOffset line,
                        ColumnOffset begin,
                        ColumnOffset end,
                        CellFlag protectedFlag = CellFlag::CharacterProtected);
    [[nodiscard]] bool containsProtectedCharacters(
        LineOffset line,
        ColumnOffset begin,
        ColumnOffset end,
        CellFlag protectedFlag = CellFlag::CharacterProtected) const;

    void eraseCharacters(ColumnCount n);  // ECH
    void insertCharacters(ColumnCount n); // ICH
    void deleteCharacters(ColumnCount n); // DCH
    void deleteColumns(ColumnCount n);    // DECDC
    void insertLines(LineCount n);        // IL
    void insertColumns(ColumnCount n);    // DECIC

    void copyArea(Rect sourceArea, int page, CellLocation targetTopLeft, int targetPage);

    // DEC Multi-Page Navigation (VT420)
    void nextPage(int count);                   ///< NP — Move to next page(s), cursor to home.
    void previousPage(int count);               ///< PP — Move to previous page(s), cursor to home.
    void pagePositionAbsolute(int page);        ///< PPA — Move to absolute page, preserve cursor.
    void pagePositionRelative(int count);       ///< PPR — Move forward by count pages, preserve cursor.
    void pagePositionBackward(int count);       ///< PPB — Move backward by count pages, preserve cursor.
    void requestDisplayedExtent();              ///< DECRQDE — Report displayed page extent.
    void requestUserPreferredSupplementalSet(); ///< DECRQUPSS — Report the User-Preferred Supplemental Set.

    void eraseArea(int top, int left, int bottom, int right);

    void fillArea(char32_t ch, int top, int left, int bottom, int right);

    void deleteLines(LineCount n); // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorTo(LineOffset line, ColumnOffset column); // CUP
    void moveCursorBackward(ColumnCount n);                  // CUB
    void moveCursorDown(LineCount n);                        // CUD
    void moveCursorForward(ColumnCount n);                   // CUF
    void moveCursorToBeginOfLine();                          // CR
    void moveCursorToColumn(ColumnOffset n);                 // CHA
    void moveCursorToLine(LineOffset n);                     // VPA
    void moveCursorToNextLine(LineCount n);                  // CNL
    void moveCursorToNextTab();                              // HT
    void moveCursorToPrevLine(LineCount n);                  // CPL
    void moveCursorUp(LineCount n);                          // CUU

    void cursorBackwardTab(TabStopCount count);        // CBT
    void cursorForwardTab(TabStopCount count);         // CHT
    void backspace();                                  // BS
    void horizontalTabClear(HorizontalTabClear which); // TBC
    void horizontalTabSet();                           // HTS

    void index();        // IND
    void reverseIndex(); // RI

    void setScrollSpeed(int speed); // DECSSCLS
    void deviceStatusReport();      // DSR
    void reportCursorPosition();    // CPR

    /// DECCKSR (`CSI ? 63 ; Pid n`) -- the checksum of the macro memory.
    /// @param requestId The id the request was tagged with, carried back in the reply.
    void reportMacroSpaceChecksum(unsigned requestId);
    void reportExtendedCursorPosition(); // DECXCPR
    void reportCursorInformation();      // DECCIR
    void reportColorPaletteUpdate();
    void selectConformanceLevel(VTType level);
    void requestDynamicColor(DynamicColorName name);
    void requestCapability(capabilities::Code code);
    void requestCapability(std::string_view name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& url); // OSC 7

    void hyperlink(std::string id, std::string uri);                   // OSC 8
    void notify(std::string const& title, std::string const& content); // OSC 777

    void captureBuffer(LineCount lineCount, bool logicalLines);

    void setForegroundColor(Color color);
    void setBackgroundColor(Color color);
    void setUnderlineColor(Color color);
    void setGraphicsRendition(GraphicsRendition rendition);
    void screenAlignmentPattern();
    void applicationKeypadMode(bool enable);
    void designateCharset(CharsetTable table, CharsetId charset);

    /// Attempts to handle an unmatched ESC sequence as SCS designation.
    /// Returns true if the sequence was recognized as SCS (including DRCS two-byte designators).
    [[nodiscard]] bool tryHandleSCS(Sequence const& seq);
    void singleShiftSelect(CharsetTable table);
    void requestPixelSize(RequestPixelSize area);
    void requestCharacterSize(RequestPixelSize area);
    void sixelImage(ImageSize pixelSize, Image::Data&& rgbaData);

    /// Publishes the persistent ReGIS canvas to the grid as an overlay image. Invoked when a ReGIS
    /// DCS string drew anything.
    /// @param pixelSize The canvas pixel dimensions.
    /// @param rgbaData The canvas RGBA pixels (a copy; the persistent canvas is left intact).
    void regisImage(ImageSize pixelSize, Image::Data&& rgbaData);

    /// Commits the current ReGIS canvas to the grid via @ref regisImage if anything was drawn.
    void commitReGIS();

    /// Injects the rasterizer used for ReGIS text glyphs, replacing the built-in embedded font.
    ///
    /// The display layer calls this with a text_shaper-backed implementation so ReGIS text renders
    /// through the terminal's real font engine; passing nullptr restores the embedded default.
    /// @param rasterizer The text rasterizer to use (shared with the display).
    void setReGISTextRasterizer(std::shared_ptr<regis::ReGISTextRasterizer> rasterizer) noexcept
    {
        _regisTextRasterizer = std::move(rasterizer);
    }
    void requestStatusString(RequestStatusString value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName name);
    void setDynamicColor(DynamicColorName name, RGBColor color);
    void inspect();
    void smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value);
    // }}}

    std::shared_ptr<Image const> uploadImage(ImageFormat format, ImageSize imageSize, Image::Data&& pixmap);

    /// Uploads an image to the named image pool, decoding PNG to RGBA if needed.
    void uploadImage(std::string name, ImageFormat format, ImageSize imageSize, Image::Data&& pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p imageId ID to the image to be rendered.
     * @p topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p gridSize Screen grid size to span the image into.
     * @p imageOffset Offset into the image in screen grid coordinate to start rendering from.
     * @p imageSize Size of the full image in Screen grid coordinates.
     * @p alignmentPolicy render the image using the given image alignment policy.
     * @p resizePolicy render the image using the given image resize policy.
     * @p autoScroll Boolean indicating whether or not the screen should scroll if the image cannot be fully
     * displayed otherwise.
     */
    void renderImage(std::shared_ptr<Image const> image,
                     CellLocation topLeft,
                     GridSize gridSize,
                     PixelCoordinate imageOffset,
                     ImageSize imageSize,
                     ImageAlignment alignmentPolicy,
                     ImageResize resizePolicy,
                     bool autoScroll,
                     bool updateCursor = true,
                     ImageLayer layer = ImageLayer::Replace);

    /// Renders a previously uploaded named image onto the grid at the current cursor position.
    void renderImageByName(std::string const& name,
                           GridSize gridSize,
                           PixelCoordinate imageOffset,
                           ImageSize imageSize,
                           ImageAlignment alignmentPolicy,
                           ImageResize resizePolicy,
                           bool autoScroll,
                           bool requestStatus,
                           bool updateCursor = false,
                           ImageLayer layer = ImageLayer::Replace);

    /// Renders an image from raw pixel data (upload + render in one step).
    /// @return true on success, false if decoding failed.
    [[nodiscard]] bool renderImage(ImageFormat format,
                                   ImageSize imageSize,
                                   Image::Data&& pixmap,
                                   GridSize gridSize,
                                   ImageAlignment alignmentPolicy,
                                   ImageResize resizePolicy,
                                   bool autoScroll,
                                   bool updateCursor = false,
                                   ImageLayer layer = ImageLayer::Replace);

    /// Removes a named image from the image pool.
    void releaseImage(std::string const& name);

    void inspect(std::string const& message, std::ostream& os) const;

    // for DECSC and DECRC
    void requestAnsiMode(unsigned int mode);
    void requestDECMode(unsigned int mode);

    [[nodiscard]] PageSize pageSize() const noexcept { return _grid.pageSize(); }

    [[nodiscard]] constexpr Margin margin() const noexcept { return *_margin; }
    [[nodiscard]] constexpr Margin& margin() noexcept { return *_margin; }

    [[nodiscard]] bool isFullHorizontalMargins() const noexcept
    {
        return margin().horizontal.to.value + 1 == pageSize().columns.value;
    }

    [[nodiscard]] bool isCursorInsideMargins() const noexcept;

    /// Tests whether the cursor sits within the active left/right margins.
    ///
    /// This mirrors xterm's `!(IsLeftRightMode && !ScrnIsColInMargins)` guard: when DECLRMM is off the
    /// horizontal margins span the whole page, so this is trivially true; when DECLRMM is on it reports
    /// whether the cursor column lies within the margin band. Vertical scrolling is confined to that
    /// band, so IND/RI/LF and friends consult this before deciding to scroll.
    /// @return true if the cursor column is within the horizontal margins.
    [[nodiscard]] bool isCursorInsideHorizontalMargins() const noexcept;

    [[nodiscard]] constexpr CellLocation realCursorPosition() const noexcept { return _cursor.position; }

    [[nodiscard]] constexpr CellLocation logicalCursorPosition() const noexcept
    {
        if (!_cursor.originMode)
            return realCursorPosition();
        else
            return CellLocation { .line = _cursor.position.line - margin().vertical.from,
                                  .column = _cursor.position.column - margin().horizontal.from };
    }

    [[nodiscard]] constexpr CellLocation origin() const noexcept
    {
        if (!_cursor.originMode)
            return {};

        return CellLocation { .line = margin().vertical.from, .column = margin().horizontal.from };
    }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    [[nodiscard]] CellLocation toRealCoordinate(CellLocation pos) const noexcept
    {
        if (!_cursor.originMode)
            return pos;
        else
            return CellLocation { .line = pos.line + margin().vertical.from,
                                  .column = pos.column + margin().horizontal.from };
    }

    [[nodiscard]] LineOffset applyOriginMode(LineOffset line) const noexcept
    {
        if (!_cursor.originMode)
            return line;
        else
            return line + margin().vertical.from;
    }

    [[nodiscard]] ColumnOffset applyOriginMode(ColumnOffset column) const noexcept
    {
        if (!_cursor.originMode)
            return column;
        else
            return column + margin().horizontal.from;
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    [[nodiscard]] CellLocation clampCoordinate(CellLocation coord) const noexcept
    {
        if (_cursor.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    [[nodiscard]] CellLocation clampToOrigin(CellLocation coord) const noexcept
    {
        return CellLocation { .line = std::clamp(coord.line, LineOffset { 0 }, margin().vertical.to),
                              .column =
                                  std::clamp(coord.column, ColumnOffset { 0 }, margin().horizontal.to) };
    }

    [[nodiscard]] LineOffset clampedLine(LineOffset line) const noexcept
    {
        return std::clamp(line, LineOffset(0), boxed_cast<LineOffset>(_grid.pageSize().lines) - 1);
    }

    [[nodiscard]] ColumnOffset clampedColumn(ColumnOffset column) const noexcept
    {
        return std::clamp(column, ColumnOffset(0), boxed_cast<ColumnOffset>(_grid.pageSize().columns) - 1);
    }

    [[nodiscard]] CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return CellLocation { .line = clampedLine(coord.line), .column = clampedColumn(coord.column) };
    }

    [[nodiscard]] CellLocation clampToMargin(CellLocation pos) const noexcept
    {
        return CellLocation { .line = std::clamp(pos.line, margin().vertical.from, margin().vertical.to),
                              .column =
                                  std::clamp(pos.column, margin().horizontal.from, margin().horizontal.to) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] bool contains(CellLocation coord) const noexcept
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(pageSize().lines)
               && ColumnOffset(0) <= coord.column
               && coord.column <= boxed_cast<ColumnOffset>(pageSize().columns);
    }

    [[nodiscard]] std::optional<CellLocation> search(std::u32string_view searchText,
                                                     CellLocation startPosition);
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                            CellLocation startPosition);

    [[nodiscard]] CellProxy usePreviousCell() noexcept
    {
        return useCellAt(_lastCursorPosition.line, _lastCursorPosition.column);
    }

    void updateCursorIterator() noexcept { _currentLine = &_grid.lineAt(_cursor.position.line); }

    [[nodiscard]] Line& currentLine() noexcept { return *_currentLine; }

    [[nodiscard]] Line const& currentLine() const noexcept { return *_currentLine; }

    [[nodiscard]] CellProxy useCurrentCell() noexcept
    {
        return currentLine().useCellAt(_cursor.position.column);
    }

    /// Gets a CellProxy to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.useCellAt(line, column);
    }
    [[nodiscard]] CellProxy useCellAt(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.lineAt(line).useCellAt(column);
    }

    /// Gets a CellProxy to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) const noexcept
    {
        return _grid.at(line, column);
    }

    [[nodiscard]] CellProxy at(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] CellProxy useCellAt(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] CellProxy at(CellLocation p) const noexcept { return _grid.at(p.line, p.column); }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerDownwards(LineOffset startLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerUpwards(LineOffset startLine) const;

    /// The most recently FINISHED shell command, reconstructed from the OSC 133 marks its shell left in
    /// the scrollback.
    ///
    /// Reads the marks alone, so it works for any shell that speaks plain OSC 133 — the semantic-block
    /// reader protocol (DEC mode 2034) is a separate, opt-in channel and is not required here.
    ///
    /// @return The block, or nullopt when the scrollback holds no finished command.
    [[nodiscard]] std::optional<CommandBlockText> lastCommandBlock() const;

    /// Where the shell's LIVE prompt sits in the grid — the one the user is typing at right now.
    ///
    /// The complement of lastCommandBlock(): that one reconstructs a FINISHED command, which is opened by
    /// an OSC 133;D and therefore can never be the prompt currently being typed into. Reads the OSC 133
    /// marks alone, so no shell needs to opt into DEC mode 2034 for this to work.
    ///
    /// Does NOT take the terminal lock — the caller holds it, the same contract lastCommandBlock() has.
    ///
    /// @return The span, or why there is no live prompt (a command is running, no shell integration, or
    ///         the prompt scrolled out of reach).
    [[nodiscard]] std::expected<LivePromptSpan, PromptRegionError> livePromptSpan() const;

    void scrollUp(LineCount n) { scrollUp(n, margin()); }
    void scrollDown(LineCount n) { scrollDown(n, margin()); }
    void scrollLeft(ColumnCount n);
    void scrollRight(ColumnCount n);
    void unscroll(LineCount n);

    void verifyState() const;

    [[nodiscard]] Grid const& grid() const noexcept { return _grid; }
    [[nodiscard]] Grid& grid() noexcept { return _grid; }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return _grid.isLineWrapped(lineNumber);
    }

    [[nodiscard]] bool isCellEmpty(CellLocation position) const noexcept
    {
        return _grid.lineAt(position.line).cellEmptyAt(position.column);
    }

    [[nodiscard]] bool compareCellTextAt(CellLocation position, char32_t codepoint) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return CellUtil::compareText(cell, codepoint);
    }

    [[nodiscard]] std::string cellTextAt(CellLocation position) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return cell.toUtf8();
    }

    [[nodiscard]] CellFlags cellFlagsAt(CellLocation position) const noexcept
    {
        auto cell = _grid.at(position.line, position.column);
        return cell.flags();
    }

    [[nodiscard]] Color cellForegroundColorAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.foregroundColor();
    }

    [[nodiscard]] Color cellBackgroundColorAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.backgroundColor();
    }

    [[nodiscard]] LineFlags lineFlagsAt(LineOffset line) const noexcept { return _grid.lineAt(line).flags(); }

    void enableLineFlags(LineOffset lineOffset, LineFlags flags, bool enable) noexcept
    {
        _grid.lineAt(lineOffset).setFlag(flags, enable);
    }

    /// Sets or clears the semantic marks @p flags on the LOGICAL line that @p line belongs to.
    ///
    /// The one way these marks may be written. They name a logical line — the line the shell wrote, or the
    /// line the user put a Vi mark on — and never the physical piece a wrap happened to chop it into.
    /// Stamping a continuation is exactly where they cannot survive: see HeadOnlyLineFlags, and the
    /// widening resize that rebuilds a joined logical line from its head alone.
    void setLogicalLineFlags(LineOffset line, LineFlags flags, bool enable) noexcept
    {
        enableLineFlags(_grid.logicalLineHead(line), flags, enable);
    }

    /// Whether the LOGICAL line that @p line belongs to carries all of @p flags.
    [[nodiscard]] bool isLogicalLineFlagEnabled(LineOffset line, LineFlags flags) const noexcept
    {
        return isLineFlagEnabledAt(_grid.logicalLineHead(line), flags);
    }

    /// Stamps the semantic marks @p flags onto the LOGICAL line the cursor is on.
    void markLogicalLineAtCursor(LineFlags flags) noexcept
    {
        setLogicalLineFlags(cursor().position.line, flags, true);
    }

    [[nodiscard]] bool isLineFlagEnabledAt(LineOffset line, LineFlags flags) const noexcept
    {
        return _grid.lineAt(line).isFlagEnabled(flags);
    }

    [[nodiscard]] std::string lineTextAt(LineOffset line,
                                         bool stripLeadingSpaces = true,
                                         bool stripTrailingSpaces = true) const noexcept
    {
        return _grid.lineAt(line).toUtf8Trimmed(stripLeadingSpaces, stripTrailingSpaces);
    }

    [[nodiscard]] bool isLineEmpty(LineOffset line) const noexcept { return _grid.lineAt(line).empty(); }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept
    {
        return _grid.lineAt(position.line).cellWidthAt(position.column);
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept { return _grid.historyLineCount(); }

    [[nodiscard]] HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept
    {
        auto cell = at(position);
        return cell.hyperlink();
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkAt(CellLocation pos) const noexcept;

    void applyAndLog(Function const& function, Sequence const& seq);
    [[nodiscard]] ApplyResult apply(Function const& function, Sequence const& seq);

    void fail(std::string const& message) const;

    void hardReset();

    /// Clears any active ISO 6429 (SPA/EPA) guarded-area protection. Called from both the hard reset
    /// (RIS) and the soft reset (DECSTR), mirroring xterm's unconditional reset of `protected_mode`.
    /// The per-cell DECSCA flag is cleared separately, by the SGR reset. @see SPA, EPA, DECSCA.
    void resetProtection() noexcept { _isoProtectionActive = false; }

    void applyPageSizeToMainDisplay(PageSize pageSize);

    void saveCursor();
    void restoreCursor();
    void restoreCursor(Cursor const& savedCursor);

    void restoreGraphicsRendition();
    void saveGraphicsRendition();

    void reply(std::string_view text);

    template <typename... Ts>
    void reply(std::string_view message, Ts const&... args)
    {
        reply(std::vformat(message, std::make_format_args(args...)));
    }

  private:
    void writeTextInternal(char32_t codepoint);

    void configureCurrentLineSize(LineFlags enabled);

    void advanceCursorAfterWrite(ColumnCount n) noexcept;

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p column.
    void linefeed(ColumnOffset column);

    void writeCharToCurrentAndAdvance(char32_t codepoint) noexcept;
    void clearAndAdvance(int oldWidth, int newWidth) noexcept;

    /// Claims or releases columns after a grapheme cluster's width was revised by a codepoint that
    /// joined it late (a variation selector).
    ///
    /// Anchored on @c _lastCursorPosition -- the cluster head -- not on the cursor, which has already
    /// moved past it. Does nothing unless the cursor still sits immediately after that head, so an
    /// intervening cursor move or scroll cannot make this touch an unrelated cell.
    void applyClusterWidthChange(int delta) noexcept;

    /// @return The rightmost column that may still be written on the cursor's line: the right margin
    ///         when the cursor is inside a DECLRMM band, otherwise the last page column.
    [[nodiscard]] ColumnOffset lastWritableColumn() const noexcept;

    void scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin);
    void scrollUp(LineCount n, Margin margin);
    void scrollDown(LineCount n, Margin margin);
    void insertChars(LineOffset lineOffset, ColumnCount columnsToInsert);
    void deleteChars(LineOffset lineOffset, ColumnOffset column, ColumnCount columnsToDelete);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnOffset n);

    /// Places the cursor at the absolute @p column, clamped to the page.
    ///
    /// Unlike setCurrentColumn(), this does not apply origin mode: it is for callers that have already
    /// computed a real column (a tab stop, the page's first column), which must not have the left margin
    /// added to it a second time. @see cursorBackwardTab().
    void setCurrentAbsoluteColumn(ColumnOffset column);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookReGIS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECAUPSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECDLD(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECDMAC(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECUDK(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(Sequence const& seq);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageProtocol(Sequence const& seq);
    void handleGipUpload(Message message);
    void handleGipRender(Message const& message);
    void handleGipRelease(Message const& message);
    void handleGipOneshot(Message message);
    void handleGipQuery();
    void replyGipStatus(int statusCode);

    [[nodiscard]] std::optional<Image::Data> decodePng(std::span<uint8_t const> data, ImageSize& size) const;

    void processShellIntegration(Sequence const& seq);
    void handleSemanticBlockQuery(Sequence const& seq);
    void handleInProgressQuery(SemanticBlockTracker const& tracker);
    void handleCompletedBlocksQuery(SemanticBlockTracker const& tracker,
                                    std::deque<CommandBlockInfo> const& completedBlocks,
                                    unsigned queryType,
                                    int count);

    gsl::not_null<Terminal*> _terminal;
    gsl::not_null<Settings*> _settings;
    gsl::not_null<Margin*> _margin;
    Grid _grid;

    Cursor _cursor {};
    Cursor _savedCursor {};

    /// Payload accumulated across a chunked kitty graphics transmission (`m=1`), plus the command
    /// that opened it -- the continuation chunks carry no control data of their own.
    std::string _kittyChunkedPayload {};
    std::optional<kitty_graphics::Command> _kittyChunkedCommand {};

    /// Images transmitted by a kitty graphics command but not yet displayed, keyed by their `i=` id.
    std::unordered_map<uint32_t, std::shared_ptr<Image const>> _kittyImages {};

    GraphicsAttributes _savedGraphicsRenditions {};

    CellLocation _lastCursorPosition {};

    Line* _currentLine = nullptr;
    std::unique_ptr<SixelImageBuilder> _sixelImageBuilder;

    // Persistent ReGIS interpreter state and canvas: ReGIS state and pixels survive across DCS
    // strings, so these live on the Screen and a fresh parser references them each hook.
    std::unique_ptr<regis::ReGISContext> _regisContext;
    std::unique_ptr<regis::ReGISRasterizer> _regisCanvas;
    std::shared_ptr<regis::ReGISTextRasterizer> _regisTextRasterizer;
    std::unique_ptr<regis::ReGISEvents> _regisEvents;

#if defined(LIBTERMINAL_LOG_TRACE)
    std::atomic<bool> _logCharTrace = true;
    std::string _pendingCharTraceLog;
#endif

    std::string_view _name;

    /// Controls whether DECCARA/DECRARA operate on the full rectangle (true) or stream (false).
    bool _rectangularAttributeMode = true;

    /// Whether ISO 6429 guarded-area protection (SPA/EPA) is in effect. Set by SPA, and left set by
    /// EPA (which only stops guarding *new* cells); cleared only by a reset. @see resetProtection.
    bool _isoProtectionActive = false;

    /// Whether the regular erases (ED/EL/ECH) must spare CellFlag::CharacterProtectedISO cells.
    /// True only while ISO protection is active; when false those erases take their fast paths. DECSCA
    /// (CellFlag::CharacterProtected) is honoured by the selective erases instead, never here.
    [[nodiscard]] bool eraseSkipsProtectedCells() const noexcept { return _isoProtectionActive; }

    // VT525 keyboard settings that Contour has nothing to act on but must still remember and report
    // through DECRQSS: it stores the parameter it was given and hands it back verbatim. @see DECELF,
    // DECLFKC and DECSMKR in requestStatusString(). TODO: wire these to real keyboard behaviour.
    int _enableLocalFunctions = 0;    ///< DECELF (`CSI Pn + q`).
    int _localFunctionKeyControl = 0; ///< DECLFKC (`CSI Pn * }`).
    int _modifierKeyReporting = 0;    ///< DECSMKR (`CSI Pn + r`).
};

inline void Screen::scrollUp(LineCount n, Margin margin)
{
    scrollUp(n, cursor().graphicsRendition, margin);
}

// isContiguousToCurrentLine is implemented in Screen.cpp because it needs
// access to Terminal which is only forward-declared in this header.

} // namespace vtbackend

// {{{ fmt formatter
template <>
struct std::formatter<vtbackend::RequestStatusString>: formatter<std::string_view>
{
    auto format(vtbackend::RequestStatusString value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::RequestStatusString::SGR: name = "SGR"; break;
            case vtbackend::RequestStatusString::DECSCL: name = "DECSCL"; break;
            case vtbackend::RequestStatusString::DECSCUSR: name = "DECSCUSR"; break;
            case vtbackend::RequestStatusString::DECSCA: name = "DECSCA"; break;
            case vtbackend::RequestStatusString::DECSACE: name = "DECSACE"; break;
            case vtbackend::RequestStatusString::DECELF: name = "DECELF"; break;
            case vtbackend::RequestStatusString::DECLFKC: name = "DECLFKC"; break;
            case vtbackend::RequestStatusString::DECSMKR: name = "DECSMKR"; break;
            case vtbackend::RequestStatusString::DECSTBM: name = "DECSTBM"; break;
            case vtbackend::RequestStatusString::DECSLRM: name = "DECSLRM"; break;
            case vtbackend::RequestStatusString::DECSLPP: name = "DECSLPP"; break;
            case vtbackend::RequestStatusString::DECSCPP: name = "DECSCPP"; break;
            case vtbackend::RequestStatusString::DECSNLS: name = "DECSNLS"; break;
            case vtbackend::RequestStatusString::DECSASD: name = "DECSASD"; break;
            case vtbackend::RequestStatusString::DECSSDT: name = "DECSSDT"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::Sequence>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::Sequence const& seq, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "{}", seq.text());
    }
};
// }}}
