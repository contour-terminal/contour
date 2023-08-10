/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/Capabilities.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Charset.h>
#include <vtbackend/Color.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/ScreenEvents.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/VTType.h>
#include <vtbackend/cell/CellConcept.h>

#include <vtparser/ParserExtension.h>

#include <crispy/algorithm.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <bitset>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/width.h>

namespace terminal
{

class ScreenBase: public sequence_handler
{
  public:
    virtual void verifyState() const = 0;
    virtual void fail(std::string const& message) const = 0;

    [[nodiscard]] Cursor& cursor() noexcept { return _cursor; }
    [[nodiscard]] Cursor const& cursor() const noexcept { return _cursor; }
    [[nodiscard]] Cursor const& savedCursorState() const noexcept { return _savedCursor; }
    void resetSavedCursorState() { _savedCursor = {}; }
    virtual void saveCursor() = 0;
    virtual void restoreCursor() = 0;

    [[nodiscard]] virtual margin getMargin() const noexcept = 0;
    [[nodiscard]] virtual margin& getMargin() noexcept = 0;
    [[nodiscard]] virtual bool contains(cell_location coord) const noexcept = 0;
    [[nodiscard]] virtual bool isCellEmpty(cell_location position) const noexcept = 0;
    [[nodiscard]] virtual bool compareCellTextAt(cell_location position, char codepoint) const noexcept = 0;
    [[nodiscard]] virtual std::string cellTextAt(cell_location position) const noexcept = 0;
    [[nodiscard]] virtual line_flags lineFlagsAt(line_offset line) const noexcept = 0;
    virtual void enableLineFlags(line_offset lineOffset, line_flags flags, bool enable) noexcept = 0;
    [[nodiscard]] virtual bool isLineFlagEnabledAt(line_offset line, line_flags flags) const noexcept = 0;
    [[nodiscard]] virtual std::string lineTextAt(line_offset line,
                                                 bool stripLeadingSpaces = true,
                                                 bool stripTrailingSpaces = true) const noexcept = 0;
    [[nodiscard]] virtual bool isLineEmpty(line_offset line) const noexcept = 0;
    [[nodiscard]] virtual uint8_t cellWidthAt(cell_location position) const noexcept = 0;
    [[nodiscard]] virtual LineCount historyLineCount() const noexcept = 0;
    [[nodiscard]] virtual hyperlink_id hyperlinkIdAt(cell_location position) const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<hyperlink_info const> hyperlinkAt(
        cell_location pos) const noexcept = 0;
    virtual void inspect(std::string const& message, std::ostream& os) const = 0;
    virtual void moveCursorTo(line_offset line, column_offset column) = 0; // CUP
    virtual void updateCursorIterator() noexcept = 0;

    [[nodiscard]] virtual std::optional<cell_location> search(std::u32string_view searchText,
                                                              cell_location startPosition) = 0;
    [[nodiscard]] virtual std::optional<cell_location> searchReverse(std::u32string_view searchText,
                                                                     cell_location startPosition) = 0;

  protected:
    Cursor _cursor {};
    Cursor _savedCursor {};
};

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
class Screen final: public ScreenBase, public capabilities::static_database
{
  public:
    /// @param terminal            reference to the terminal this display belongs to.
    /// @param pageSize            page size of this display. This is passed because it does not necessarily
    ///                            need to match the terminal's main display page size.
    /// @param reflowOnResize      whether or not to perform virtual line text reflow on resuze.
    /// @param maxHistoryLineCount maximum number of lines that are can be scrolled back to via Viewport.
    Screen(Terminal& terminal,
           PageSize pageSize,
           bool reflowOnResize,
           max_history_line_count maxHistoryLineCount);

    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) noexcept = default;
    Screen& operator=(Screen&&) noexcept = default;
    ~Screen() override = default;

    using static_database::numericCapability;
    [[nodiscard]] unsigned numericCapability(capabilities::code cap) const override;

    // {{{ SequenceHandler overrides
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view text, size_t cellCount) override;
    void executeControlCode(char controlCode) override;
    void processSequence(sequence const& seq) override;
    // }}}

    void writeTextFromExternal(std::string_view text);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename Renderer>
    RenderPassHints render(
        Renderer&& render,
        scroll_offset scrollOffset = {},
        highlight_search_matches highlightSearchMatches = highlight_search_matches::Yes) const
    {
        return _grid.render(std::forward<Renderer>(render), scrollOffset, highlightSearchMatches);
    }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    [[nodiscard]] std::string renderMainPageText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    [[nodiscard]] std::string screenshot(std::function<std::string(line_offset)> const& postLine = {}) const;

    void crlf() { linefeed(getMargin().hori.from); }
    void crlfIfWrapPending();

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();

    // DECSEL
    void selectiveEraseToBeginOfLine();
    void selectiveEraseToEndOfLine();
    void selectiveEraseLine(line_offset line);

    // DECSED
    void selectiveEraseToBeginOfScreen();
    void selectiveEraseToEndOfScreen();
    void selectiveEraseScreen();

    void selectiveEraseArea(rect area);

    void selectiveErase(line_offset line, column_offset begin, column_offset end);
    [[nodiscard]] bool containsProtectedCharacters(line_offset line,
                                                   column_offset begin,
                                                   column_offset end) const;

    void eraseCharacters(ColumnCount n);  // ECH
    void insertCharacters(ColumnCount n); // ICH
    void deleteCharacters(ColumnCount n); // DCH
    void deleteColumns(ColumnCount n);    // DECDC
    void insertLines(LineCount n);        // IL
    void insertColumns(ColumnCount n);    // DECIC

    void copyArea(rect sourceArea, int page, cell_location targetTopLeft, int targetPage);

    void eraseArea(int top, int left, int bottom, int right);

    void fillArea(char32_t ch, int top, int left, int bottom, int right);

    void deleteLines(LineCount n); // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorTo(line_offset line, column_offset column) override; // CUP
    void moveCursorBackward(ColumnCount n);                             // CUB
    void moveCursorDown(LineCount n);                                   // CUD
    void moveCursorForward(ColumnCount n);                              // CUF
    void moveCursorToBeginOfLine();                                     // CR
    void moveCursorToColumn(column_offset n);                           // CHA
    void moveCursorToLine(line_offset n);                               // VPA
    void moveCursorToNextLine(LineCount n);                             // CNL
    void moveCursorToNextTab();                                         // HT
    void moveCursorToPrevLine(LineCount n);                             // CPL
    void moveCursorUp(LineCount n);                                     // CUU

    void cursorBackwardTab(tab_stop_count count);      // CBT
    void cursorForwardTab(tab_stop_count count);       // CHT
    void backspace();                                  // BS
    void horizontalTabClear(HorizontalTabClear which); // TBC
    void horizontalTabSet();                           // HTS

    void index();        // IND
    void reverseIndex(); // RI

    void setMark();
    void deviceStatusReport();           // DSR
    void reportCursorPosition();         // CPR
    void reportExtendedCursorPosition(); // DECXCPR
    void selectConformanceLevel(vt_type level);
    void requestDynamicColor(dynamic_color_name name);
    void requestCapability(capabilities::code code);
    void requestCapability(std::string_view name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& url); // OSC 7

    void hyperlink(std::string id, std::string uri);                   // OSC 8
    void notify(std::string const& title, std::string const& content); // OSC 777

    void captureBuffer(LineCount lineCount, bool logicalLines);

    void setForegroundColor(color color);
    void setBackgroundColor(color color);
    void setUnderlineColor(color color);
    void setCursorStyle(cursor_display display, cursor_shape shape);
    void setGraphicsRendition(graphics_rendition rendition);
    void screenAlignmentPattern();
    void applicationKeypadMode(bool enable);
    void designateCharset(charset_table table, charset_id charset);
    void singleShiftSelect(charset_table table);
    void requestPixelSize(RequestPixelSize area);
    void requestCharacterSize(RequestPixelSize area);
    void sixelImage(image_size pixelSize, image::data&& rgbaData);
    void requestStatusString(RequestStatusString value);
    void requestTabStops();
    void resetDynamicColor(dynamic_color_name name);
    void setDynamicColor(dynamic_color_name name, rgb_color color);
    void inspect();
    void smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value);
    // }}}

    std::shared_ptr<image const> uploadImage(image_format format, image_size imageSize, image::data&& pixmap);

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
    void renderImage(std::shared_ptr<image const> image,
                     cell_location topLeft,
                     grid_size gridSize,
                     pixel_coordinate imageOffset,
                     image_size imageSize,
                     image_alignment alignmentPolicy,
                     image_resize resizePolicy,
                     bool autoScroll);

    void inspect(std::string const& message, std::ostream& os) const override;

    // for DECSC and DECRC
    void saveModes(std::vector<dec_mode> const& modes);
    void restoreModes(std::vector<dec_mode> const& modes);
    void requestAnsiMode(unsigned int mode);
    void requestDECMode(unsigned int mode);

    [[nodiscard]] PageSize pageSize() const noexcept { return _grid.pageSize(); }
    [[nodiscard]] image_size pixelSize() const noexcept { return _state.cellPixelSize * _settings.pageSize; }

    [[nodiscard]] margin getMargin() const noexcept override { return _grid.getMargin(); }
    [[nodiscard]] margin& getMargin() noexcept override { return _grid.getMargin(); }

    [[nodiscard]] bool isFullHorizontalMargins() const noexcept
    {
        return getMargin().hori.to.value + 1 == pageSize().columns.value;
    }

    [[nodiscard]] bool isCursorInsideMargins() const noexcept;

    [[nodiscard]] constexpr cell_location realCursorPosition() const noexcept { return _cursor.position; }

    [[nodiscard]] constexpr cell_location logicalCursorPosition() const noexcept
    {
        if (!_cursor.originMode)
            return realCursorPosition();
        else
            return cell_location { _cursor.position.line - getMargin().vert.from,
                                   _cursor.position.column - getMargin().hori.from };
    }

    [[nodiscard]] constexpr cell_location origin() const noexcept
    {
        if (!_cursor.originMode)
            return {};

        return { getMargin().vert.from, getMargin().hori.from };
    }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    [[nodiscard]] cell_location toRealCoordinate(cell_location pos) const noexcept
    {
        if (!_cursor.originMode)
            return pos;
        else
            return { pos.line + getMargin().vert.from, pos.column + getMargin().hori.from };
    }

    [[nodiscard]] line_offset applyOriginMode(line_offset line) const noexcept
    {
        if (!_cursor.originMode)
            return line;
        else
            return line + getMargin().vert.from;
    }

    [[nodiscard]] column_offset applyOriginMode(column_offset column) const noexcept
    {
        if (!_cursor.originMode)
            return column;
        else
            return column + getMargin().hori.from;
    }

    [[nodiscard]] rect applyOriginMode(rect area) const noexcept
    {
        if (!_cursor.originMode)
            return area;

        auto const top = top::cast_from(area.top.value + getMargin().vert.from.value);
        auto const left = left::cast_from(area.top.value + getMargin().hori.from.value);
        auto const bottom = bottom::cast_from(area.bottom.value + getMargin().vert.from.value);
        auto const right = right::cast_from(area.right.value + getMargin().hori.from.value);
        // TODO: Should this automatically clamp to margin's botom/right values?

        return rect { top, left, bottom, right };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    [[nodiscard]] cell_location clampCoordinate(cell_location coord) const noexcept
    {
        if (_cursor.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    [[nodiscard]] cell_location clampToOrigin(cell_location coord) const noexcept
    {
        return { std::clamp(coord.line, line_offset { 0 }, getMargin().vert.to),
                 std::clamp(coord.column, column_offset { 0 }, getMargin().hori.to) };
    }

    [[nodiscard]] line_offset clampedLine(line_offset line) const noexcept
    {
        return std::clamp(line, line_offset(0), boxed_cast<line_offset>(_grid.pageSize().lines) - 1);
    }

    [[nodiscard]] column_offset clampedColumn(column_offset column) const noexcept
    {
        return std::clamp(column, column_offset(0), boxed_cast<column_offset>(_grid.pageSize().columns) - 1);
    }

    [[nodiscard]] cell_location clampToScreen(cell_location coord) const noexcept
    {
        return { clampedLine(coord.line), clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] bool contains(cell_location coord) const noexcept override
    {
        return line_offset(0) <= coord.line && coord.line < boxed_cast<line_offset>(_settings.pageSize.lines)
               && column_offset(0) <= coord.column
               && coord.column <= boxed_cast<column_offset>(_settings.pageSize.columns);
    }

    [[nodiscard]] std::optional<cell_location> search(std::u32string_view searchText,
                                                      cell_location startPosition) override;
    [[nodiscard]] std::optional<cell_location> searchReverse(std::u32string_view searchText,
                                                             cell_location startPosition) override;

    [[nodiscard]] Cell& usePreviousCell() noexcept
    {
        return useCellAt(_lastCursorPosition.line, _lastCursorPosition.column);
    }

    void updateCursorIterator() noexcept override
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        _currentLine = &_grid.lineAt(_cursor.position.line);
#endif
    }

    [[nodiscard]] line<Cell>& currentLine() noexcept
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        return *_currentLine;
#else
        return _grid.lineAt(_cursor.position.line);
#endif
    }

    [[nodiscard]] line<Cell> const& currentLine() const noexcept
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        return *_currentLine;
#else
        return _grid.lineAt(_cursor.position.line);
#endif
    }

    [[nodiscard]] Cell& useCurrentCell() noexcept { return currentLine().useCellAt(_cursor.position.column); }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] Cell& at(line_offset line, column_offset column) noexcept
    {
        return _grid.useCellAt(line, column);
    }
    [[nodiscard]] Cell& useCellAt(line_offset line, column_offset column) noexcept
    {
        return _grid.lineAt(line).useCellAt(column);
    }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    [[nodiscard]] Cell const& at(line_offset line, column_offset column) const noexcept
    {
        return _grid.at(line, column);
    }

    [[nodiscard]] Cell& at(cell_location p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] Cell& useCellAt(cell_location p) noexcept { return useCellAt(p.line, p.column); }
    [[nodiscard]] Cell const& at(cell_location p) const noexcept { return _grid.at(p.line, p.column); }

    [[nodiscard]] std::string const& windowTitle() const noexcept { return _state.windowTitle; }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<line_offset> findMarkerDownwards(line_offset startLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn startLine the line number of the current cursor (1..N) for screen area, or
    ///                   (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<line_offset> findMarkerUpwards(line_offset startLine) const;

    /// ScreenBuffer's type, such as main screen or alternate screen.
    [[nodiscard]] screen_type bufferType() const noexcept { return _state.screenType; }

    void scrollUp(LineCount n) { scrollUp(n, getMargin()); }
    void scrollDown(LineCount n) { scrollDown(n, getMargin()); }

    void verifyState() const override;

    [[nodiscard]] Grid<Cell> const& grid() const noexcept { return _grid; }
    [[nodiscard]] Grid<Cell>& grid() noexcept { return _grid; }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    [[nodiscard]] bool isLineWrapped(line_offset lineNumber) const noexcept
    {
        return _grid.isLineWrapped(lineNumber);
    }

    [[nodiscard]] color_palette& colorPalette() noexcept { return _state.colorPalette; }
    [[nodiscard]] color_palette const& colorPalette() const noexcept { return _state.colorPalette; }

    [[nodiscard]] color_palette& defaultColorPalette() noexcept { return _state.defaultColorPalette; }
    [[nodiscard]] color_palette const& defaultColorPalette() const noexcept
    {
        return _state.defaultColorPalette;
    }

    [[nodiscard]] bool isCellEmpty(cell_location position) const noexcept override
    {
        return _grid.lineAt(position.line).cellEmptyAt(position.column);
    }

    [[nodiscard]] bool compareCellTextAt(cell_location position, char codepoint) const noexcept override
    {
        auto const& cell = _grid.lineAt(position.line).inflatedBuffer().at(position.column.as<size_t>());
        return CellUtil::compareText(cell, codepoint);
    }

    // IMPORTANT: Invokig inflatedBuffer() is expensive. This function should be invoked with caution.
    [[nodiscard]] std::string cellTextAt(cell_location position) const noexcept override
    {
        return _grid.lineAt(position.line).inflatedBuffer().at(position.column.as<size_t>()).toUtf8();
    }

    [[nodiscard]] line_flags lineFlagsAt(line_offset line) const noexcept override
    {
        return _grid.lineAt(line).flags();
    }

    void enableLineFlags(line_offset lineOffset, line_flags flags, bool enable) noexcept override
    {
        _grid.lineAt(lineOffset).setFlag(flags, enable);
    }

    [[nodiscard]] bool isLineFlagEnabledAt(line_offset line, line_flags flags) const noexcept override
    {
        return _grid.lineAt(line).isFlagEnabled(flags);
    }

    [[nodiscard]] std::string lineTextAt(line_offset line,
                                         bool stripLeadingSpaces,
                                         bool stripTrailingSpaces) const noexcept override
    {
        return _grid.lineAt(line).toUtf8Trimmed(stripLeadingSpaces, stripTrailingSpaces);
    }

    [[nodiscard]] bool isLineEmpty(line_offset line) const noexcept override
    {
        return _grid.lineAt(line).empty();
    }

    [[nodiscard]] uint8_t cellWidthAt(cell_location position) const noexcept override
    {
        return _grid.lineAt(position.line).cellWidthAt(position.column);
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept override { return _grid.historyLineCount(); }

    [[nodiscard]] hyperlink_id hyperlinkIdAt(cell_location position) const noexcept override
    {
        auto const& line = _grid.lineAt(position.line);
        if (line.isTrivialBuffer())
        {
            trivial_line_buffer const& lineBuffer = line.trivialBuffer();
            return lineBuffer.hyperlink;
        }
        return at(position).hyperlink();
    }

    [[nodiscard]] std::shared_ptr<hyperlink_info const> hyperlinkAt(cell_location pos) const noexcept override
    {
        return _state.hyperlinks.hyperlinkById(hyperlinkIdAt(pos));
    }

    [[nodiscard]] hyperlink_storage const& hyperlinks() const noexcept { return _state.hyperlinks; }

    void resetInstructionCounter() noexcept { _state.instructionCounter = 0; }
    [[nodiscard]] uint64_t instructionCounter() const noexcept { return _state.instructionCounter; }
    [[nodiscard]] char32_t precedingGraphicCharacter() const noexcept
    {
        return _state.parser.precedingGraphicCharacter();
    }

    void applyAndLog(function_definition const& function, sequence const& seq);
    [[nodiscard]] ApplyResult apply(function_definition const& function, sequence const& seq);

    void fail(std::string const& message) const override;

    void hardReset();
    void applyPageSizeToMainDisplay(PageSize pageSize);

    void saveCursor() override;
    void restoreCursor() override;
    void restoreCursor(Cursor const& savedCursor);

  private:
    void writeTextInternal(char32_t codepoint);

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
    void linefeed(column_offset column);

    void writeCharToCurrentAndAdvance(char32_t codepoint) noexcept;
    void clearAndAdvance(int offset) noexcept;

    void scrollUp(LineCount n, graphics_attributes sgr, margin margin);
    void scrollUp(LineCount n, margin margin);
    void scrollDown(LineCount n, margin margin);
    void insertChars(line_offset lineOffset, ColumnCount columnsToInsert);
    void deleteChars(line_offset lineOffset, column_offset column, ColumnCount columnsToDelete);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(column_offset n);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(sequence const& seq);

    Terminal& _terminal;
    Settings& _settings;
    TerminalState& _state;
    Grid<Cell> _grid;

    cell_location _lastCursorPosition {};

#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
    Line<Cell>* _currentLine = nullptr;
#endif
    std::unique_ptr<sixel_image_builder> _sixelImageBuilder;
};

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline void Screen<Cell>::scrollUp(LineCount n, margin margin)
{
    scrollUp(n, cursor().graphicsRendition, margin);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline bool Screen<Cell>::isContiguousToCurrentLine(std::string_view continuationChars) const noexcept
{
    auto const& line = currentLine();
    return line.isTrivialBuffer() && line.trivialBuffer().text.view().end() == continuationChars.begin();
}

} // namespace terminal
