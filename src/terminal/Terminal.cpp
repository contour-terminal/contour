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
#include <terminal/ControlCode.h>
#include <terminal/InputGenerator.h>
#include <terminal/RenderBuffer.h>
#include <terminal/RenderBufferBuilder.h>
#include <terminal/Terminal.h>
#include <terminal/logging.h>
#include <terminal/pty/MockPty.h>

#include <crispy/escape.h>
#include <crispy/stdfs.h>

#include <fmt/chrono.h>

#include <chrono>
#include <iostream>
#include <signal.h>
#include <utility>

#include <sys/types.h>

using crispy::Size;

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using std::move;

namespace terminal
{

namespace // {{{ helpers
{
    void trimSpaceRight(string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    }

#if defined(CONTOUR_PERF_STATS)
    void logRenderBufferSwap(bool _success, uint64_t _frameID)
    {
        if (!RenderBufferLog)
            return;

        if (_success)
            RenderBufferLog()("Render buffer {} swapped.", _frameID);
        else
            RenderBufferLog()("Render buffer {} swapping failed.", _frameID);
    }
#endif
} // namespace
// }}}

Terminal::Terminal(unique_ptr<Pty> _pty,
                   size_t _ptyReadBufferSize,
                   Terminal::Events& _eventListener,
                   LineCount _maxHistoryLineCount,
                   LineOffset _copyLastMarkRangeOffset,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   string const& _wordDelimiters,
                   Modifier _mouseProtocolBypassModifier,
                   ImageSize _maxImageSize,
                   unsigned _maxImageColorRegisters,
                   bool _sixelCursorConformance,
                   ColorPalette _colorPalette,
                   double _refreshRate,
                   bool _allowReflowOnResize):
    changes_ { 0 },
    ptyReadBufferSize_ { _ptyReadBufferSize },
    eventListener_ { _eventListener },
    refreshInterval_ { static_cast<long long>(1000.0 / _refreshRate) },
    renderBuffer_ {},
    pty_ { move(_pty) },
    startTime_ { _now },
    currentTime_ { _now },
    lastCursorBlink_ { _now },
    cursorDisplay_ { CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_ { CursorShape::Block },      // TODO: pass via param
    cursorBlinkInterval_ { _cursorBlinkInterval },
    cursorBlinkState_ { 1 },
    wordDelimiters_ { unicode::from_utf8(_wordDelimiters) },
    mouseProtocolBypassModifier_ { _mouseProtocolBypassModifier },
    copyLastMarkRangeOffset_ { _copyLastMarkRangeOffset },
    // clang-format off
    state_ { *this,
             pty_->pageSize(),
             _maxHistoryLineCount,
             _maxImageSize,
             _maxImageColorRegisters,
             _sixelCursorConformance,
             move(_colorPalette),
             _allowReflowOnResize },
    // clang-format on
    primaryScreen_ { state_, ScreenType::Primary, state_.primaryBuffer },
    alternateScreen_ { state_, ScreenType::Alternate, state_.alternateBuffer },
    currentScreen_ { primaryScreen_ },
    viewport_ { *this,
                [this]() {
                    breakLoopAndRefreshRenderBuffer();
                } },
    selectionHelper_ { this }
{
#if 0
    hardReset();
#else
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::TextReflow, true);
    setMode(DECMode::SixelCursorNextToGraphic, state_.sixelCursorConformance);
#endif
}

Terminal::~Terminal()
{
}

void Terminal::setRefreshRate(double _refreshRate)
{
    refreshInterval_ = std::chrono::milliseconds(static_cast<long long>(1000.0 / _refreshRate));
}

void Terminal::setLastMarkRangeOffset(LineOffset _value) noexcept
{
    copyLastMarkRangeOffset_ = _value;
}

bool Terminal::processInputOnce()
{
    auto const timeout = renderBuffer_.state == RenderBufferState::WaitingForRefresh && !screenDirty_
                             ? std::chrono::seconds(4)
                             //: refreshInterval_ : std::chrono::seconds(0)
                             : std::chrono::seconds(30);

    optional<string_view> const bufOpt = pty_->read(ptyReadBufferSize_, timeout);
    if (!bufOpt)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            TerminalLog()("PTY read failed (timeout: {}). {}", timeout, strerror(errno));
            pty_->close();
        }
        return errno == EINTR || errno == EAGAIN;
    }
    string_view const buf = *bufOpt;

    if (buf.empty())
    {
        TerminalLog()("PTY read returned with zero bytes. Closing PTY.");
        pty_->close();
        return true;
    }

    writeToScreen(buf);

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
    ensureFreshRenderBuffer();
#endif

    return true;
}

// {{{ RenderBuffer synchronization
void Terminal::breakLoopAndRefreshRenderBuffer()
{
    changes_++;
    renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;

    // if (this_thread::get_id() == mainLoopThreadID_)
    //     return;

    pty_->wakeupReader();
}

bool Terminal::refreshRenderBuffer(bool _locked)
{
    renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
    ensureFreshRenderBuffer(_locked);
    return renderBuffer_.state == RenderBufferState::WaitingForRefresh;
}

bool Terminal::ensureFreshRenderBuffer(bool _locked)
{
    if (!renderBufferUpdateEnabled_)
    {
        // renderBuffer_.state = RenderBufferState::WaitingForRefresh;
        return false;
    }

    auto const elapsed = currentTime_ - renderBuffer_.lastUpdate;
    auto const avoidRefresh = elapsed < refreshInterval_;

    switch (renderBuffer_.state)
    {
        case RenderBufferState::WaitingForRefresh:
            if (avoidRefresh)
                break;
            renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
            [[fallthrough]];
        case RenderBufferState::RefreshBuffersAndTrySwap:
            if (!_locked)
                refreshRenderBuffer(renderBuffer_.backBuffer());
            else
                refreshRenderBufferInternal(renderBuffer_.backBuffer());
            renderBuffer_.state = RenderBufferState::TrySwapBuffers;
            [[fallthrough]];
        case RenderBufferState::TrySwapBuffers: {
            [[maybe_unused]] auto const success = renderBuffer_.swapBuffers(currentTime_);

#if defined(CONTOUR_PERF_STATS)
            logRenderBufferSwap(success, lastFrameID_);
#endif

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
            // Passively invoked by the terminal thread -> do inform render thread about updates.
            if (success)
                eventListener_.renderBufferUpdated();
#endif
        }
        break;
    }
    return true;
}

void Terminal::refreshRenderBuffer(RenderBuffer& _output)
{
    auto const _l = lock_guard { *this };
    refreshRenderBufferInternal(_output);
}

PageSize Terminal::SelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::SelectionHelper::wordDelimited(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    if (terminal->isPrimaryScreen())
    {
        auto const& cell = terminal->primaryScreen().at(_pos);
        return cell.empty()
               || terminal->wordDelimiters_.find(cell.codepoint(0)) != terminal->wordDelimiters_.npos;
    }
    else
    {
        auto const& cell = terminal->alternateScreen().at(_pos);
        return cell.empty()
               || terminal->wordDelimiters_.find(cell.codepoint(0)) != terminal->wordDelimiters_.npos;
    }
}

bool Terminal::SelectionHelper::wrappedLine(LineOffset _line) const noexcept
{
    return terminal->isLineWrapped(_line);
}

bool Terminal::SelectionHelper::cellEmpty(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().isCellEmpty(_pos);
}

int Terminal::SelectionHelper::cellWidth(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().cellWithAt(_pos);
}

/**
 * Sets the hyperlink into hovering state if mouse is currently hovering it
 * and unsets the state when the object is being destroyed.
 */
struct ScopedHyperlinkHover
{
    shared_ptr<HyperlinkInfo const> const href;

    ScopedHyperlinkHover(Terminal const& terminal, ScreenBase const& screen):
        href { terminal.tryGetHoveringHyperlink() }
    {
        if (href)
            href->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
    }

    ~ScopedHyperlinkHover()
    {
        if (href)
            href->state = HyperlinkState::Inactive;
    }
};

void Terminal::refreshRenderBufferInternal(RenderBuffer& _output)
{
    verifyState();

    auto const renderHyperlinks = currentScreen_.get().contains(currentMousePosition_);

    changes_.store(0);
    screenDirty_ = false;
    ++lastFrameID_;

#if defined(CONTOUR_PERF_STATS)
    if (TerminalLog)
        TerminalLog()("{}: Refreshing render buffer.\n", lastFrameID_.load());
#endif

    auto const hoveringHyperlinkGuard = ScopedHyperlinkHover { *this, currentScreen_ };

    if (isPrimaryScreen())
        primaryScreen_.render(RenderBufferBuilder<Cell> { *this, _output }, viewport_.scrollOffset());
    else
        alternateScreen_.render(RenderBufferBuilder<Cell> { *this, _output }, viewport_.scrollOffset());
}
// }}}

bool Terminal::sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    viewport_.scrollToBottom();
    bool const success = state_.inputGenerator.generate(_key, _modifier);
    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendCharPressEvent(char32_t _value, Modifier _modifier, Timestamp _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    auto const success = state_.inputGenerator.generate(_value, _modifier);

    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendMousePressEvent(Modifier _modifier,
                                   MouseButton _button,
                                   MousePixelPosition _pixelPosition,
                                   Timestamp /*_now*/)
{
    verifyState();

    respectMouseProtocol_ =
        mouseProtocolBypassModifier_ == Modifier::None || !_modifier.contains(mouseProtocolBypassModifier_);

    if (respectMouseProtocol_
        && state_.inputGenerator.generateMousePress(
            _modifier, _button, currentMousePosition_, _pixelPosition))
    {
        // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
        // under the current position
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::handleMouseSelection(Modifier _modifier, Timestamp _now)
{
    verifyState();

    double const diff_ms = chrono::duration<double, milli>(_now - lastClick_).count();
    lastClick_ = _now;
    speedClicks_ = diff_ms >= 0.0 && diff_ms <= 500.0 ? speedClicks_ + 1 : 1;
    leftMouseButtonPressed_ = true;

    auto const startPos = CellLocation {
        currentMousePosition_.line - boxed_cast<LineOffset>(viewport_.scrollOffset()),
        currentMousePosition_.column,
    };

    switch (speedClicks_)
    {
        case 1:
            if (_modifier == mouseBlockSelectionModifier_)
                selection_ = make_unique<RectangularSelection>(selectionHelper_, startPos);
            else
                selection_ = make_unique<LinearSelection>(selectionHelper_, startPos);
            break;
        case 2:
            selection_ = make_unique<WordWiseSelection>(selectionHelper_, startPos);
            selection_->extend(startPos);
            break;
        case 3:
            selection_ = make_unique<FullLineSelection>(selectionHelper_, startPos);
            selection_->extend(startPos);
            break;
        default: clearSelection(); break;
    }

    breakLoopAndRefreshRenderBuffer();
    return true;
}

void Terminal::clearSelection()
{
    selection_.reset();
    speedClicks_ = 0;
    breakLoopAndRefreshRenderBuffer();
}

bool Terminal::sendMouseMoveEvent(Modifier _modifier,
                                  CellLocation newPosition,
                                  MousePixelPosition _pixelPosition,
                                  Timestamp /*_now*/)
{
    speedClicks_ = 0;

    if (leftMouseButtonPressed_ && isSelectionComplete())
        clearSelection();

    if (newPosition == currentMousePosition_ && !isModeEnabled(DECMode::MouseSGRPixels))
        return false;

    currentMousePosition_ = newPosition;

    auto const relativePos = viewport_.translateScreenToGridCoordinate(currentMousePosition_);

    bool changed = updateCursorHoveringState();

    // Do not handle mouse-move events in sub-cell dimensions.
    if (respectMouseProtocol_
        && state_.inputGenerator.generateMouseMove(_modifier, currentMousePosition_, _pixelPosition))
    {
        flushInput();
        return true;
    }

    if (leftMouseButtonPressed_ && !selectionAvailable())
    {
        changed = true;
        setSelector(make_unique<LinearSelection>(selectionHelper_, relativePos));
    }

    if (selectionAvailable() && selector()->state() != Selection::State::Complete)
    {
        changed = true;
        selector()->extend(relativePos);
        breakLoopAndRefreshRenderBuffer();
        return true;
    }

    // TODO: adjust selector's start lines according the the current viewport

    return changed;
}

bool Terminal::sendMouseReleaseEvent(Modifier _modifier,
                                     MouseButton _button,
                                     MousePixelPosition _pixelPosition,
                                     Timestamp /*_now*/)
{
    verifyState();

    if (respectMouseProtocol_
        && state_.inputGenerator.generateMouseRelease(
            _modifier, _button, currentMousePosition_, _pixelPosition))
    {
        flushInput();
        return true;
    }
    respectMouseProtocol_ = true;

    if (_button == MouseButton::Left)
    {
        leftMouseButtonPressed_ = false;
        if (selectionAvailable())
        {
            switch (selector()->state())
            {
                case Selection::State::InProgress:
                    selector()->complete();
                    eventListener_.onSelectionCompleted();
                    break;
                case Selection::State::Waiting: selection_.reset(); break;
                case Selection::State::Complete: break;
            }
        }
    }

    return true;
}

bool Terminal::sendFocusInEvent()
{
    state_.focused = true;
    breakLoopAndRefreshRenderBuffer();

    if (state_.inputGenerator.generateFocusInEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::sendFocusOutEvent()
{
    state_.focused = false;
    breakLoopAndRefreshRenderBuffer();

    if (state_.inputGenerator.generateFocusOutEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

void Terminal::sendPaste(string_view _text)
{
    state_.inputGenerator.generatePaste(_text);
    flushInput();
}

void Terminal::sendRaw(string_view _text)
{
    state_.inputGenerator.generateRaw(_text);
}

bool Terminal::hasInput() const noexcept
{
    return !state_.inputGenerator.peek().empty();
}

size_t Terminal::pendingInputBytes() const noexcept
{
    return !state_.inputGenerator.peek().size();
}

void Terminal::flushInput()
{
    if (state_.inputGenerator.peek().empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    auto const input = state_.inputGenerator.peek();
    auto const rv = pty_->write(input.data(), input.size());
    if (rv > 0)
        state_.inputGenerator.consume(rv);
}

void Terminal::writeToScreen(string_view _data)
{
    {
        auto const _l = std::lock_guard { *this };
        state_.parser.parseFragment(_data);
    }

    if (!state_.modes.enabled(DECMode::BatchedRendering))
    {
        screenUpdated();
    }
}

void Terminal::updateCursorVisibilityState() const
{
    if (cursorDisplay_ == CursorDisplay::Steady)
        return;

    auto const passed = chrono::duration_cast<chrono::milliseconds>(currentTime_ - lastCursorBlink_);
    if (passed < cursorBlinkInterval_)
        return;

    lastCursorBlink_ = currentTime_;
    cursorBlinkState_ = (cursorBlinkState_ + 1) % 2;
}

bool Terminal::updateCursorHoveringState()
{
    verifyState();

    auto const mouseInView = isPrimaryScreen() ? primaryScreen_.contains(currentMousePosition_)
                                               : alternateScreen_.contains(currentMousePosition_);
    if (!mouseInView)
        return false;

    auto const relCursorPos = viewport_.translateScreenToGridCoordinate(currentMousePosition_);
    auto const mouseInView2 = currentScreen_.get().contains(currentMousePosition_);
    auto const newState = mouseInView2 && !!currentScreen_.get().hyperlinkIdAt(relCursorPos);

    auto const oldState = hoveringHyperlink_.exchange(newState);
    return newState != oldState;
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    if (!state_.cursor.visible)
        return nullopt;

    if (cursorDisplay_ != CursorDisplay::Blink)
        return nullopt;

    auto const passed = chrono::duration_cast<chrono::milliseconds>(currentTime_ - lastCursorBlink_);
    if (passed <= cursorBlinkInterval_)
        return cursorBlinkInterval_ - passed;
    else
        return chrono::milliseconds::min();
}

void Terminal::resizeScreen(PageSize _cells, optional<ImageSize> _pixels)
{
    auto const _l = lock_guard { *this };

    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.

    auto const oldCursorPos = state_.cursor.position;

    state_.pageSize = _cells;

    // Reset margin to their default.
    state_.margin = Margin { Margin::Vertical { {}, _cells.lines.as<LineOffset>() - 1 },
                             Margin::Horizontal { {}, _cells.columns.as<ColumnOffset>() - 1 } };

    applyPageSizeToCurrentBuffer();

    if (_pixels)
    {
        auto width = Width(*_pixels->width / _cells.columns.as<unsigned>());
        auto height = Height(*_pixels->height / _cells.lines.as<unsigned>());
        setCellPixelSize(ImageSize { width, height });
    }

    currentMousePosition_.column =
        min(currentMousePosition_.column, boxed_cast<ColumnOffset>(_cells.columns - 1));
    currentMousePosition_.line = min(currentMousePosition_.line, boxed_cast<LineOffset>(_cells.lines - 1));

    pty_->resizeScreen(_cells, _pixels);

    verifyState();
}

void Terminal::resizeColumns(ColumnCount _newColumnCount, bool _clear)
{
    // DECCOLM / DECSCPP
    if (_clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin({}, unbox<LineOffset>(state_.pageSize.lines) - LineOffset(1));       // DECSTBM
        setLeftRightMargin({}, unbox<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1)); // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(DECMode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = PageSize { state_.pageSize.lines, _newColumnCount };
    auto const pixels = cellPixelSize() * newSize;
    resizeScreen(newSize, pixels);

    resizeWindow(newSize);
}

void Terminal::verifyState()
{
#if !defined(NDEBUG)
    Require(*currentMousePosition_.column < *pageSize().columns);
    Require(*currentMousePosition_.line < *pageSize().lines);

    if (isPrimaryScreen())
        Require(state_.primaryBuffer.pageSize() == state_.pageSize);
    else
        Require(state_.alternateBuffer.pageSize() == state_.pageSize);

    Require(*state_.cursor.position.column < *state_.pageSize.columns);
    Require(*state_.cursor.position.line < *state_.pageSize.lines);

    Require(state_.tabs.empty() || state_.tabs.back() < unbox<ColumnOffset>(state_.pageSize.columns));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(state_.cursor.position);
    if (state_.cursor.position != clampedCursorPos)
    {
        auto const errorMessage =
            fmt::format("Cursor {} does not match clamp to screen {}.", state_.cursor, clampedCursorPos);
        currentScreen_.get().fail(errorMessage);
        // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)
    }

    currentScreen_.get().verifyState();
#endif
}

void Terminal::setCursorDisplay(CursorDisplay _display)
{
    cursorDisplay_ = _display;
}

void Terminal::setCursorShape(CursorShape _shape)
{
    cursorShape_ = _shape;
}

void Terminal::setWordDelimiters(string const& _wordDelimiters)
{
    wordDelimiters_ = unicode::from_utf8(_wordDelimiters);
}

string Terminal::extractSelectionText() const
{
    using namespace terminal;
    ColumnOffset lastColumn = {};
    string text;
    string currentLine;

    renderSelection([&](CellLocation const& _pos, Cell const& _cell) {
        auto const _lock = scoped_lock { *this };
        auto const isNewLine = _pos.column <= lastColumn;
        bool const touchesRightPage =
            _pos.line.value > 0 && isSelected({ _pos.line - 1, pageSize().columns.as<ColumnOffset>() - 1 });
        if (isNewLine && (!isLineWrapped(_pos.line) || !touchesRightPage))
        {
            // TODO: handle logical line in word-selection (don't include LF in wrapped lines)
            trimSpaceRight(currentLine);
            text += currentLine;
            text += '\n';
            currentLine.clear();
        }
        currentLine += _cell.toUtf8();
        lastColumn = _pos.column;
    });

    trimSpaceRight(currentLine);
    text += currentLine;

    return text;
}

string Terminal::extractLastMarkRange() const
{
    auto const _l = std::lock_guard { *this };

    // -1 because we always want to start extracting one line above the cursor by default.
    auto const bottomLine = state_.cursor.position.line + LineOffset(-1) + copyLastMarkRangeOffset_;

    auto const marker1 = optional { bottomLine };

    auto const marker0 = primaryScreen_.findMarkerUpwards(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 + 1;
    auto const lastLine = *marker1;

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        auto const lineText = primaryScreen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += primaryScreen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += '\n';
    }

    return text;
}

// {{{ ScreenEvents overrides
void Terminal::requestCaptureBuffer(LineCount lines, bool logical)
{
    return eventListener_.requestCaptureBuffer(lines, logical);
}

void Terminal::bell()
{
    eventListener_.bell();
}

void Terminal::bufferChanged(ScreenType _type)
{
    selection_.reset();
    viewport_.forceScrollToBottom();
    eventListener_.bufferChanged(_type);
}

void Terminal::scrollbackBufferCleared()
{
    selection_.reset();
    viewport_.scrollToBottom();
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::screenUpdated()
{
    if (!renderBufferUpdateEnabled_)
        return;

    if (renderBuffer_.state == RenderBufferState::TrySwapBuffers)
    {
        renderBuffer_.swapBuffers(renderBuffer_.lastUpdate);
        return;
    }

    screenDirty_ = true;
    eventListener_.screenUpdated();
}

FontDef Terminal::getFontDef()
{
    return eventListener_.getFontDef();
}

void Terminal::setFontDef(FontDef const& _fontDef)
{
    eventListener_.setFontDef(_fontDef);
}

void Terminal::copyToClipboard(string_view _data)
{
    eventListener_.copyToClipboard(_data);
}

void Terminal::inspect()
{
    eventListener_.inspect();
}

void Terminal::notify(string_view _title, string_view _body)
{
    eventListener_.notify(_title, _body);
}

void Terminal::reply(string_view _reply)
{
    // this is invoked from within the terminal thread.
    // most likely that's not the main thread, which will however write
    // the actual input events.
    // TODO: introduce new mutex to guard terminal writes.
    sendRaw(_reply);
}

void Terminal::resizeWindow(PageSize _size)
{
    eventListener_.resizeWindow(_size.lines, _size.columns);
}

void Terminal::resizeWindow(ImageSize _size)
{
    eventListener_.resizeWindow(_size.width, _size.height);
}

void Terminal::setApplicationkeypadMode(bool _enabled)
{
    state_.inputGenerator.setApplicationKeypadMode(_enabled);
}

void Terminal::setBracketedPaste(bool _enabled)
{
    state_.inputGenerator.setBracketedPaste(_enabled);
}

void Terminal::setCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    cursorDisplay_ = _display;
    cursorShape_ = _shape;
}

void Terminal::setCursorVisibility(bool /*_visible*/)
{
    // don't do anything for now
}

void Terminal::setGenerateFocusEvents(bool _enabled)
{
    state_.inputGenerator.setGenerateFocusEvents(_enabled);
}

void Terminal::setMouseProtocol(MouseProtocol _protocol, bool _enabled)
{
    state_.inputGenerator.setMouseProtocol(_protocol, _enabled);
}

void Terminal::setMouseTransport(MouseTransport _transport)
{
    state_.inputGenerator.setMouseTransport(_transport);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode _mode)
{
    state_.inputGenerator.setMouseWheelMode(_mode);
}

void Terminal::setWindowTitle(string_view _title)
{
    state_.windowTitle = _title;
    eventListener_.setWindowTitle(_title);
}

std::string const& Terminal::windowTitle() const noexcept
{
    return state_.windowTitle;
}

void Terminal::saveWindowTitle()
{
    state_.savedWindowTitles.push(state_.windowTitle);
}

void Terminal::restoreWindowTitle()
{
    if (!state_.savedWindowTitles.empty())
    {
        state_.windowTitle = state_.savedWindowTitles.top();
        state_.savedWindowTitles.pop();
        setWindowTitle(state_.windowTitle);
    }
}

void Terminal::setTerminalProfile(string const& _configProfileName)
{
    eventListener_.setTerminalProfile(_configProfileName);
}

void Terminal::useApplicationCursorKeys(bool _enable)
{
    auto const keyMode = _enable ? KeyMode::Application : KeyMode::Normal;
    state_.inputGenerator.setCursorKeysMode(keyMode);
}

void Terminal::setMode(AnsiMode _mode, bool _enable)
{
    if (!isValidAnsiMode(static_cast<unsigned int>(_mode)))
        return;

    state_.modes.set(_mode, _enable);
}

void Terminal::setMode(DECMode _mode, bool _enable)
{
    if (!isValidDECMode(static_cast<unsigned int>(_mode)))
        return;

    switch (_mode)
    {
        case DECMode::AutoWrap: state_.cursor.autoWrap = _enable; break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                state_.margin.horizontal =
                    Margin::Horizontal { ColumnOffset(0),
                                         boxed_cast<ColumnOffset>(state_.pageSize.columns - 1) };
            break;
        case DECMode::Origin: state_.cursor.originMode = _enable; break;
        case DECMode::Columns132:
            if (!isModeEnabled(DECMode::AllowColumns80to132))
                break;
            if (_enable != isModeEnabled(DECMode::Columns132))
            {
                auto const clear = _enable != isModeEnabled(_mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(_enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case DECMode::BatchedRendering:
            if (state_.modes.enabled(DECMode::BatchedRendering) != _enable)
                synchronizedOutput(_enable);
            break;
        case DECMode::TextReflow:
            if (state_.allowReflowOnResize && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = _enable ? LineOffset(0) : realCursorPosition().line;
                for (auto line = startLine; line < boxed_cast<LineOffset>(state_.pageSize.lines); ++line)
                    primaryScreen_.grid().lineAt(line).setWrappable(_enable);
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(_enable);
            break;
        case DECMode::UseAlternateScreen:
            if (_enable)
                setScreen(ScreenType::Alternate);
            else
                setScreen(ScreenType::Primary);
            break;
        case DECMode::UseApplicationCursorKeys:
            useApplicationCursorKeys(_enable);
            if (isAlternateScreen())
            {
                if (_enable)
                    setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case DECMode::BracketedPaste: setBracketedPaste(_enable); break;
        case DECMode::MouseSGR:
            if (_enable)
                setMouseTransport(MouseTransport::SGR);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseExtended: setMouseTransport(MouseTransport::Extended); break;
        case DECMode::MouseURXVT: setMouseTransport(MouseTransport::URXVT); break;
        case DECMode::MouseSGRPixels:
            if (_enable)
                setMouseTransport(MouseTransport::SGRPixels);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseAlternateScroll:
            if (_enable)
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
        case DECMode::FocusTracking: setGenerateFocusEvents(_enable); break;
        case DECMode::UsePrivateColorRegisters: state_.usePrivateColorRegisters = _enable; break;
        case DECMode::VisibleCursor:
            state_.cursor.visible = _enable;
            setCursorVisibility(_enable);
            break;
        case DECMode::MouseProtocolX10: setMouseProtocol(MouseProtocol::X10, _enable); break;
        case DECMode::MouseProtocolNormalTracking:
            setMouseProtocol(MouseProtocol::NormalTracking, _enable);
            break;
        case DECMode::MouseProtocolHighlightTracking:
            setMouseProtocol(MouseProtocol::HighlightTracking, _enable);
            break;
        case DECMode::MouseProtocolButtonTracking:
            setMouseProtocol(MouseProtocol::ButtonTracking, _enable);
            break;
        case DECMode::MouseProtocolAnyEventTracking:
            setMouseProtocol(MouseProtocol::AnyEventTracking, _enable);
            break;
        case DECMode::SaveCursor:
            if (_enable)
                saveCursor();
            else
                restoreCursor();
            break;
        case DECMode::ExtendedAltScreen:
            if (_enable)
            {
                state_.savedPrimaryCursor = cursor();
                setMode(DECMode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(DECMode::UseAlternateScreen, false);
                restoreCursor(state_.savedPrimaryCursor);
            }
            break;
        default: break;
    }

    state_.modes.set(_mode, _enable);
}

void Terminal::setTopBottomMargin(optional<LineOffset> _top, optional<LineOffset> _bottom)
{
    auto const defaultTop = LineOffset(0);
    auto const defaultBottom = boxed_cast<LineOffset>(state_.pageSize.lines) - 1;
    auto const top = max(defaultTop, _top.value_or(defaultTop));
    auto const bottom = min(defaultBottom, _bottom.value_or(defaultBottom));

    if (top < bottom)
    {
        state_.margin.vertical.from = top;
        state_.margin.vertical.to = bottom;
        moveCursorTo({}, {});
    }
}

void Terminal::setLeftRightMargin(optional<ColumnOffset> _left, optional<ColumnOffset> _right)
{
    if (isModeEnabled(DECMode::LeftRightMargin))
    {
        auto const right =
            _right.has_value()
                ? min(_right.value(), boxed_cast<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1))
                : boxed_cast<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1);
        auto const left = _left.value_or(ColumnOffset(0));
        if (left < right)
        {
            state_.margin.horizontal.from = left;
            state_.margin.horizontal.to = right;
            moveCursorTo({}, {});
        }
    }
}

void Terminal::clearScreen()
{
    if (isPrimaryScreen())
        primaryScreen_.clearScreen();
    else
        alternateScreen_.clearScreen();
}

void Terminal::moveCursorTo(LineOffset _line, ColumnOffset _column)
{
    auto const [line, column] = [&]() {
        if (!state_.cursor.originMode)
            return pair { _line, _column };
        else
            return pair { _line + state_.margin.vertical.from, _column + state_.margin.horizontal.from };
    }();

    state_.wrapPending = false;
    state_.cursor.position.line = clampedLine(line);
    state_.cursor.position.column = clampedColumn(column);
}

void Terminal::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    state_.savedCursor = state_.cursor;
}

void Terminal::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    restoreCursor(state_.savedCursor);

    setMode(DECMode::AutoWrap, state_.savedCursor.autoWrap);
    setMode(DECMode::Origin, state_.savedCursor.originMode);
}

void Terminal::restoreCursor(Cursor const& _savedCursor)
{
    state_.wrapPending = false;
    state_.cursor = _savedCursor;
    state_.cursor.position = clampCoordinate(_savedCursor.position);
    verifyState();
}

void Terminal::softReset()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, state_.allowReflowOnResize);
    setGraphicsRendition(GraphicsRendition::Reset);    // SGR
    state_.savedCursor.position = {};                  // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true);             // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);                   // DECOM
    setMode(AnsiMode::KeyboardAction, false);          // KAM
    setMode(DECMode::AutoWrap, false);                 // DECAWM
    setMode(AnsiMode::Insert, false);                  // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, boxed_cast<LineOffset>(state_.pageSize.lines) - LineOffset(1));       // DECSTBM
    setLeftRightMargin({}, boxed_cast<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1)); // DECRLM

    state_.cursor.hyperlink = {};
    state_.colorPalette = state_.defaultColorPalette;

    // TODO: DECNKM (Numeric keypad)
    // TODO: DECSCA (Select character attribute)
    // TODO: DECNRCM (National replacement character set)
    // TODO: GL, GR (G0, G1, G2, G3)
    // TODO: DECAUPSS (Assign user preference supplemental set)
    // TODO: DECSASD (Select active status display)
    // TODO: DECKPM (Keyboard position mode)
    // TODO: DECPCTERM (PCTerm mode)
}

void Terminal::setGraphicsRendition(GraphicsRendition _rendition)
{
    // TODO: optimize this as there are only 3 cases
    // 1.) reset
    // 2.) set some bits |=
    // 3.) clear some bits &= ~
    switch (_rendition)
    {
        case GraphicsRendition::Reset: state_.cursor.graphicsRendition = {}; break;
        case GraphicsRendition::Bold: state_.cursor.graphicsRendition.styles |= CellFlags::Bold; break;
        case GraphicsRendition::Faint: state_.cursor.graphicsRendition.styles |= CellFlags::Faint; break;
        case GraphicsRendition::Italic: state_.cursor.graphicsRendition.styles |= CellFlags::Italic; break;
        case GraphicsRendition::Underline:
            state_.cursor.graphicsRendition.styles |= CellFlags::Underline;
            break;
        case GraphicsRendition::Blinking:
            state_.cursor.graphicsRendition.styles |= CellFlags::Blinking;
            break;
        case GraphicsRendition::Inverse: state_.cursor.graphicsRendition.styles |= CellFlags::Inverse; break;
        case GraphicsRendition::Hidden: state_.cursor.graphicsRendition.styles |= CellFlags::Hidden; break;
        case GraphicsRendition::CrossedOut:
            state_.cursor.graphicsRendition.styles |= CellFlags::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            state_.cursor.graphicsRendition.styles |= CellFlags::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            state_.cursor.graphicsRendition.styles |= CellFlags::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            state_.cursor.graphicsRendition.styles |= CellFlags::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            state_.cursor.graphicsRendition.styles |= CellFlags::DashedUnderline;
            break;
        case GraphicsRendition::Framed: state_.cursor.graphicsRendition.styles |= CellFlags::Framed; break;
        case GraphicsRendition::Overline:
            state_.cursor.graphicsRendition.styles |= CellFlags::Overline;
            break;
        case GraphicsRendition::Normal:
            state_.cursor.graphicsRendition.styles &= ~(CellFlags::Bold | CellFlags::Faint);
            break;
        case GraphicsRendition::NoItalic: state_.cursor.graphicsRendition.styles &= ~CellFlags::Italic; break;
        case GraphicsRendition::NoUnderline:
            state_.cursor.graphicsRendition.styles &=
                ~(CellFlags::Underline | CellFlags::DoublyUnderlined | CellFlags::CurlyUnderlined
                  | CellFlags::DottedUnderline | CellFlags::DashedUnderline);
            break;
        case GraphicsRendition::NoBlinking:
            state_.cursor.graphicsRendition.styles &= ~CellFlags::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            state_.cursor.graphicsRendition.styles &= ~CellFlags::Inverse;
            break;
        case GraphicsRendition::NoHidden: state_.cursor.graphicsRendition.styles &= ~CellFlags::Hidden; break;
        case GraphicsRendition::NoCrossedOut:
            state_.cursor.graphicsRendition.styles &= ~CellFlags::CrossedOut;
            break;
        case GraphicsRendition::NoFramed: state_.cursor.graphicsRendition.styles &= ~CellFlags::Framed; break;
        case GraphicsRendition::NoOverline:
            state_.cursor.graphicsRendition.styles &= ~CellFlags::Overline;
            break;
    }
}

void Terminal::setForegroundColor(Color _color)
{
    state_.cursor.graphicsRendition.foregroundColor = _color;
}

void Terminal::setBackgroundColor(Color _color)
{
    state_.cursor.graphicsRendition.backgroundColor = _color;
}

void Terminal::setUnderlineColor(Color _color)
{
    state_.cursor.graphicsRendition.underlineColor = _color;
}

void Terminal::hardReset()
{
    setScreen(ScreenType::Primary);

    state_.modes = Modes {};
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::TextReflow, state_.allowReflowOnResize);
    setMode(DECMode::SixelCursorNextToGraphic, state_.sixelCursorConformance);

    state_.primaryBuffer.reset();
    state_.alternateBuffer.reset();

    state_.imagePool.clear();

    state_.cursor = {};
    state_.tabs.clear();

    state_.lastCursorPosition = state_.cursor.position;

    state_.margin =
        Margin { Margin::Vertical { {}, boxed_cast<LineOffset>(state_.pageSize.lines) - 1 },
                 Margin::Horizontal { {}, boxed_cast<ColumnOffset>(state_.pageSize.columns) - 1 } };

    state_.colorPalette = state_.defaultColorPalette;

    primaryScreen_.verifyState();

    state_.inputGenerator.reset();
}

void Terminal::setScreen(ScreenType _type)
{
    if (_type == state_.screenType)
        return;

    switch (_type)
    {
        case ScreenType::Primary:
            currentScreen_ = primaryScreen_;
            setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
            break;
        case ScreenType::Alternate:
            currentScreen_ = alternateScreen_;
            if (isModeEnabled(DECMode::MouseAlternateScroll))
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
    }

    state_.screenType = _type;

    // Reset wrapPending-flag when switching buffer.
    state_.wrapPending = false;

    // Reset last-cursor position.
    state_.lastCursorPosition = state_.cursor.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    bufferChanged(_type);
}

void Terminal::applyPageSizeToCurrentBuffer()
{
    auto cursorPosition = state_.cursor.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    cursorPosition = isPrimaryScreen()
                         ? state_.primaryBuffer.resize(state_.pageSize, cursorPosition, state_.wrapPending)
                         : state_.alternateBuffer.resize(state_.pageSize, cursorPosition, state_.wrapPending);
    cursorPosition = clampCoordinate(cursorPosition);

    if (state_.cursor.position.column < boxed_cast<ColumnOffset>(state_.pageSize.columns))
        state_.wrapPending = false;

    // update (last-)cursor position
    state_.cursor.position = cursorPosition;
    state_.lastCursorPosition = cursorPosition;

    // truncating tabs
    while (!state_.tabs.empty() && state_.tabs.back() >= unbox<ColumnOffset>(state_.pageSize.columns))
        state_.tabs.pop_back();

        // TODO: find out what to do with DECOM mode. Reset it to?
#if 0
    inspect("after resize", std::cout);
    fmt::print("applyPageSizeToCurrentBuffer: cursor pos before: {} after: {}\n", oldCursorPos, state_.cursor.position);
#endif

    verifyState();
}

void Terminal::discardImage(Image const& _image)
{
    eventListener_.discardImage(_image);
}

void Terminal::markCellDirty(CellLocation _position) noexcept
{
    if (!selection_)
        return;

    if (selection_->contains(_position))
        clearSelection();
}

void Terminal::markRegionDirty(Rect _area) noexcept
{
    if (!selection_)
        return;

    if (selection_->intersects(_area))
        clearSelection();
}

void Terminal::synchronizedOutput(bool _enabled)
{
    renderBufferUpdateEnabled_ = !_enabled;
    if (_enabled)
        return;

    auto const diff = currentTime_ - renderBuffer_.lastUpdate;
    if (diff < refreshInterval_)
        return;

    if (renderBuffer_.state == RenderBufferState::TrySwapBuffers)
        return;

    refreshRenderBuffer(true);
    eventListener_.screenUpdated();
}

void Terminal::onBufferScrolled(LineCount _n) noexcept
{
    if (!selection_)
        return;

    auto const top = -boxed_cast<LineOffset>(primaryScreen_.historyLineCount());
    if (selection_->from().line > top && selection_->to().line > top)
        selection_->applyScroll(boxed_cast<LineOffset>(_n), primaryScreen_.historyLineCount());
    else
        selection_.reset();
}
// }}}

void Terminal::setMaxHistoryLineCount(LineCount _maxHistoryLineCount)
{
    primaryScreen_.grid().setMaxHistoryLineCount(_maxHistoryLineCount);
}

LineCount Terminal::maxHistoryLineCount() const noexcept
{
    return primaryScreen_.grid().maxHistoryLineCount();
}

} // namespace terminal
