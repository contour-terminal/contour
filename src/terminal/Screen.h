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

#include <terminal/Capabilities.h>
#include <terminal/Charset.h>
#include <terminal/Color.h>
#include <terminal/Grid.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/Parser.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Sequencer.h>
#include <terminal/VTType.h>

#include <crispy/algorithm.h>
#include <crispy/size.h>
#include <crispy/span.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/width.h>
#include <unicode/utf8.h>

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

namespace terminal {

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes {
  public:
    void set(AnsiMode _mode, bool _enabled)
    {
        ansi_.set(static_cast<size_t>(_mode), _enabled);
    }

    void set(DECMode  _mode, bool _enabled)
    {
        dec_.set(static_cast<size_t>(_mode), _enabled);
    }

    bool enabled(AnsiMode _mode) const noexcept { return ansi_.test(static_cast<size_t>(_mode)); }

    bool enabled(DECMode _mode) const noexcept { return dec_.test(static_cast<size_t>(_mode)); }

    void save(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode : _modes)
            savedModes_[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode : _modes)
        {
            if (auto i = savedModes_.find(mode); i != savedModes_.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

  private:
    // TODO: make this a vector<bool> by casting from Mode, but that requires ensured small linearity in Mode enum values.
    std::bitset<32> ansi_; // AnsiMode
    std::bitset<2048> dec_; // DECMode
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes
};
// }}}

// {{{ Cursor
/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    Coordinate position{1, 1};
    bool autoWrap = true; // false;
    bool originMode = false;
    bool visible = true;
    GraphicsAttributes graphicsRendition{};
    CharsetMapping charsets{};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
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
class Screen : public capabilities::StaticDatabase {
  public:
    /**
     * Initializes the screen with the given screen size and callbaks.
     *
     * @param _size screen dimensions in number of characters per line and number of lines.
     * @param _eventListener Interface to some VT sequence related callbacks.
     * @param _logRaw whether or not to log raw VT sequences.
     * @param _logTrace whether or not to log VT sequences in trace mode.
     * @param _maxHistoryLineCount number of lines the history must not exceed.
     */
    Screen(PageSize _size,
           ScreenEvents& _eventListener,
           bool _logRaw = false,
           bool _logTrace = false,
           std::optional<LineCount> _maxHistoryLineCount = std::nullopt,
           ImageSize _maxImageSize = ImageSize{Width(800), Height(600)},
           int _maxImageColorRegisters = 256,
           bool _sixelCursorConformance = true,
           ColorPalette _colorPalette = {},
           bool _reflowOnResize = true
    );

    using StaticDatabase::numericCapability;
    unsigned numericCapability(capabilities::Code _cap) const override;

    void setLogTrace(bool _enabled) { logTrace_ = _enabled; }
    bool logTrace() const noexcept { return logTrace_; }
    void setLogRaw(bool _enabled) { logRaw_ = _enabled; }
    bool logRaw() const noexcept { return logRaw_; }

    void setMaxImageColorRegisters(unsigned _value) noexcept { sequencer_.setMaxImageColorRegisters(_value); }
    void setSixelCursorConformance(bool _value) noexcept { sixelCursorConformance_ = _value; }

    void setRespondToTCapQuery(bool _enable) { respondToTCapQuery_ = _enable; }

    constexpr ImageSize cellPixelSize() const noexcept { return cellPixelSize_; }

    constexpr void setCellPixelSize(ImageSize _cellPixelSize)
    {
        cellPixelSize_ = _cellPixelSize;
    }

    void setTerminalId(VTType _id) noexcept
    {
        terminalId_ = _id;
    }

    void setMaxHistoryLineCount(std::optional<LineCount> _maxHistoryLineCount);
    std::optional<LineCount> maxHistoryLineCount() const noexcept { return grid().maxHistoryLineCount(); }

    LineCount historyLineCount() const noexcept { return grid().historyLineCount(); }

    /// Writes given data into the screen.
    void write(std::string_view _data);
    void write(std::u32string_view _data);

    void writeText(char32_t _char);
    void writeText(std::string_view _chars);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename Renderer>
    void render(Renderer&& _render, std::optional<StaticScrollbackPosition> _scrollOffset = std::nullopt) const
    {
        activeGrid_->render(std::forward<Renderer>(_render), _scrollOffset);
    }

    /// Renders a single text line.
    std::string renderTextLine(int _row) const;

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    std::string renderText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot(std::function<std::string(int)> const& _postLine = {}) const;

    void setFocus(bool _focused) { focused_ = _focused; }
    bool focused() const noexcept { return focused_; }

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();

    void clearScrollbackBuffer();

    void eraseCharacters(ColumnCount _n);  // ECH
    void insertCharacters(ColumnCount _n); // ICH
    void deleteCharacters(ColumnCount _n); // DCH
    void deleteColumns(ColumnCount _n);    // DECDC
    void insertLines(LineCount _n);      // IL
    void insertColumns(ColumnCount _n);    // DECIC

    void copyArea(
        int _top, int _left, int _bottom, int _right, int _page,
        int _targetTop, int _targetLeft, int _targetPage
    );

    void eraseArea(int _top, int _left, int _bottom, int _right);

    void fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right);

    void deleteLines(LineCount _n);      // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorBackward(ColumnCount _n);    // CUB
    void moveCursorDown(LineCount _n);          // CUD
    void moveCursorForward(ColumnCount _n);     // CUF
    void moveCursorToBeginOfLine();             // CR
    void moveCursorToColumn(ColumnPosition _n); // CHA
    void moveCursorToLine(LinePosition _n);     // VPA
    void moveCursorToNextLine(LineCount _n);    // CNL
    void moveCursorToNextTab();                 // HT
    void moveCursorToPrevLine(LineCount _n);    // CPL
    void moveCursorUp(LineCount _n);            // CUU

    void cursorBackwardTab(TabStopCount _n);    // CBT
    void cursorForwardTab(TabStopCount _n);     // CHT
    void backspace();                           // BS
    void horizontalTabClear(HorizontalTabClear _which); // TBC
    void horizontalTabSet();                    // HTS

    void index(); // IND
    void reverseIndex(); // RI

    void setMark();
    void deviceStatusReport();            // DSR
    void reportCursorPosition();          // CPR
    void reportExtendedCursorPosition();  // DECXCPR
    void selectConformanceLevel(VTType _level);
    void requestDynamicColor(DynamicColorName _name);
    void requestCapability(capabilities::Code _code);
    void requestCapability(std::string_view _name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& _url);             // OSC 7

    /// @returns either an empty string or a file:// URL of the last set working directory.
    std::string const& currentWorkingDirectory() const noexcept { return currentWorkingDirectory_; }

    void hyperlink(std::string const& _id, std::string const& _uri);      // OSC 8
    void notify(std::string const& _title, std::string const& _content);  // OSC 777

    void captureBuffer(int _numLines, bool _logicalLines);

    void setForegroundColor(Color _color);
    void setBackgroundColor(Color _color);
    void setUnderlineColor(Color _color);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setGraphicsRendition(GraphicsRendition _rendition);
    void setTopBottomMargin(std::optional<int> _top, std::optional<int> _bottom);
    void setLeftRightMargin(std::optional<int> _left, std::optional<int> _right);
    void screenAlignmentPattern();
    void sendMouseEvents(MouseProtocol _protocol, bool _enable);
    void applicationKeypadMode(bool _enable);
    void designateCharset(CharsetTable _table, CharsetId _charset);
    void singleShiftSelect(CharsetTable _table);
    void requestPixelSize(RequestPixelSize _area);
    void requestCharacterSize(RequestPixelSize _area);
    void sixelImage(ImageSize _pixelSize, Image::Data&& _rgba);
    void requestStatusString(RequestStatusString _value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName _name);
    void setDynamicColor(DynamicColorName _name, RGBColor _color);
    void dumpState();
    void smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value);
    // }}}

    void setMaxImageSize(ImageSize _effective, ImageSize _limit)
    {
        maxImageSize_ = _effective;
        maxImageSizeLimit_ = _limit;
    }

    ImageSize maxImageSize() const noexcept { return maxImageSize_; }
    ImageSize maxImageSizeLimit() const noexcept { return maxImageSizeLimit_; }

    std::shared_ptr<Image const> uploadImage(ImageFormat _format, ImageSize _imageSize, Image::Data&& _pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p _imageRef Reference to the image to be rendered.
     * @p _topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p _gridSize Screen grid size to span the image into.
     * @p _imageOffset Offset into the image in screen grid coordinate to start rendering from.
     * @p _imageSize Size of the full image in Screen grid coordinates.
     * @p _alignmentPolicy render the image using the given image alignment policy.
     * @p _resizePolicy render the image using the given image resize policy.
     * @p _autoScroll Boolean indicating whether or not the screen should scroll if the image cannot be fully displayed otherwise.
     */
    void renderImage(std::shared_ptr<Image const> const& _imageRef,
                     Coordinate _topLeft,
                     GridSize _gridSize,
                     Coordinate _imageOffset,
                     ImageSize _imageSize,
                     ImageAlignment _alignmentPolicy,
                     ImageResize _resizePolicy,
                     bool _autoScroll);

    void dumpState(std::string const& _message, std::ostream& _os) const;

    // reset screen
    void resetSoft();
    void resetHard();

    // for DECSC and DECRC
    void setMode(AnsiMode _mode, bool _enabled);
    void setMode(DECMode _mode, bool _enabled);
    void saveCursor();
    void restoreCursor();
    void restoreCursor(Cursor const& _savedCursor);
    void saveModes(std::vector<DECMode> const& _modes);
    void restoreModes(std::vector<DECMode> const& _modes);
    void requestAnsiMode(int _mode);
    void requestDECMode(int _mode);

    PageSize size() const noexcept { return size_; }
    void resize(PageSize _newSize);

    /// Implements semantics for  DECCOLM / DECSCPP.
    void resizeColumns(ColumnCount _newColumnCount, bool _clear);

    bool isCursorInsideMargins() const noexcept
    {
        bool const insideVerticalMargin = margin_.vertical.contains(cursor_.position.row);
        bool const insideHorizontalMargin = !isModeEnabled(DECMode::LeftRightMargin)
                                         || margin_.horizontal.contains(cursor_.position.column);
        return insideVerticalMargin && insideHorizontalMargin;
    }

    constexpr Coordinate realCursorPosition() const noexcept { return cursor_.position; }

    constexpr Coordinate cursorPosition() const noexcept {
        if (!cursor_.originMode)
            return realCursorPosition();
        else
            return Coordinate{
                cursor_.position.row - margin_.vertical.from + 1,
                cursor_.position.column - margin_.horizontal.from + 1
            };
    }

    constexpr Coordinate origin() const noexcept {
        if (cursor_.originMode)
            return {margin_.vertical.from, margin_.horizontal.from};
        else
            return {1, 1};
    }

    Cursor const& cursor() const noexcept { return cursor_; }

    int wrapPending() const noexcept { return wrapPending_; }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is enabled.
    Coordinate toRealCoordinate(Coordinate const& pos) const noexcept
    {
        if (!cursor_.originMode)
            return pos;
        else
            return { pos.row + margin_.vertical.from - 1, pos.column + margin_.horizontal.from - 1 };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    Coordinate clampCoordinate(Coordinate const& coord) const noexcept
    {
        if (!cursor_.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    Coordinate clampToOrigin(Coordinate const& coord) const noexcept
    {
        return {
            std::clamp(coord.row, int{0}, margin_.vertical.length()),
            std::clamp(coord.column, int{0}, margin_.horizontal.length())
        };
    }

    Coordinate clampToScreen(Coordinate const& coord) const noexcept
    {
        return {
            std::clamp(coord.row, int{1}, unbox<int>(size_.lines)),
            std::clamp(coord.column, int{1}, unbox<int>(size_.columns))
        };
    }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(Coordinate _coord) const noexcept
    {
        return 1 <= _coord.row && _coord.row <= unbox<int>(size_.lines)
            && 1 <= _coord.column && _coord.column <= unbox<int>(size_.columns);
    }

    Cell& lastPosition() noexcept { return grid().at(lastCursorPosition_); }
    Cell const& lastPosition() const noexcept { return grid().at(lastCursorPosition_); }

    auto currentColumn() noexcept
    {
        return std::next(currentLine_->begin(), cursor_.position.column - 1);
    }

    auto currentColumn() const noexcept
    {
        return std::next(currentLine_->cbegin(), cursor_.position.column - 1);
    }

    Cell const& currentCell() const noexcept
    {
        return (*currentLine_)[cursor_.position.column - 1];
    }

    Cell& currentCell() noexcept
    {
        return (*currentLine_)[cursor_.position.column - 1];
    }

    Cell& currentCell(Cell value)
    {
        return (*currentLine_)[cursor_.position.column - 1] = std::move(value);
    }

    void moveCursorTo(Coordinate to);

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(Coordinate const& _coord) noexcept { return grid().at(_coord); }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(Coordinate const& _coord) const noexcept { return grid().at(_coord); }

    bool isPrimaryScreen() const noexcept { return activeGrid_ == &grids_[0]; }
    bool isAlternateScreen() const noexcept { return activeGrid_ == &grids_[1]; }

    bool isModeEnabled(AnsiMode m) const noexcept { return modes_.enabled(m); }
    bool isModeEnabled(DECMode m) const noexcept { return modes_.enabled(m); }

    bool isModeEnabled(std::variant<AnsiMode, DECMode> m) const {
        if (std::holds_alternative<AnsiMode>(m))
            return modes_.enabled(std::get<AnsiMode>(m));
        else
            return modes_.enabled(std::get<DECMode>(m));
    }

    bool verticalMarginsEnabled() const noexcept { return isModeEnabled(DECMode::Origin); }
    bool horizontalMarginsEnabled() const noexcept { return isModeEnabled(DECMode::LeftRightMargin); }

    Margin const& margin() const noexcept { return margin_; }

    auto scrollbackLines() const noexcept { return grid().scrollbackLines(); }

    void setTabWidth(uint8_t _value) { tabWidth_ = _value; }

    /**
     * Returns the n'th saved line into the history scrollback buffer.
     *
     * @param _lineNumberIntoHistory the 1-based offset into the history buffer.
     *
     * @returns the textual representation of the n'th line into the history.
     */
    std::string renderHistoryTextLine(int _lineNumberIntoHistory) const;

    std::string const& windowTitle() const noexcept { return windowTitle_; }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<int> findMarkerForward(int _currentCursorLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<int> findMarkerBackward(int _currentCursorLine) const;

    /// ScreenBuffer's type, such as main screen or alternate screen.
    ScreenType bufferType() const noexcept { return screenType_; }

    bool synchronizeOutput() const noexcept { return false; } // TODO

    ScreenEvents& eventListener() noexcept { return eventListener_; }
    ScreenEvents const& eventListener()  const noexcept { return eventListener_; }

    void setWindowTitle(std::string const& _title);
    void saveWindowTitle();
    void restoreWindowTitle();

    void setMaxImageSize(ImageSize _size) noexcept { sequencer_.setMaxImageSize(_size); }

    void scrollUp(LineCount n) { scrollUp(n, margin_); }
    void scrollDown(LineCount n) { scrollDown(n, margin_); }

    void verifyState() const;

    // interactive replies
    void reply(std::string const& message)
    {
        eventListener_.reply(message);
    }

    template <typename... Args>
    void reply(std::string const& fmt, Args&&... args)
    {
        reply(fmt::format(fmt, std::forward<Args>(args)...));
    }

    /// @returns the primary screen's grid.
    Grid& primaryGrid() noexcept { return grids_[0]; }

    /// @returns the alternate  screen's grid.
    Grid& alternateGrid() noexcept { return grids_[1]; }

    /// @returns the primary screen's grid if primary screen is active.
    Grid const& grid() const noexcept { return *activeGrid_; }

    /// @returns the primary screen's grid if primary screen is active.
    Grid& grid() noexcept { return *activeGrid_; }

    /// @returns the primary screen's grid if alternate screen is active, and the alternate screen's grid otherwise.
    Grid& backgroundGrid() noexcept { return isPrimaryScreen() ? alternateGrid() : primaryGrid(); }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    bool lineWrapped(int _lineNumber) const { return activeGrid_->absoluteLineAt(_lineNumber).wrapped(); }

    int toAbsoluteLine(int _relativeLine) const noexcept { return activeGrid_->toAbsoluteLine(_relativeLine); }
    Coordinate toAbsolute(Coordinate _coord) const noexcept { return {activeGrid_->toAbsoluteLine(_coord.row), _coord.column}; }

    int toRelativeLine(int _absoluteLine) const noexcept { return activeGrid_->toRelativeLine(_absoluteLine); }
    Coordinate toRelative(Coordinate _coord) const noexcept { return {activeGrid_->toRelativeLine(_coord.row), _coord.column}; }

    ColorPalette& colorPalette() noexcept { return colorPalette_; }
    ColorPalette const& colorPalette() const noexcept { return colorPalette_; }

    ColorPalette& defaultColorPalette() noexcept { return defaultColorPalette_; }
    ColorPalette const& defaultColorPalette() const noexcept { return defaultColorPalette_; }

  private:
    void setBuffer(ScreenType _type);

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p _column.
    void linefeed(int _column);

    void writeCharToCurrentAndAdvance(char32_t _codepoint);
    void clearAndAdvance(int _offset);

    void fail(std::string const& _message) const;

    void updateCursorIterators()
    {
        currentLine_ = next(begin(grid().mainPage()), cursor_.position.row - 1);
    }

    /// @returns an iterator to @p _n columns after column @p _begin.
    ColumnIterator columnIteratorAt(ColumnIterator _begin, int _n)
    {
        return next(_begin, _n - 1);
    }

    /// @returns an iterator to the real column number @p _n.
    ColumnIterator columnIteratorAt(int _n)
    {
        return columnIteratorAt(std::begin(*currentLine_), _n);
    }

    /// @returns an iterator to the real column number @p _n.
    ColumnIterator columnIteratorAt(int _n) const
    {
        return const_cast<Screen*>(this)->columnIteratorAt(_n);
    }

    void scrollUp(LineCount n, Margin const& margin);
    void scrollDown(LineCount n, Margin const& margin);
    void insertChars(int _lineNo, ColumnCount _n);
    void deleteChars(int _lineNo, ColumnCount _n);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnPosition _n);

    // private fields
    //
    ScreenEvents& eventListener_;

    bool logRaw_ = false;
    bool logTrace_ = false;
    bool focused_ = true;

    ImageSize cellPixelSize_; ///< contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.

    VTType terminalId_ = VTType::VT525;

    Modes modes_;
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes

    ColorPalette defaultColorPalette_;
    ColorPalette colorPalette_;

    int maxImageColorRegisters_;
    ImageSize maxImageSize_;
    ImageSize maxImageSizeLimit_;
    std::shared_ptr<SixelColorPalette> imageColorPalette_;
    ImagePool imagePool_;

    Sequencer sequencer_;
    parser::Parser parser_;
    int64_t instructionCounter_ = 0;

    PageSize size_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    bool sixelCursorConformance_ = true;

    // XXX moved from ScreenBuffer
    Margin margin_;
    int wrapPending_ = 0;
    uint8_t tabWidth_{8};
    std::vector<ColumnPosition> tabs_;

    // main/alt screen and history
    //
    //std::array<Lines, 2> lines_;
    ScreenType screenType_ = ScreenType::Main;
    // Lines* activeBuffer_;
    // Lines savedLines_{};

    bool allowReflowOnResize_;
    std::array<Grid, 2> grids_;
    Grid* activeGrid_;

    // cursor related
    //
    Cursor cursor_;
    Cursor savedCursor_;
    Cursor savedPrimaryCursor_; //!< saved cursor of primary-screen when switching to alt-screen.
    LineIterator currentLine_;
    Coordinate lastCursorPosition_;

    CursorDisplay cursorDisplay_ = CursorDisplay::Steady;
    CursorShape cursorShape_ = CursorShape::Block;

    std::string currentWorkingDirectory_ = {};

    // Hyperlink related
    //
#if defined(LIBTERMINAL_HYPERLINKS)
    HyperlinkRef currentHyperlink_ = {};
    std::unordered_map<std::string, HyperlinkRef> hyperlinks_; // TODO: use a deque<> instead, always push_back, lookup reverse, evict in front.
#endif

    // experimental features
    //
    bool respondToTCapQuery_ = false;
};

}  // namespace terminal

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Margin::Range> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Margin::Range range, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}..{}", range.from, range.to);
        }
    };

    template <>
    struct formatter<terminal::Cursor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Cursor cursor, FormatContext& ctx)
        {
            return format_to(ctx.out(), "({}:{}{})", cursor.position.row, cursor.position.column, cursor.visible ? "" : ", (invis)");
        }
    };

    template <>
    struct formatter<terminal::Cell> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Cell const& cell, FormatContext& ctx)
        {
            std::string codepoints;
            for (auto const i : crispy::times(cell.codepointCount()))
            {
                if (i)
                    codepoints += ", ";
                codepoints += fmt::format("{:02X}", static_cast<unsigned>(cell.codepoint(i)));
            }
            return format_to(ctx.out(), "(chars={}, width={})", codepoints, cell.width());
        }
    };

    template <>
    struct formatter<terminal::ScreenType> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::ScreenType value, FormatContext& ctx)
        {
            switch (value)
            {
                case terminal::ScreenType::Main:
                    return format_to(ctx.out(), "main");
                case terminal::ScreenType::Alternate:
                    return format_to(ctx.out(), "alternate");
            }
            return format_to(ctx.out(), "({})", static_cast<unsigned>(value));
        }
    };

    template <>
    struct formatter<terminal::CellFlags> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::CellFlags const& _mask, FormatContext& ctx)
        {
            using Mask = terminal::CellFlags;
            auto constexpr mappings = std::array<std::pair<Mask, std::string_view>, 12>{
                std::pair{Mask::Bold, "bold"},
                std::pair{Mask::Faint, "faint"},
                std::pair{Mask::Italic, "italic"},
                std::pair{Mask::Underline, "underline"},
                std::pair{Mask::Blinking, "blinking"},
                std::pair{Mask::Inverse, "inverse"},
                std::pair{Mask::Hidden, "hidden"},
                std::pair{Mask::CrossedOut, "crossedOut"},
                std::pair{Mask::DoublyUnderlined, "doublyUnderlined"},
                std::pair{Mask::CurlyUnderlined, "curlyUnderlined"},
                std::pair{Mask::Framed, "framed"},
                std::pair{Mask::Overline, "overline"}
            };
            int i = 0;
            std::ostringstream os;
            for (auto const& mapping : mappings)
            {
                if (_mask & mapping.first)
                {
                    if (i) os << ", ";
                    os << mapping.second;
                }
            }
            return format_to(ctx.out(), "{}", os.str());
        }
    };
} // }}}
