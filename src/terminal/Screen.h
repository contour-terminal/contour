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

#include <terminal/Color.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/InputGenerator.h> // MouseTransport
#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/ScreenBuffer.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Sequencer.h>
#include <terminal/Selector.h>
#include <terminal/VTType.h>
#include <terminal/Size.h>

#include <crispy/algorithm.h>
#include <crispy/times.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/width.h>
#include <unicode/utf8.h>

#include <fmt/format.h>

#include <algorithm>
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

class Screen;

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
class Screen {
  public:
    using Renderer = ScreenBuffer::Renderer;

    /**
     * Initializes the screen with the given screen size and callbaks.
     *
     * @param _size screen dimensions in number of characters per line and number of lines.
     * @param _eventListener Interface to some VT sequence related callbacks.
     * @param _logger an optional logger for logging various events.
     * @param _logRaw whether or not to log raw VT sequences.
     * @param _logTrace whether or not to log VT sequences in trace mode.
     * @param _maxHistoryLineCount number of lines the history must not exceed.
     */
    Screen(Size const& _size,
           ScreenEvents& _eventListener,
           Logger const& _logger = Logger{},
           bool _logRaw = false,
           bool _logTrace = false,
           std::optional<size_t> _maxHistoryLineCount = std::nullopt,
           Size _maxImageSize = Size{800, 600},
           int _maxImageColorRegisters = 256,
           bool _sixelCursorConformance = true
    );

    void setLogTrace(bool _enabled) { logTrace_ = _enabled; }
    bool logTrace() const noexcept { return logTrace_; }
    void setLogRaw(bool _enabled) { logRaw_ = _enabled; }
    bool logRaw() const noexcept { return logRaw_; }

    void setMaxImageColorRegisters(int _value) noexcept { sequencer_.setMaxImageColorRegisters(_value); }
    void setSixelCursorConformance(bool _value) noexcept { sixelCursorConformance_ = _value; }

    constexpr Size cellPixelSize() const noexcept { return cellPixelSize_; }

    constexpr void setCellPixelSize(Size _cellPixelSize)
    {
        cellPixelSize_ = _cellPixelSize;
    }

    void setTerminalId(VTType _id) noexcept
    {
        terminalId_ = _id;
    }

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);
    int historyLineCount() const noexcept { return buffer_->historyLineCount(); }

    /// Writes given data into the screen.
    void write(char const* _data, size_t _size);

    /// Writes given data into the screen.
    void write(std::string_view const& _text) { write(_text.data(), _text.size()); }

    void write(std::u32string_view const& _text);

    void writeText(char32_t _char);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    void render(RendererT _renderer, std::optional<int> _scrollOffset = std::nullopt) const;

    /// Renders a single text line.
    std::string renderTextLine(int _row) const { return buffer_->renderTextLine(_row); }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    std::string renderText() const { return buffer_->renderText(); }

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot() const { return buffer_->screenshot(); }

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

    void eraseCharacters(int _n);  // ECH
    void insertCharacters(int _n); // ICH
    void deleteCharacters(int _n); // DCH
    void deleteColumns(int _n);    // DECDC
    void insertLines(int _n);      // IL
    void insertColumns(int _n);    // DECIC

    void deleteLines(int _n);      // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorBackward(int _n);      // CUB
    void moveCursorDown(int _n);          // CUD
    void moveCursorForward(int _n);       // CUF
    void moveCursorTo(int _n);            // CUP
    void moveCursorToBeginOfLine();       // CR
    void moveCursorToColumn(int _n);      // CHA
    void moveCursorToLine(int _n);        // VPA
    void moveCursorToNextLine(int _n);    // CNL
    void moveCursorToNextTab();           // HT
    void moveCursorToPrevLine(int _n);    // CPL
    void moveCursorUp(int _n);            // CUU

    void cursorBackwardTab(int _n);       // CBT
    void backspace();                     // BS
    void horizontalTabClear(HorizontalTabClear::Which _which); // TBC
    void horizontalTabSet();              // HTS

    void index(); // IND
    void reverseIndex(); // RI

    void setMark();
    void deviceStatusReport();            // DSR
    void reportCursorPosition();          // CPR
    void reportExtendedCursorPosition();  // DECXCPR
    void selectConformanceLevel(VTType _level);
    void requestDynamicColor(DynamicColorName _name);
    void sendDeviceAttributes();
    void sendTerminalId();

    void hyperlink(std::string const& _id, std::string const& _uri); // OSC 8
    void notify(std::string const& _title, std::string const& _content);

    void setForegroundColor(Color const& _color);
    void setBackgroundColor(Color const& _color);
    void setUnderlineColor(Color const& _color);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setGraphicsRendition(GraphicsRendition _rendition);
    void requestMode(Mode _mode);
    void setTopBottomMargin(std::optional<int> _top, std::optional<int> _bottom);
    void setLeftRightMargin(std::optional<int> _left, std::optional<int> _right);
    void screenAlignmentPattern();
    void sendMouseEvents(MouseProtocol _protocol, bool _enable);
    void applicationKeypadMode(bool _enable);
    void designateCharset(CharsetTable _table, CharsetId _charset);
    void singleShiftSelect(CharsetTable _table);
    void requestPixelSize(RequestPixelSize::Area _area);
    void sixelImage(Size _pixelSize, std::vector<uint8_t> const& _rgba);
    void requestStatusString(RequestStatusString::Value _value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName _name);
    void setDynamicColor(DynamicColorName _name, RGBColor const& _color);
    void dumpState();
    void smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value);
    // }}}

    // reset screen
    void resetSoft();
    void resetHard();

    // for DECSC and DECRC
    void setMode(Mode _mode, bool _enabled);
    void saveCursor();
    void restoreCursor();
    void saveModes(std::vector<Mode> const& _modes);
    void restoreModes(std::vector<Mode> const& _modes);

    Size const& size() const noexcept { return size_; }
    void resize(Size const& _newSize);

    bool isCursorInsideMargins() const noexcept { return buffer_->isCursorInsideMargins(); }

    Coordinate realCursorPosition() const noexcept { return buffer_->realCursorPosition(); }
    Coordinate cursorPosition() const noexcept { return buffer_->cursorPosition(); }
    Cursor const& cursor() const noexcept { return buffer_->cursor; }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(Coordinate const& _coord) const noexcept
    {
        return 1 <= _coord.row && _coord.row <= size_.height
            && 1 <= _coord.column && _coord.column <= size_.width;
    }

    Cell const& currentCell() const noexcept
    {
        return *buffer_->currentColumn;
    }

    Cell& currentCell() noexcept
    {
        return *buffer_->currentColumn;
    }

    Cell& currentCell(Cell value)
    {
        *buffer_->currentColumn = std::move(value);
        return *buffer_->currentColumn;
    }

    void moveCursorTo(Coordinate to);

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(Coordinate const& _coord) noexcept
    {
        return buffer_->at(_coord);
    }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(Coordinate const& _coord) const noexcept
    {
        return const_cast<Screen&>(*this).at(_coord);
    }

    bool isPrimaryScreen() const noexcept { return buffer_ == &primaryBuffer_; }
    bool isAlternateScreen() const noexcept { return buffer_ == &alternateBuffer_; }

    ScreenBuffer const& currentBuffer() const noexcept { return *buffer_; }
    ScreenBuffer& currentBuffer() noexcept { return *buffer_; }

    bool isModeEnabled(Mode m) const noexcept
    {
        return modes_.enabled(m);
    }

    bool verticalMarginsEnabled() const noexcept { return isModeEnabled(Mode::Origin); }
    bool horizontalMarginsEnabled() const noexcept { return isModeEnabled(Mode::LeftRightMargin); }

    Margin const& margin() const noexcept { return buffer_->margin_; }
    ScreenBuffer::Lines const& scrollbackLines() const noexcept { return buffer_->savedLines; }

    void setTabWidth(int _value)
    {
        // TODO: Find out if we need to have that attribute per buffer or if having it across buffers is sufficient.
        primaryBuffer_.tabWidth = _value;
        alternateBuffer_.tabWidth = _value;
    }

    /**
     * Returns the n'th saved line into the history scrollback buffer.
     *
     * @param _lineNumberIntoHistory the 1-based offset into the history buffer.
     *
     * @returns the textual representation of the n'th line into the history.
     */
    std::string renderHistoryTextLine(int _lineNumberIntoHistory) const;

    std::string const& windowTitle() const noexcept { return windowTitle_; }

    std::optional<int> findMarkerForward(int _currentCursorLine) const // TODO: remove me?
    {
        return buffer_->findMarkerForward(_currentCursorLine);
    }

    std::optional<int> findMarkerBackward(int _currentCursorLine) const // TODO: remove me?
    {
        return buffer_->findMarkerBackward(_currentCursorLine);
    }

    ScreenBuffer::Type bufferType() const noexcept { return buffer_->type_; }

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept { return selector_ && selector_->state() != Selector::State::Waiting; }

    /// Returns list of ranges that have been selected.
    std::vector<Selector::Range> selection() const;

    /// Tests whether given absolute coordinate is covered by a current selection.
    bool isSelectedAbsolute(Coordinate _coord) const noexcept
    {
        return selector_
            && selector_->state() != Selector::State::Waiting
            && selector_->contains(_coord);
    }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selector> _selector) { selector_ = std::move(_selector); }

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!selector_; }

    Selector const* selector() const noexcept { return selector_.get(); }
    Selector* selector() noexcept { return selector_.get(); }

    /// Clears current selection, if any currently present.
    void clearSelection() { selector_.reset(); }

    /// Renders only the selected area.
    void renderSelection(Renderer const& _render) const;

    bool synchronizeOutput() const noexcept { return false; } // TODO

    ScreenEvents& eventListener() noexcept { return eventListener_; }
    ScreenEvents const& eventListener()  const noexcept { return eventListener_; }

    void setWindowTitle(std::string const& _title);
    void saveWindowTitle();
    void restoreWindowTitle();

    void setMaxImageSize(Size _size) noexcept { sequencer_.setMaxImageSize(_size); }

  private:
    void setBuffer(ScreenBuffer::Type _type);

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

  private:
    ScreenEvents& eventListener_;

    Logger const logger_;
    bool logRaw_ = false;
    bool logTrace_ = false;
    bool focused_ = true;

    Size cellPixelSize_; ///< contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.

    VTType terminalId_ = VTType::VT525;

    Modes modes_;
    std::map<Mode, std::vector<bool>> savedModes_; //!< saved DEC modes

    int maxImageColorRegisters_;
    std::shared_ptr<ColorPalette> imageColorPalette_;
    ImagePool imagePool_;

    Sequencer sequencer_;
    parser::Parser parser_;
    int64_t instructionCounter_ = 0;

    ScreenBuffer primaryBuffer_;
    ScreenBuffer alternateBuffer_;
    ScreenBuffer* buffer_;

    Size size_;
    std::optional<size_t> maxHistoryLineCount_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    bool sixelCursorConformance_ = true;

    std::unique_ptr<Selector> selector_;
};

class Viewport {
  private:
    Screen& screen_;
    std::optional<int> scrollOffset_; //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history

    int historyLineCount() const noexcept { return screen_.historyLineCount(); }
    int screenLineCount() const noexcept { return screen_.size().height; }

  public:
    explicit Viewport(Screen& _screen) : screen_{ _screen } {}

    /// Returns the absolute offset where 0 is the top of scrollback buffer, and the maximum value the bottom of the screeen (plus history).
    std::optional<int> absoluteScrollOffset() const noexcept
    {
        return scrollOffset_;
    }

    /// @returns scroll offset relative to the main screen buffer
    int relativeScrollOffset() const noexcept
    {
        return scrollOffset_.has_value()
            ? historyLineCount() - scrollOffset_.value()
            : 0;
    }

    bool isLineVisible(int _row) const noexcept
    {
        return crispy::ascending(1 - relativeScrollOffset(), _row, screenLineCount() - relativeScrollOffset());
    }

    bool scrollUp(int _numLines)
    {
        if (screen_.isAlternateScreen()) // TODO: make configurable
            return false;

        if (_numLines <= 0)
            return false;

        if (auto const newOffset = std::max(absoluteScrollOffset().value_or(historyLineCount()) - _numLines, 0); newOffset != absoluteScrollOffset())
        {
            scrollOffset_.emplace(newOffset);
            return true;
        }
        else
            return false;
    }

    bool scrollDown(int _numLines)
    {
        if (screen_.isAlternateScreen()) // TODO: make configurable
            return false;

        if (_numLines <= 0)
            return false;

        auto const newOffset = absoluteScrollOffset().value_or(historyLineCount()) + _numLines;
        if (newOffset < historyLineCount())
        {
            scrollOffset_.emplace(newOffset);
            return true;
        }
        else if (newOffset == historyLineCount() || scrollOffset_.has_value())
        {
            scrollOffset_.reset();
            return true;
        }
        else
            return false;
    }

    bool scrollToTop()
    {
        if (absoluteScrollOffset() != 0)
        {
            scrollOffset_.emplace(0);
            return true;
        }
        else
            return false;
    }

    bool scrollToBottom()
    {
        if (scrollOffset_.has_value())
        {
            scrollOffset_.reset();
            return true;
        }
        else
            return false;
    }

    void scrollToAbsolute(int _absoluteScrollOffset)
    {
        if (_absoluteScrollOffset >= 0 && _absoluteScrollOffset < historyLineCount())
            scrollOffset_.emplace(_absoluteScrollOffset);
        else if (_absoluteScrollOffset >= historyLineCount())
            scrollOffset_.reset();
    }

    bool scrollMarkUp()
    {
        auto const newScrollOffset = screen_.currentBuffer().findMarkerBackward(absoluteScrollOffset().value_or(historyLineCount()));
        if (newScrollOffset.has_value())
        {
            scrollOffset_.emplace(newScrollOffset.value());
            return true;
        }

        return false;
    }

    bool scrollMarkDown()
    {
        auto const newScrollOffset = screen_.currentBuffer().findMarkerForward(absoluteScrollOffset().value_or(historyLineCount()));

        if (!newScrollOffset.has_value())
            return false;

        if (*newScrollOffset < historyLineCount())
            scrollOffset_.emplace(*newScrollOffset);
        else
            scrollOffset_.reset();

        return true;
    }
};

// {{{ template functions
template <typename RendererT>
void Screen::render(RendererT _render, std::optional<int> _scrollOffset) const
{
    if (!_scrollOffset.has_value())
    {
        crispy::for_each(
            crispy::times(1, size_.height) * crispy::times(1, size_.width),
            [&](auto const& _pos) {
                auto const [row, col] = _pos;
                auto const pos = Coordinate{row, col};
                _render({row, col}, at(pos));
            }
        );
    }
    else
    {
        _scrollOffset = std::clamp(*_scrollOffset, 0, buffer_->historyLineCount());

        int rowNumber = 1;

        // render first part from history
        for (auto line = next(begin(buffer_->savedLines), *_scrollOffset);
                line != end(buffer_->savedLines) && rowNumber <= size_.height;
                ++line, ++rowNumber)
        {
            if (static_cast<int>(line->size()) < size_.width)
                line->resize(size_.width);

            auto column = begin(*line);
            for (int colNumber = 1; colNumber <= size_.width; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }

        // render second part from main screen buffer
        for (auto line = begin(buffer_->lines); rowNumber <= size_.height; ++line, ++rowNumber)
        {
            auto column = begin(*line);
            for (int colNumber = 1; colNumber <= size_.width; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }
    }
}
// }}}

}  // namespace terminal
