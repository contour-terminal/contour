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
#include <terminal/Terminal.h>
#include <terminal/logging.h>

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

    constexpr RGBColor makeRGBColor(RGBColor fg, RGBColor bg, CellRGBColor cellColor) noexcept
    {
        if (holds_alternative<CellForegroundColor>(cellColor))
            return fg;
        if (holds_alternative<CellBackgroundColor>(cellColor))
            return bg;
        return get<RGBColor>(cellColor);
    }

    tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette,
                                         Cell const& _cell,
                                         bool _reverseVideo,
                                         bool _selected,
                                         bool _isCursor)
    {
        auto const [fg, bg] = _cell.makeColors(_colorPalette, _reverseVideo);
        if (!_selected && !_isCursor)
            return tuple { fg, bg };

        auto const [selectionFg, selectionBg] =
            [](auto fg, auto bg, bool selected, ColorPalette const& colors) -> tuple<RGBColor, RGBColor> {
            auto const a = colors.selectionForeground.value_or(bg);
            auto const b = colors.selectionBackground.value_or(fg);
            if (selected)
                return tuple { a, b };
            else
                return tuple { b, a };
        }(fg, bg, _selected, _colorPalette);
        if (!_isCursor)
            return tuple { selectionFg, selectionBg };

        auto const cursorFg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.textOverrideColor);
        auto const cursorBg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.color);

        return tuple { cursorFg, cursorBg };
    }

#if defined(CONTOUR_PERF_STATS)
    void logRenderBufferSwap(bool _success, uint64_t _frameID)
    {
        if (!RenderBufferLog)
            return;

        if (_success)
            LOGSTORE(RenderBufferLog)("Render buffer {} swapped.", _frameID);
        else
            LOGSTORE(RenderBufferLog)("Render buffer {} swapping failed.", _frameID);
    }
#endif
} // namespace
// }}}

Terminal::Terminal(Pty& _pty,
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
    pty_ { _pty },
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
             pty_.pageSize(),
             _maxHistoryLineCount,
             _maxImageSize,
             _maxImageColorRegisters,
             _sixelCursorConformance,
             move(_colorPalette),
             _allowReflowOnResize },
    screen_ { state_, ScreenType::Main },
    // clang-format on
    viewport_ { screen_,
                [this]() {
                    breakLoopAndRefreshRenderBuffer();
                } },
    selectionHelper_ { this }
{
#if 0
    resetHard();
#else
    screen().setMode(DECMode::AutoWrap, true);
    screen().setMode(DECMode::TextReflow, true);
    screen().setMode(DECMode::SixelCursorNextToGraphic, state_.sixelCursorConformance);
#endif
}

Terminal::~Terminal()
{
    state_.terminating = true;
    pty_.wakeupReader();

    if (screenUpdateThread_)
        screenUpdateThread_->join();
}

void Terminal::start()
{
    screenUpdateThread_ = make_unique<std::thread>(bind(&Terminal::mainLoop, this));
}

void Terminal::resetHard()
{
    screen_.resetHard();
}

void Terminal::setRefreshRate(double _refreshRate)
{
    refreshInterval_ = std::chrono::milliseconds(static_cast<long long>(1000.0 / _refreshRate));
}

void Terminal::setLastMarkRangeOffset(LineOffset _value) noexcept
{
    copyLastMarkRangeOffset_ = _value;
}

void Terminal::mainLoop()
{
    mainLoopThreadID_ = this_thread::get_id();

    TerminalLog()("Starting main loop with thread id {}", [&]() {
        stringstream sstr;
        sstr << mainLoopThreadID_;
        return sstr.str();
    }());

    while (!state_.terminating)
    {
        if (!processInputOnce())
            break;
    }

    LOGSTORE(TerminalLog)("Event loop terminating (PTY {}).", pty_.isClosed() ? "closed" : "open");
    eventListener_.onClosed();
}

bool Terminal::processInputOnce()
{
    auto const timeout = renderBuffer_.state == RenderBufferState::WaitingForRefresh && !screenDirty_
                             ? std::chrono::seconds(4)
                             //: refreshInterval_ : std::chrono::seconds(0)
                             : std::chrono::seconds(30);

    auto const bufOpt = pty_.read(ptyReadBufferSize_, timeout);
    if (!bufOpt)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            TerminalLog()("PTY read failed (timeout: {}). {}", timeout, strerror(errno));
            pty_.close();
        }
        return errno == EINTR || errno == EAGAIN;
    }
    auto const buf = *bufOpt;

    if (buf.empty())
    {
        TerminalLog()("PTY read returned with zero bytes. Closing PTY.");
        pty_.close();
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

    if (this_thread::get_id() == mainLoopThreadID_)
        return;

    pty_.wakeupReader();
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

RenderCell makeRenderCell(ColorPalette const& _colorPalette,
                          HyperlinkStorage const& _hyperlinks,
                          Cell const& _cell,
                          RGBColor fg,
                          RGBColor bg,
                          LineOffset _line,
                          ColumnOffset _column)
{
    RenderCell cell;
    cell.backgroundColor = bg;
    cell.foregroundColor = fg;
    cell.decorationColor = _cell.getUnderlineColor(_colorPalette, fg);
    cell.position.line = _line;
    cell.position.column = _column;
    cell.flags = _cell.styles();
    cell.width = _cell.width();

    if (_cell.codepointCount() != 0)
    {
        for (size_t i = 0; i < _cell.codepointCount(); ++i)
            cell.codepoints.push_back(_cell.codepoint(i));
    }

    cell.image = _cell.imageFragment();

    if (auto href = _hyperlinks.hyperlinkById(_cell.hyperlink()))
    {
        auto const& color = href->state == HyperlinkState::Hover ? _colorPalette.hyperlinkDecoration.hover
                                                                 : _colorPalette.hyperlinkDecoration.normal;
        // TODO(decoration): Move property into Terminal.
        auto const decoration =
            href->state == HyperlinkState::Hover
                ? CellFlags::Underline        // TODO: decorationRenderer_.hyperlinkHover()
                : CellFlags::DottedUnderline; // TODO: decorationRenderer_.hyperlinkNormal();
        cell.flags |= decoration;             // toCellStyle(decoration);
        cell.decorationColor = color;
    }

    return cell;
}

PageSize Terminal::SelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::SelectionHelper::wordDelimited(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    Cell const& cell = terminal->screen().at(_pos);
    return cell.empty()
           || terminal->wordDelimiters_.find(cell.codepoint(0)) != terminal->wordDelimiters_.npos;
}

bool Terminal::SelectionHelper::wrappedLine(LineOffset _line) const noexcept
{
    return terminal->isLineWrapped(_line);
}

bool Terminal::SelectionHelper::cellEmpty(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->screen().at(_pos).empty();
}

int Terminal::SelectionHelper::cellWidth(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->screen().at(_pos).width();
}

void Terminal::refreshRenderBufferInternal(RenderBuffer& _output)
{
    verifyState();

    auto const renderHyperlinks = screen_.contains(currentMousePosition_);

    auto const currentMousePositionRel = viewport_.translateScreenToGridCoordinate(currentMousePosition_);

    changes_.store(0);
    screenDirty_ = false;
    ++lastFrameID_;

    _output.clear();
    _output.frameID = lastFrameID_;

    enum class State
    {
        Gap,
        Sequence
    };

#if defined(CONTOUR_PERF_STATS)
    if (TerminalLog)
        LOGSTORE(TerminalLog)("{}: Refreshing render buffer.\n", lastFrameID_.load());
#endif

    shared_ptr<HyperlinkInfo> href =
        renderHyperlinks ? screen_.hyperlinkAt(currentMousePositionRel) : shared_ptr<HyperlinkInfo> {};
    if (href)
        href->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?

    _output.cursor = renderCursor();
    auto const reverseVideo = screen_.isModeEnabled(terminal::DECMode::ReverseVideo);
    screen_.render(
        [this,
         reverseVideo,
         &_output,
         prevWidth = 0,
         prevHasCursor = false,
         state = State::Gap,
         lineNr = LineOffset(0)](Cell const& _cell, LineOffset _line, ColumnOffset _column) mutable {
            // clang-format off
            auto const selected = isSelected( CellLocation { _line - boxed_cast<LineOffset>(viewport_.scrollOffset()), _column });
            auto const pos = CellLocation { _line, _column };
            auto const gridPosition = viewport_.translateScreenToGridCoordinate(pos);
            auto const hasCursor = gridPosition == screen_.realCursorPosition();
            bool const paintCursor =
                (hasCursor || (prevHasCursor && prevWidth == 2))
                    && _output.cursor.has_value()
                    && _output.cursor->shape == CursorShape::Block;
            auto const [fg, bg] = makeColors(screen_.colorPalette(), _cell, reverseVideo, selected, paintCursor);
            // clang-format on

            prevWidth = _cell.width();
            prevHasCursor = hasCursor;

            auto const cellEmpty = _cell.empty();
            auto const customBackground = bg != screen_.colorPalette().defaultBackground || !!_cell.styles();

            bool isNewLine = false;
            if (lineNr != _line)
            {
                isNewLine = true;
                lineNr = _line;
                prevWidth = 0;
                prevHasCursor = false;
                if (!_output.screen.empty())
                    _output.screen.back().groupEnd = true;
            }

            switch (state)
            {
            case State::Gap:
                if (!cellEmpty || customBackground)
                {
                    state = State::Sequence;
                    // clang-format off
                    _output.screen.emplace_back(makeRenderCell(screen_.colorPalette(),
                                                               screen_.hyperlinks(),
                                                               _cell,
                                                               fg,
                                                               bg,
                                                               _line,
                                                               _column));
                    // clang-format on
                    _output.screen.back().groupStart = true;
                }
                break;
            case State::Sequence:
                if (cellEmpty && !customBackground)
                {
                    _output.screen.back().groupEnd = true;
                    state = State::Gap;
                }
                else
                {
                    // clang-format off
                    _output.screen.emplace_back(makeRenderCell(screen_.colorPalette(),
                                                               screen_.hyperlinks(),
                                                               _cell,
                                                               fg,
                                                               bg,
                                                               _line,
                                                               _column));
                    // clang-format on

                    if (isNewLine)
                        _output.screen.back().groupStart = true;
                }
                break;
            }
        },
        viewport_.scrollOffset());

    if (href)
        href->state = HyperlinkState::Inactive;
}

optional<RenderCursor> Terminal::renderCursor()
{
    if (!cursorCurrentlyVisible() || !viewport().isLineVisible(screen_.cursor().position.line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    Cell const& cursorCell = screen_.at(screen_.cursor().position);

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = screen_.focused() ? cursorShape() : InactiveCursorShape;

    return RenderCursor { CellLocation { screen_.cursor().position.line
                                             + viewport_.scrollOffset().as<LineOffset>(),
                                         screen_.cursor().position.column },
                          shape,
                          cursorCell.width() };
}
// }}}

bool Terminal::sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
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
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
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

    if (newPosition == currentMousePosition_ && !screen_.isModeEnabled(DECMode::MouseSGRPixels))
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
    screen_.setFocus(true);
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
    screen_.setFocus(false);
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
    auto const rv = pty_.write(input.data(), input.size());
    if (rv > 0)
        state_.inputGenerator.consume(rv);
}

void Terminal::writeToScreen(string_view _data)
{
    auto const _l = lock_guard { *this };
    screen_.write(_data);
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

    if (!screen_.contains(currentMousePosition_))
        return false;

    auto const relCursorPos = viewport_.translateScreenToGridCoordinate(currentMousePosition_);

    auto const newState = screen_.contains(currentMousePosition_) && !!screen_.at(relCursorPos).hyperlink();

    auto const oldState = hoveringHyperlink_.exchange(newState);
    return newState != oldState;
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    if (!screen_.cursor().visible)
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

    screen_.resize(_cells);
    if (_pixels)
    {
        auto width = Width(*_pixels->width / _cells.columns.as<unsigned>());
        auto height = Height(*_pixels->height / _cells.lines.as<unsigned>());
        screen_.setCellPixelSize(ImageSize { width, height });
    }

    currentMousePosition_.column =
        min(currentMousePosition_.column, boxed_cast<ColumnOffset>(_cells.columns - 1));
    currentMousePosition_.line = min(currentMousePosition_.line, boxed_cast<LineOffset>(_cells.lines - 1));

    pty_.resizeScreen(_cells, _pixels);

    verifyState();
}

void Terminal::verifyState()
{
    Require(*currentMousePosition_.column < *pageSize().columns);
    Require(*currentMousePosition_.line < *pageSize().lines);
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
            _pos.line.value > 0
            && isSelected({ _pos.line - 1, screen_.pageSize().columns.as<ColumnOffset>() - 1 });
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
    auto const bottomLine = screen_.cursor().position.line + LineOffset(-1) + copyLastMarkRangeOffset_;

    auto const marker1 = optional { bottomLine };

    auto const marker0 = screen_.findMarkerUpwards(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 + 1;
    auto const lastLine = *marker1;

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        auto const lineText = screen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += screen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += '\n';
    }

    return text;
}

// {{{ ScreenEvents overrides
void Terminal::requestCaptureBuffer(int _absoluteStartLine, int _lineCount)
{
    return eventListener_.requestCaptureBuffer(_absoluteStartLine, _lineCount);
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
    eventListener_.setWindowTitle(_title);
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

void Terminal::hardReset()
{
    // NB: Screen was already reset.
    state_.inputGenerator.reset();
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

    auto const top = -boxed_cast<LineOffset>(screen_.historyLineCount());
    if (selection_->from().line > top && selection_->to().line > top)
        selection_->applyScroll(boxed_cast<LineOffset>(_n), screen_.historyLineCount());
    else
        selection_.reset();
}
// }}}

} // namespace terminal
