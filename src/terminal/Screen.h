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
#include <terminal/CommandBuilder.h>
#include <terminal/Commands.h>
#include <terminal/Hyperlink.h>
#include <terminal/InputGenerator.h> // MouseTransport
#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/ScreenBuffer.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Selector.h>
#include <terminal/VTType.h>
#include <terminal/WindowSize.h>

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
class Debugger;

/// VT Sequence Executor for directly executing the VT sequences as they arive.
class CommandExecutor : public CommandVisitor {
  protected:
    Screen& screen_;

  public:
    explicit CommandExecutor(Screen& _screen) :
        screen_{ _screen }
    {}

    void visit(AppendChar const& v) override;
    void visit(ApplicationKeypadMode const& v) override;
    void visit(BackIndex const& v) override;
    void visit(Backspace const& v) override;
    void visit(Bell const& v) override;
    void visit(ChangeIconTitle const& v) override;
    void visit(ChangeWindowTitle const& v) override;
    void visit(ClearLine const& v) override;
    void visit(ClearScreen const& v) override;
    void visit(ClearScrollbackBuffer const& v) override;
    void visit(ClearToBeginOfLine const& v) override;
    void visit(ClearToBeginOfScreen const& v) override;
    void visit(ClearToEndOfLine const& v) override;
    void visit(ClearToEndOfScreen const& v) override;
    void visit(CopyToClipboard const& v) override;
    void visit(CursorBackwardTab const& v) override;
    void visit(CursorNextLine const& v) override;
    void visit(CursorPreviousLine const& v) override;
    void visit(DeleteCharacters const& v) override;
    void visit(DeleteColumns const& v) override;
    void visit(DeleteLines const& v) override;
    void visit(DesignateCharset const& v) override;
    void visit(DeviceStatusReport const& v) override;
    void visit(DumpState const& v) override;
    void visit(EraseCharacters const& v) override;
    void visit(ForwardIndex const& v) override;
    void visit(FullReset const& v) override;
    void visit(HorizontalPositionAbsolute const& v) override;
    void visit(HorizontalPositionRelative const& v) override;
    void visit(HorizontalTabClear const& v) override;
    void visit(HorizontalTabSet const& v) override;
    void visit(Hyperlink const& v) override;
    void visit(Index const& v) override;
    void visit(InsertCharacters const& v) override;
    void visit(InsertColumns const& v) override;
    void visit(InsertLines const& v) override;
    void visit(Linefeed const& v) override;
    void visit(MoveCursorBackward const& v) override;
    void visit(MoveCursorDown const& v) override;
    void visit(MoveCursorForward const& v) override;
    void visit(MoveCursorTo const& v) override;
    void visit(MoveCursorToBeginOfLine const& v) override;
    void visit(MoveCursorToColumn const& v) override;
    void visit(MoveCursorToLine const& v) override;
    void visit(MoveCursorToNextTab const& v) override;
    void visit(MoveCursorUp const& v) override;
    void visit(Notify const& v) override;
    void visit(ReportCursorPosition const& v) override;
    void visit(ReportExtendedCursorPosition const& v) override;
    void visit(RequestDynamicColor const& v) override;
    void visit(RequestMode const& v) override;
    void visit(RequestTabStops const& v) override;
    void visit(ResetDynamicColor const& v) override;
    void visit(ResizeWindow const& v) override;
    void visit(RestoreCursor const& v) override;
    void visit(RestoreWindowTitle const& v) override;
    void visit(ReverseIndex const& v) override;
    void visit(SaveCursor const& v) override;
    void visit(SaveWindowTitle const& v) override;
    void visit(ScreenAlignmentPattern const& v) override;
    void visit(ScrollDown const& v) override;
    void visit(ScrollUp const& v) override;
    void visit(SelectConformanceLevel const& v) override;
    void visit(SendDeviceAttributes const& v) override;
    void visit(SendMouseEvents const& v) override;
    void visit(SendTerminalId const& v) override;
    void visit(SetBackgroundColor const& v) override;
    void visit(SetCursorStyle const& v) override;
    void visit(SetDynamicColor const& v) override;
    void visit(SetForegroundColor const& v) override;
    void visit(SetGraphicsRendition const& v) override;
    void visit(SetLeftRightMargin const& v) override;
    void visit(SetMark const& v) override;
    void visit(SetMode const& v) override;
    void visit(SetTopBottomMargin const& v) override;
    void visit(SetUnderlineColor const& v) override;
    void visit(SingleShiftSelect const& v) override;
    void visit(SoftTerminalReset const& v) override;
};

/// Batches any drawing related command until synchronization point, or
/// executes the command directly otherwise.
class SynchronizedCommandExecutor : public CommandExecutor {
  private:
    CommandList queuedCommands_;

  public:
    explicit SynchronizedCommandExecutor(Screen& _screen)
        : CommandExecutor{_screen}
    {}

    // applies all queued commands.
    void flush();

    void enqueue(Command&& _cmd)
    {
        queuedCommands_.emplace_back(std::move(_cmd));
    }

    void visit(AppendChar const& v) override { enqueue(v); }
    void visit(BackIndex const& v) override { enqueue(v); }
    void visit(Backspace const& v) override { enqueue(v); }
    void visit(ClearLine const& v) override { enqueue(v); }
    void visit(ClearScreen const& v) override { enqueue(v); }
    void visit(ClearScrollbackBuffer const& v) override { enqueue(v); }
    void visit(ClearToBeginOfLine const& v) override { enqueue(v); }
    void visit(ClearToBeginOfScreen const& v) override { enqueue(v); }
    void visit(ClearToEndOfLine const& v) override { enqueue(v); }
    void visit(ClearToEndOfScreen const& v) override { enqueue(v); }
    void visit(CursorBackwardTab const& v) override { enqueue(v); }
    void visit(CursorNextLine const& v) override { enqueue(v); }
    void visit(CursorPreviousLine const& v) override { enqueue(v); }
    void visit(DeleteCharacters const& v) override { enqueue(v); }
    void visit(DeleteColumns const& v) override { enqueue(v); }
    void visit(DeleteLines const& v) override { enqueue(v); }
    void visit(DesignateCharset const& v) override { enqueue(v); }
    void visit(EraseCharacters const& v) override { enqueue(v); }
    void visit(ForwardIndex const& v) override { enqueue(v); }
    void visit(FullReset const& v) override { enqueue(v); }
    void visit(HorizontalPositionAbsolute const& v) override { enqueue(v); }
    void visit(HorizontalPositionRelative const& v) override { enqueue(v); }
    void visit(HorizontalTabClear const& v) override { enqueue(v); }
    void visit(HorizontalTabSet const& v) override { enqueue(v); }
    void visit(Hyperlink const& v) override { enqueue(v); }
    void visit(Index const& v) override { enqueue(v); }
    void visit(InsertCharacters const& v) override { enqueue(v); }
    void visit(InsertColumns const& v) override { enqueue(v); }
    void visit(InsertLines const& v) override { enqueue(v); }
    void visit(Linefeed const& v) override { enqueue(v); }
    void visit(MoveCursorBackward const& v) override { enqueue(v); }
    void visit(MoveCursorDown const& v) override { enqueue(v); }
    void visit(MoveCursorForward const& v) override { enqueue(v); }
    void visit(MoveCursorTo const& v) override { enqueue(v); }
    void visit(MoveCursorToBeginOfLine const& v) override { enqueue(v); }
    void visit(MoveCursorToColumn const& v) override { enqueue(v); }
    void visit(MoveCursorToLine const& v) override { enqueue(v); }
    void visit(MoveCursorToNextTab const& v) override { enqueue(v); }
    void visit(MoveCursorUp const& v) override { enqueue(v); }
    void visit(ResetDynamicColor const& v) override { enqueue(v); }
    void visit(ResizeWindow const& v) override { enqueue(v); }
    void visit(RestoreCursor const& v) override { enqueue(v); }
    void visit(ReverseIndex const& v) override { enqueue(v); }
    void visit(SaveCursor const& v) override { enqueue(v); }
    void visit(ScreenAlignmentPattern const& v) override { enqueue(v); }
    void visit(ScrollDown const& v) override { enqueue(v); }
    void visit(ScrollUp const& v) override { enqueue(v); }
    void visit(SetBackgroundColor const& v) override { enqueue(v); }
    void visit(SetCursorStyle const& v) override { enqueue(v); }
    void visit(SetDynamicColor const& v) override { enqueue(v); }
    void visit(SetForegroundColor const& v) override { enqueue(v); }
    void visit(SetGraphicsRendition const& v) override { enqueue(v); }
    void visit(SetLeftRightMargin const& v) override { enqueue(v); }
    void visit(SetMark const& v) override { enqueue(v); }
    void visit(SetTopBottomMargin const& v) override { enqueue(v); }
    void visit(SetUnderlineColor const& v) override { enqueue(v); }
    void visit(SingleShiftSelect const& v) override { enqueue(v); }
};

/**
 * Terminal Screen.
 *
 * Implements the all Command types and applies all instruction
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
    Screen(WindowSize const& _size,
           ScreenEvents& _eventListener,
           Logger const& _logger = Logger{},
           bool _logRaw = false,
           bool _logTrace = false,
           std::optional<size_t> _maxHistoryLineCount = std::nullopt);

    void setLogTrace(bool _enabled) { logTrace_ = _enabled; }
    bool logTrace() const noexcept { return logTrace_; }
    void setLogRaw(bool _enabled) { logRaw_ = _enabled; }
    bool logRaw() const noexcept { return logRaw_; }

    void setTerminalId(VTType _id) noexcept
    {
        terminalId_ = _id;
    }

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);
    int historyLineCount() const noexcept { return buffer_->historyLineCount(); }

    /// Writes given data into the screen.
    void write(char const* _data, size_t _size);

    void write(Command const& _command);

    /// Writes given data into the screen.
    void write(std::string_view const& _text) { write(_text.data(), _text.size()); }

    void write(std::u32string_view const& _text);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    void render(RendererT _renderer, int _scrollOffset = 0) const;

    /// Renders a single text line.
    std::string renderTextLine(cursor_pos_t _row) const { return buffer_->renderTextLine(_row); }

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

    // {{{ Command processor
    void operator()(AppendChar const& v);
    void operator()(ApplicationKeypadMode const& v);
    void operator()(BackIndex const& v);
    void operator()(Backspace const& v);
    void operator()(Bell const& v);
    void operator()(ChangeIconTitle const& v);
    void operator()(ChangeWindowTitle const& v);
    void operator()(ClearLine const& v);
    void operator()(ClearScreen const& v);
    void operator()(ClearScrollbackBuffer const& v);
    void operator()(ClearToBeginOfLine const& v);
    void operator()(ClearToBeginOfScreen const& v);
    void operator()(ClearToEndOfLine const& v);
    void operator()(ClearToEndOfScreen const& v);
    void operator()(CopyToClipboard const& v);
    void operator()(CursorBackwardTab const& v);
    void operator()(CursorNextLine const& v);
    void operator()(CursorPreviousLine const& v);
    void operator()(DeleteCharacters const& v);
    void operator()(DeleteColumns const& v);
    void operator()(DeleteLines const& v);
    void operator()(DesignateCharset const& v);
    void operator()(DeviceStatusReport const& v);
    void operator()(DumpState const&);
    void operator()(EraseCharacters const& v);
    void operator()(ForwardIndex const& v);
    void operator()(FullReset const& v);
    void operator()(HorizontalPositionAbsolute const& v);
    void operator()(HorizontalPositionRelative const& v);
    void operator()(HorizontalTabClear const& v);
    void operator()(HorizontalTabSet const& v);
    void operator()(Hyperlink const& v);
    void operator()(Index const& v);
    void operator()(InsertCharacters const& v);
    void operator()(InsertColumns const& v);
    void operator()(InsertLines const& v);
    void operator()(Linefeed const& v);
    void operator()(MoveCursorBackward const& v);
    void operator()(MoveCursorDown const& v);
    void operator()(MoveCursorForward const& v);
    void operator()(MoveCursorTo const& v);
    void operator()(MoveCursorToBeginOfLine const& v);
    void operator()(MoveCursorToColumn const& v);
    void operator()(MoveCursorToLine const& v);
    void operator()(MoveCursorToNextTab const& v);
    void operator()(MoveCursorUp const& v);
    void operator()(Notify const& v);
    void operator()(ReportCursorPosition const& v);
    void operator()(ReportExtendedCursorPosition const& v);
    void operator()(RequestDynamicColor const& v);
    void operator()(RequestMode const& v);
    void operator()(RequestTabStops const& v);
    void operator()(ResetDynamicColor const& v);
    void operator()(ResizeWindow const& v);
    void operator()(RestoreCursor const& v);
    void operator()(RestoreWindowTitle const& v);
    void operator()(ReverseIndex const& v);
    void operator()(SaveCursor const& v);
    void operator()(SaveWindowTitle const& v);
    void operator()(ScreenAlignmentPattern const& v);
    void operator()(ScrollDown const& v);
    void operator()(ScrollUp const& v);
    void operator()(SelectConformanceLevel const& v);
    void operator()(SendDeviceAttributes const& v);
    void operator()(SendMouseEvents const& v);
    void operator()(SendTerminalId const& v);
    void operator()(SetBackgroundColor const& v);
    void operator()(SetCursorStyle const& v);
    void operator()(SetDynamicColor const& v);
    void operator()(SetForegroundColor const& v);
    void operator()(SetGraphicsRendition const& v);
    void operator()(SetLeftRightMargin const& v);
    void operator()(SetMark const&);
    void operator()(SetMode const& v);
    void operator()(SetTopBottomMargin const& v);
    void operator()(SetUnderlineColor const& v);
    void operator()(SingleShiftSelect const& v);
    void operator()(SoftTerminalReset const& v);
    // }}}

    // reset screen
    void resetSoft();
    void resetHard();

    // for DECSC and DECRC
    void setMode(Mode _mode, bool _enabled);
    void saveCursor();
    void restoreCursor();

    WindowSize const& size() const noexcept { return size_; }
    void resize(WindowSize const& _newSize);

    /// {{{ viewport management API
    int scrollOffset() const noexcept { return scrollOffset_; }
    bool isLineVisible(cursor_pos_t _row) const noexcept;
    bool scrollUp(int _numLines);
    bool scrollDown(int _numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool scrollMarkUp();
    bool scrollMarkDown();
    //}}}

    bool isCursorInsideMargins() const noexcept { return buffer_->isCursorInsideMargins(); }

    Coordinate realCursorPosition() const noexcept { return buffer_->realCursorPosition(); }
    Coordinate cursorPosition() const noexcept { return buffer_->cursorPosition(); }
    Cursor const& cursor() const noexcept { return buffer_->cursor; }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(Coordinate const& _coord) const noexcept
    {
        return 1 <= _coord.row && _coord.row <= size_.rows
            && 1 <= _coord.column && _coord.column <= size_.columns;
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
    std::string renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const;

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

    /// @returns true when currently in debugging mode and false otherwise.
    bool debugging() const noexcept { return debugExecutor_.get(); }
    void setDebugging(bool _enabled);

    /// @returns pointer to DebugExecutor if debugging, nullptr otherwise.
    Debugger* debugger() noexcept;

    bool synchronizeOutput() const noexcept { return false; } // TODO

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

    std::stack<Cursor> savedCursors_{};

    Logger const logger_;
    bool logRaw_ = false;
    bool logTrace_ = false;
    bool focused_ = true;

    CommandBuilder commandBuilder_;
    parser::Parser parser_;
    int64_t instructionCounter_ = 0;

    VTType terminalId_ = VTType::VT525;

    Modes modes_;

    ScreenBuffer primaryBuffer_;
    ScreenBuffer alternateBuffer_;
    ScreenBuffer* buffer_;

    WindowSize size_;
    std::optional<size_t> maxHistoryLineCount_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    CommandExecutor directExecutor_;
    SynchronizedCommandExecutor synchronizedExecutor_;
    std::unique_ptr<CommandVisitor> debugExecutor_;
    CommandVisitor* commandExecutor_ = nullptr;

    int scrollOffset_ = 0;

    std::unique_ptr<Selector> selector_;
};

// {{{ template functions
template <typename RendererT>
void Screen::render(RendererT _render, int _scrollOffset) const
{
    if (!_scrollOffset)
    {
        crispy::for_each(
            crispy::times(1, size_.rows) * crispy::times(1, size_.columns),
            [&](auto const& _pos) {
                auto const [row, col] = _pos;
                auto const pos = Coordinate{row, col};
                _render({row, col}, at(pos));
            }
        );
    }
    else
    {
        _scrollOffset = std::min(_scrollOffset, buffer_->historyLineCount());
        auto const historyLineCount = std::min(size_.rows, static_cast<int>(_scrollOffset));
        auto const mainLineCount = size_.rows - historyLineCount;

        cursor_pos_t rowNumber = 1;

        // render first part from history
        for (auto line = prev(end(buffer_->savedLines), _scrollOffset); rowNumber <= historyLineCount; ++line, ++rowNumber)
        {
            if (static_cast<int>(line->size()) < size_.columns)
                line->resize(size_.columns);

            auto column = begin(*line);
            for (cursor_pos_t colNumber = 1; colNumber <= size_.columns; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }

        // render second part from main screen buffer
        for (auto line = begin(buffer_->lines); line != next(begin(buffer_->lines), mainLineCount); ++line, ++rowNumber)
        {
            auto column = begin(*line);
            for (cursor_pos_t colNumber = 1; colNumber <= size_.columns; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }
    }
}
// }}}

}  // namespace terminal
