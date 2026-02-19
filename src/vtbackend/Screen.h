// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Capabilities.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Charset.h>
#include <vtbackend/Color.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/ScreenBase.h>
#include <vtbackend/VTType.h>
#include <vtbackend/cell/CellConcept.h>

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
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

class SixelImageBuilder;
class Terminal;
struct Settings;

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
enum class RequestPixelSize : uint8_t // TODO: rename RequestPixelSize to RequestArea?
{
    CellArea,
    TextArea,
    WindowArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString : uint8_t
{
    SGR,
    DECSCL,
    DECSCUSR,
    DECSCA,
    DECSTBM,
    DECSLRM,
    DECSLPP,
    DECSCPP,
    DECSNLS,
    DECSASD,
    DECSSDT,
};

inline std::string setDynamicColorValue(
    RGBColor const& color) // TODO: yet another helper. maybe SemanticsUtils static class?
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return std::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
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
template <CellConcept Cell>
class Screen final: public ScreenBase, public capabilities::StaticDatabase
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
    Screen(Screen&&) noexcept = default;
    Screen& operator=(Screen&&) noexcept = default;
    ~Screen() override = default;

    using StaticDatabase::numericCapability;
    [[nodiscard]] unsigned numericCapability(capabilities::Code cap) const override;

    [[nodiscard]] LineFlags lineFlags(LineOffset line) const noexcept { return _grid.lineAt(line).flags(); }

    // {{{ SequenceHandler overrides
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view text, size_t cellCount) override;
    void writeTextEnd() override;
    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& seq) override;
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

    // DECSEL
    void selectiveEraseToBeginOfLine();
    void selectiveEraseToEndOfLine();
    void selectiveEraseLine(LineOffset line);

    // DECSED
    void selectiveEraseToBeginOfScreen();
    void selectiveEraseToEndOfScreen();
    void selectiveEraseScreen();

    void selectiveEraseArea(Rect area);

    void selectiveErase(LineOffset line, ColumnOffset begin, ColumnOffset end);
    [[nodiscard]] bool containsProtectedCharacters(LineOffset line,
                                                   ColumnOffset begin,
                                                   ColumnOffset end) const;

    void eraseCharacters(ColumnCount n);  // ECH
    void insertCharacters(ColumnCount n); // ICH
    void deleteCharacters(ColumnCount n); // DCH
    void deleteColumns(ColumnCount n);    // DECDC
    void insertLines(LineCount n);        // IL
    void insertColumns(ColumnCount n);    // DECIC

    void copyArea(Rect sourceArea, int page, CellLocation targetTopLeft, int targetPage);

    void eraseArea(int top, int left, int bottom, int right);

    void fillArea(char32_t ch, int top, int left, int bottom, int right);

    void deleteLines(LineCount n); // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorTo(LineOffset line, ColumnOffset column) override; // CUP
    void moveCursorBackward(ColumnCount n);                           // CUB
    void moveCursorDown(LineCount n);                                 // CUD
    void moveCursorForward(ColumnCount n);                            // CUF
    void moveCursorToBeginOfLine();                                   // CR
    void moveCursorToColumn(ColumnOffset n);                          // CHA
    void moveCursorToLine(LineOffset n);                              // VPA
    void moveCursorToNextLine(LineCount n);                           // CNL
    void moveCursorToNextTab();                                       // HT
    void moveCursorToPrevLine(LineCount n);                           // CPL
    void moveCursorUp(LineCount n);                                   // CUU

    void cursorBackwardTab(TabStopCount count);        // CBT
    void cursorForwardTab(TabStopCount count);         // CHT
    void backspace();                                  // BS
    void horizontalTabClear(HorizontalTabClear which); // TBC
    void horizontalTabSet();                           // HTS

    void index();        // IND
    void reverseIndex(); // RI

    void setScrollSpeed(int speed);      // DECSSCLS
    void deviceStatusReport();           // DSR
    void reportCursorPosition();         // CPR
    void reportExtendedCursorPosition(); // DECXCPR
    void reportColorPaletteUpdate() override;
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
    void singleShiftSelect(CharsetTable table);
    void requestPixelSize(RequestPixelSize area);
    void requestCharacterSize(RequestPixelSize area);
    void sixelImage(ImageSize pixelSize, Image::Data&& rgbaData);
    void requestStatusString(RequestStatusString value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName name);
    void setDynamicColor(DynamicColorName name, RGBColor color);
    void inspect();
    void smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value);
    // }}}

    std::shared_ptr<Image const> uploadImage(ImageFormat format, ImageSize imageSize, Image::Data&& pixmap);

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
                     ImageLayer layer = ImageLayer::Replace);

    void renderImageByName(std::string const& name,
                           GridSize gridSize,
                           PixelCoordinate imageOffset,
                           ImageSize imageSize,
                           ImageAlignment alignmentPolicy,
                           ImageResize resizePolicy,
                           bool autoScroll,
                           bool requestStatus,
                           ImageLayer layer = ImageLayer::Replace);

    void renderImage(ImageFormat format,
                     ImageSize imageSize,
                     Image::Data&& pixmap,
                     GridSize gridSize,
                     ImageAlignment alignmentPolicy,
                     ImageResize resizePolicy,
                     bool autoScroll,
                     ImageLayer layer = ImageLayer::Replace);

    void releaseImage(std::string const& name);

    void inspect(std::string const& message, std::ostream& os) const override;

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

    [[nodiscard]] constexpr CellLocation realCursorPosition() const noexcept { return _cursor.position; }

    [[nodiscard]] constexpr CellLocation logicalCursorPosition() const noexcept
    {
        if (!_cursor.originMode)
            return realCursorPosition();
        else
            return CellLocation { _cursor.position.line - margin().vertical.from,
                                  _cursor.position.column - margin().horizontal.from };
    }

    [[nodiscard]] constexpr CellLocation origin() const noexcept
    {
        if (!_cursor.originMode)
            return {};

        return { margin().vertical.from, margin().horizontal.from };
    }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    [[nodiscard]] CellLocation toRealCoordinate(CellLocation pos) const noexcept
    {
        if (!_cursor.originMode)
            return pos;
        else
            return { pos.line + margin().vertical.from, pos.column + margin().horizontal.from };
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

    [[nodiscard]] Rect applyOriginMode(Rect area) const noexcept
    {
        if (!_cursor.originMode)
            return area;

        auto const top = Top::cast_from(area.top.value + margin().vertical.from.value);
        auto const left = Left::cast_from(area.top.value + margin().horizontal.from.value);
        auto const bottom = Bottom::cast_from(area.bottom.value + margin().vertical.from.value);
        auto const right = Right::cast_from(area.right.value + margin().horizontal.from.value);
        // TODO: Should this automatically clamp to margin's botom/right values?

        return Rect { top, left, bottom, right };
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
        return { std::clamp(coord.line, LineOffset { 0 }, margin().vertical.to),
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
        return { clampedLine(coord.line), clampedColumn(coord.column) };
    }

    [[nodiscard]] CellLocation clampToMargin(CellLocation pos) const noexcept
    {
        return { std::clamp(pos.line, margin().vertical.from, margin().vertical.to),
                 std::clamp(pos.column, margin().horizontal.from, margin().horizontal.to) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] bool contains(CellLocation coord) const noexcept override
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(pageSize().lines)
               && ColumnOffset(0) <= coord.column
               && coord.column <= boxed_cast<ColumnOffset>(pageSize().columns);
    }

    [[nodiscard]] std::optional<CellLocation> search(std::u32string_view searchText,
                                                     CellLocation startPosition) override;
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                            CellLocation startPosition) override;

    [[nodiscard]] Cell& usePreviousCell() noexcept
    {
        return useCellAt(_lastCursorPosition.line, _lastCursorPosition.column);
    }

    void updateCursorIterator() noexcept override { _currentLine = &_grid.lineAt(_cursor.position.line); }

    [[nodiscard]] Line<Cell>& currentLine() noexcept { return *_currentLine; }

    [[nodiscard]] Line<Cell> const& currentLine() const noexcept { return *_currentLine; }

    [[nodiscard]] Cell& useCurrentCell() noexcept { return currentLine().useCellAt(_cursor.position.column); }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] Cell& at(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.useCellAt(line, column);
    }
    [[nodiscard]] Cell& useCellAt(LineOffset line, ColumnOffset column) noexcept
    {
        return _grid.lineAt(line).useCellAt(column);
    }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] Cell const& at(LineOffset line, ColumnOffset column) const noexcept
    {
        return _grid.at(line, column);
    }

    [[nodiscard]] Cell& at(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] Cell& useCellAt(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] Cell const& at(CellLocation p) const noexcept { return _grid.at(p.line, p.column); }

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

    void scrollUp(LineCount n) { scrollUp(n, margin()); }
    void scrollDown(LineCount n) { scrollDown(n, margin()); }
    void scrollLeft(ColumnCount n);
    void scrollRight(ColumnCount n);
    void unscroll(LineCount n);

    void verifyState() const override;

    [[nodiscard]] Grid<Cell> const& grid() const noexcept { return _grid; }
    [[nodiscard]] Grid<Cell>& grid() noexcept { return _grid; }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return _grid.isLineWrapped(lineNumber);
    }

    [[nodiscard]] bool isCellEmpty(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).cellEmptyAt(position.column);
    }

    [[nodiscard]] bool compareCellTextAt(CellLocation position, char32_t codepoint) const noexcept override
    {
        auto const& cell = _grid.lineAt(position.line).inflatedBuffer().at(position.column.as<size_t>());
        return CellUtil::compareText(cell, codepoint);
    }

    // IMPORTANT: Invokig inflatedBuffer() is expensive. This function should be invoked with caution.
    [[nodiscard]] std::string cellTextAt(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).inflatedBuffer().at(position.column.as<size_t>()).toUtf8();
    }

    [[nodiscard]] CellFlags cellFlagsAt(CellLocation position) const noexcept override
    {
        // TODO: This is not efficient. We should have a direct access to the flags.
        return _grid.lineAt(position.line).inflatedBuffer().at(position.column.as<size_t>()).flags();
    }

    [[nodiscard]] Color cellForegroundColorAt(CellLocation position) const noexcept override
    {
        return at(position).foregroundColor();
    }

    [[nodiscard]] Color cellBackgroundColorAt(CellLocation position) const noexcept override
    {
        return at(position).backgroundColor();
    }

    [[nodiscard]] LineFlags lineFlagsAt(LineOffset line) const noexcept override
    {
        return _grid.lineAt(line).flags();
    }

    void enableLineFlags(LineOffset lineOffset, LineFlags flags, bool enable) noexcept override
    {
        _grid.lineAt(lineOffset).setFlag(flags, enable);
    }

    [[nodiscard]] bool isLineFlagEnabledAt(LineOffset line, LineFlags flags) const noexcept override
    {
        return _grid.lineAt(line).isFlagEnabled(flags);
    }

    [[nodiscard]] std::string lineTextAt(LineOffset line,
                                         bool stripLeadingSpaces,
                                         bool stripTrailingSpaces) const noexcept override
    {
        return _grid.lineAt(line).toUtf8Trimmed(stripLeadingSpaces, stripTrailingSpaces);
    }

    [[nodiscard]] bool isLineEmpty(LineOffset line) const noexcept override
    {
        return _grid.lineAt(line).empty();
    }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).cellWidthAt(position.column);
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept override { return _grid.historyLineCount(); }

    [[nodiscard]] HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept override
    {
        auto const& line = _grid.lineAt(position.line);
        if (line.isTrivialBuffer())
        {
            TrivialLineBuffer const& lineBuffer = line.trivialBuffer();
            return lineBuffer.hyperlink;
        }
        return at(position).hyperlink();
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkAt(CellLocation pos) const noexcept override;

    void applyAndLog(Function const& function, Sequence const& seq);
    [[nodiscard]] ApplyResult apply(Function const& function, Sequence const& seq);

    void fail(std::string const& message) const override;

    void hardReset();
    void applyPageSizeToMainDisplay(PageSize pageSize);

    void saveCursor() override;
    void restoreCursor() override;
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

    /// Attempts to emplace the given character sequence into the current cursor position, assuming
    /// that the current line is either empty or trivial and the input character sequence is contiguous.
    ///
    /// @returns the string view of the UTF-8 text that could not be emplaced.
    std::string_view tryEmplaceChars(std::string_view chars, size_t cellCount) noexcept;
    size_t emplaceCharsIntoCurrentLine(std::string_view chars, size_t cellCount) noexcept;
    [[nodiscard]] bool isContiguousToCurrentLine(std::string_view continuationChars) const noexcept;
    void advanceCursorAfterWrite(ColumnCount n) noexcept;

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p column.
    void linefeed(ColumnOffset column);

    void writeCharToCurrentAndAdvance(char32_t codepoint) noexcept;
    void clearAndAdvance(int oldWidth, int newWidth) noexcept;

    void scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin);
    void scrollUp(LineCount n, Margin margin);
    void scrollDown(LineCount n, Margin margin);
    void insertChars(LineOffset lineOffset, ColumnCount columnsToInsert);
    void deleteChars(LineOffset lineOffset, ColumnOffset column, ColumnCount columnsToDelete);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnOffset n);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(Sequence const& seq);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageUpload(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageRender(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageRelease(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookGoodImageOneshot(Sequence const& seq);

    void processShellIntegration(Sequence const& seq);

    gsl::not_null<Terminal*> _terminal;
    gsl::not_null<Settings*> _settings;
    gsl::not_null<Margin*> _margin;
    Grid<Cell> _grid;

    GraphicsAttributes _savedGraphicsRenditions {};

    CellLocation _lastCursorPosition {};

    Line<Cell>* _currentLine = nullptr;
    std::unique_ptr<SixelImageBuilder> _sixelImageBuilder;

#if defined(LIBTERMINAL_LOG_TRACE)
    std::atomic<bool> _logCharTrace = true;
    std::string _pendingCharTraceLog;
#endif

    std::string_view _name;

    /// Controls whether DECCARA/DECRARA operate on the full rectangle (true) or stream (false).
    bool _rectangularAttributeMode = true;

    /// XTCHECKSUM extension flags for DECRQCRA checksum computation.
    int _checksumExtension = 0;
};

template <CellConcept Cell>
inline void Screen<Cell>::scrollUp(LineCount n, Margin margin)
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
