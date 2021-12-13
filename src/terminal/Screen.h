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
#include <terminal/Cell.h>
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
    Coordinate position{LineOffset(0), ColumnOffset(0)};
    bool autoWrap = true; // false;
    bool originMode = false;
    bool visible = true;
    GraphicsAttributes graphicsRendition{};
    CharsetMapping charsets{};
    HyperlinkId hyperlink{};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};
// }}}

using ImageFragmentCache = crispy::LRUCache<ImageFragmentId, ImageFragment>;

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
template <typename EventListener>
class Screen: public capabilities::StaticDatabase {
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
           EventListener& _eventListener,
           bool _logRaw = false,
           bool _logTrace = false,
           LineCount _maxHistoryLineCount = LineCount(0),
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

    void setMaxHistoryLineCount(LineCount _maxHistoryLineCount);
    LineCount maxHistoryLineCount() const noexcept { return grid().maxHistoryLineCount(); }

    LineCount historyLineCount() const noexcept { return grid().historyLineCount(); }

    /// Writes given data into the screen.
    void write(std::string_view _data);
    void write(std::u32string_view _data);

    void writeText(char32_t _char);
    void writeText(std::string_view _chars);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename Renderer>
    void render(Renderer&& _render, ScrollOffset _scrollOffset = {}) const
    {
        activeGrid_->render(std::forward<Renderer>(_render), _scrollOffset);
    }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    std::string renderMainPageText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot(std::function<std::string(LineOffset)> const& _postLine = {}) const;

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

    void copyArea(Rect sourceArea, int page,
                  Coordinate targetTopLeft, int targetPage);

    void eraseArea(int _top, int _left, int _bottom, int _right);

    void fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right);

    void deleteLines(LineCount _n);      // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorBackward(ColumnCount _n);    // CUB
    void moveCursorDown(LineCount _n);          // CUD
    void moveCursorForward(ColumnCount _n);     // CUF
    void moveCursorToBeginOfLine();             // CR
    void moveCursorToColumn(ColumnOffset _n);   // CHA
    void moveCursorToLine(LineOffset _n);       // VPA
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

    void hyperlink(std::string _id, std::string _uri);                    // OSC 8
    void notify(std::string const& _title, std::string const& _content);  // OSC 777

    void captureBuffer(int _numLines, bool _logicalLines);

    void setForegroundColor(Color _color);
    void setBackgroundColor(Color _color);
    void setUnderlineColor(Color _color);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setGraphicsRendition(GraphicsRendition _rendition);
    void setTopBottomMargin(std::optional<LineOffset> _top, std::optional<LineOffset> _bottom);
    void setLeftRightMargin(std::optional<ColumnOffset> _left, std::optional<ColumnOffset> _right);
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

    Image const& uploadImage(ImageFormat _format, ImageSize _imageSize, Image::Data&& _pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p _imageId ID to the image to be rendered.
     * @p _topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p _gridSize Screen grid size to span the image into.
     * @p _imageOffset Offset into the image in screen grid coordinate to start rendering from.
     * @p _imageSize Size of the full image in Screen grid coordinates.
     * @p _alignmentPolicy render the image using the given image alignment policy.
     * @p _resizePolicy render the image using the given image resize policy.
     * @p _autoScroll Boolean indicating whether or not the screen should scroll if the image cannot be fully displayed otherwise.
     */
    void renderImage(ImageId _imageId,
                     Coordinate _topLeft,
                     GridSize _gridSize,
                     Coordinate _imageOffset,
                     ImageSize _imageSize,
                     ImageAlignment _alignmentPolicy,
                     ImageResize _resizePolicy,
                     bool _autoScroll);

    ImageFragmentCache const& imageFragments() const noexcept { return imageFragments_; }

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

    PageSize pageSize() const noexcept { return pageSize_; }
    void resize(PageSize _newSize);

    /// Implements semantics for  DECCOLM / DECSCPP.
    void resizeColumns(ColumnCount _newColumnCount, bool _clear);

    bool isCursorInsideMargins() const noexcept
    {
        bool const insideVerticalMargin = margin_.vertical.contains(cursor_.position.line);
        bool const insideHorizontalMargin = !isModeEnabled(DECMode::LeftRightMargin)
                                         || margin_.horizontal.contains(cursor_.position.column);
        return insideVerticalMargin && insideHorizontalMargin;
    }

    constexpr Coordinate realCursorPosition() const noexcept { return cursor_.position; }

    constexpr Coordinate logicalCursorPosition() const noexcept
    {
        if (!cursor_.originMode)
            return realCursorPosition();
        else
            return Coordinate{
                cursor_.position.line - margin_.vertical.from,
                cursor_.position.column - margin_.horizontal.from
            };
    }

    constexpr Coordinate origin() const noexcept
    {
        if (!cursor_.originMode)
            return {};

        return {
            margin_.vertical.from,
            margin_.horizontal.from
        };
    }

    Cursor const& cursor() const noexcept { return cursor_; }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is enabled.
    Coordinate toRealCoordinate(Coordinate pos) const noexcept
    {
        if (!cursor_.originMode)
            return pos;
        else
            return { pos.line + margin_.vertical.from, pos.column + margin_.horizontal.from };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    Coordinate clampCoordinate(Coordinate coord) const noexcept
    {
        if (!cursor_.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    Coordinate clampToOrigin(Coordinate coord) const noexcept
    {
        return {
            std::clamp(coord.line, LineOffset{0}, margin_.vertical.length().template as<LineOffset>() - LineOffset(1)),
            std::clamp(coord.column, ColumnOffset{0}, margin_.horizontal.length().template as<ColumnOffset>() - ColumnOffset(1))
        };
    }

    LineOffset clampedLine(LineOffset _line) const noexcept
    {
        return std::clamp(_line, LineOffset(0), pageSize_.lines.as<LineOffset>() - 1);
    }

    ColumnOffset clampedColumn(ColumnOffset _column) const noexcept
    {
        return std::clamp(_column, ColumnOffset(0), pageSize_.columns.as<ColumnOffset>() - 1);
    }

    Coordinate clampToScreen(Coordinate coord) const noexcept
    {
        return {
            clampedLine(coord.line),
            clampedColumn(coord.column)
        };
    }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(Coordinate _coord) const noexcept
    {
        return LineOffset(0) <= _coord.line && _coord.line < pageSize_.lines.as<LineOffset>()
            && ColumnOffset(0) <= _coord.column && _coord.column <= pageSize_.columns.as<ColumnOffset>();
    }

    Cell& usePreviousCell() noexcept { return useCellAt(lastCursorPosition_.line, lastCursorPosition_.column); }

    Line<Cell>& currentLine() { return grid().lineAt(cursor_.position.line); }
    Line<Cell> const& currentLine() const { return grid().lineAt(cursor_.position.line); }

    Cell& useCurrentCell() noexcept { return useCellAt(cursor_.position); }
    Cell const& currentCell() const noexcept { return at(cursor_.position); }

    void moveCursorTo(LineOffset _line, ColumnOffset _column);

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(LineOffset _line, ColumnOffset _column) noexcept { return grid().useCellAt(_line, _column); }
    Cell& useCellAt(LineOffset _line, ColumnOffset _column) noexcept { return grid().lineAt(_line).useCellAt(_column); }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(LineOffset _line, ColumnOffset _column) const noexcept { return grid().at(_line, _column); }

    Cell& at(Coordinate p) noexcept { return useCellAt(p.line, p.column); }
    Cell& useCellAt(Coordinate p) noexcept { return useCellAt(p.line, p.column); }
    Cell const& at(Coordinate p) const noexcept { return grid().at(p.line, p.column); }

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

    Margin margin() const noexcept { return margin_; }

    void setTabWidth(ColumnCount _value) { tabWidth_ = _value; }

    std::string const& windowTitle() const noexcept { return windowTitle_; }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<LineOffset> findMarkerDownwards(LineOffset _currentCursorLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<LineOffset> findMarkerUpwards(LineOffset _currentCursorLine) const;

    /// ScreenBuffer's type, such as main screen or alternate screen.
    ScreenType bufferType() const noexcept { return screenType_; }

    bool synchronizeOutput() const noexcept { return false; } // TODO

    EventListener& eventListener() noexcept { return eventListener_; }
    EventListener const& eventListener()  const noexcept { return eventListener_; }

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
    Grid<Cell>& primaryGrid() noexcept { return grids_[0]; }

    /// @returns the alternate  screen's grid.
    Grid<Cell>& alternateGrid() noexcept { return grids_[1]; }

    /// @returns the primary screen's grid if primary screen is active.
    Grid<Cell> const& grid() const noexcept { return *activeGrid_; }

    /// @returns the primary screen's grid if primary screen is active.
    Grid<Cell>& grid() noexcept { return *activeGrid_; }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    bool isLineWrapped(LineOffset _lineNumber) const noexcept { return activeGrid_->isLineWrapped(_lineNumber); }

    ColorPalette& colorPalette() noexcept { return colorPalette_; }
    ColorPalette const& colorPalette() const noexcept { return colorPalette_; }

    ColorPalette& defaultColorPalette() noexcept { return defaultColorPalette_; }
    ColorPalette const& defaultColorPalette() const noexcept { return defaultColorPalette_; }

    std::shared_ptr<HyperlinkInfo> hyperlinkAt(Coordinate pos) noexcept
    {
        return hyperlinks_.hyperlinkById(at(pos).hyperlink());
    }

    HyperlinkStorage const& hyperlinks() const noexcept
    {
        return hyperlinks_;
    }

  private:
    void setBuffer(ScreenType _type);
    void applyPageSizeToCurrentBuffer();

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p _column.
    void linefeed(ColumnOffset _column);

    void writeCharToCurrentAndAdvance(char32_t _codepoint) noexcept;
    void clearAndAdvance(int _offset) noexcept;

    void fail(std::string const& _message) const;

    void scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin);
    void scrollUp(LineCount n, Margin margin);
    void scrollDown(LineCount n, Margin margin);
    void insertChars(LineOffset _lineNo, ColumnCount _n);
    void deleteChars(LineOffset _lineNo, ColumnOffset _column, ColumnCount _count);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnOffset _n);

    // private fields
    //
    EventListener& eventListener_;

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
    ImageFragmentCache imageFragments_;
    ImageFragmentId nextImageFragmentId_ = ImageFragmentId(1);

    Sequencer<EventListener> sequencer_;
    parser::Parser<Sequencer<EventListener>> parser_;
    int64_t instructionCounter_ = 0;

    PageSize pageSize_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    bool sixelCursorConformance_ = true;

    // XXX moved from ScreenBuffer
    Margin margin_;
    ColumnCount tabWidth_{8};
    std::vector<ColumnOffset> tabs_;

    // main/alt screen and history
    //
    //std::array<Lines, 2> lines_;
    ScreenType screenType_ = ScreenType::Main;
    // Lines* activeBuffer_;
    // Lines savedLines_{};

    bool allowReflowOnResize_;
    std::array<Grid<Cell>, 2> grids_;
    Grid<Cell>* activeGrid_;

    // cursor related
    //
    Cursor cursor_;
    Cursor savedCursor_;
    Cursor savedPrimaryCursor_; //!< saved cursor of primary-screen when switching to alt-screen.
    Coordinate lastCursorPosition_;
    bool wrapPending_ = false;

    CursorDisplay cursorDisplay_ = CursorDisplay::Steady;
    CursorShape cursorShape_ = CursorShape::Block;

    std::string currentWorkingDirectory_ = {};

    // Hyperlink related
    //

    HyperlinkStorage hyperlinks_{};

    // experimental features
    //
    bool respondToTCapQuery_ = true;
};

class MockTerm: public MockScreenEvents
{
public:
    explicit MockTerm(PageSize _size, LineCount _hist = {}):
        screen(_size, *this, false, false, _hist)
    {
    }

    Screen<MockTerm> screen;
};

}  // namespace terminal

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Margin::Horizontal> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Margin::Horizontal range, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}..{}", range.from, range.to);
        }
    };

    template <>
    struct formatter<terminal::Margin::Vertical> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Margin::Vertical range, FormatContext& ctx)
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
            return format_to(ctx.out(), "({}:{}{})", cursor.position.line, cursor.position.column, cursor.visible ? "" : ", (invis)");
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
} // }}}
