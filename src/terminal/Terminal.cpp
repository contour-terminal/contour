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
#include <terminal/Terminal.h>

#include <terminal/ControlCode.h>
#include <terminal/InputGenerator.h>
#include <terminal/logging.h>

#include <crispy/escape.h>
#include <crispy/stdfs.h>

#include <chrono>
#include <utility>

#include <fmt/chrono.h>

#include <iostream>

#include <sys/types.h>
#include <signal.h>

using crispy::Size;

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using std::move;

namespace terminal {

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
        auto const [fg, bg] = _cell.attributes().makeColors(_colorPalette, _reverseVideo);
        if (!_selected && !_isCursor)
            return tuple{fg, bg};

        auto const [selectionFg, selectionBg] =
                [](auto fg, auto bg, bool selected, ColorPalette const& colors) -> tuple<RGBColor, RGBColor> {
            auto const a = colors.selectionForeground.value_or(bg);
            auto const b = colors.selectionBackground.value_or(fg);
            if (selected)
                return tuple{a, b};
            else
                return tuple{b, a};
        }(fg, bg, _selected, _colorPalette);
        if (!_isCursor)
            return tuple{selectionFg, selectionBg};

        auto const cursorFg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.textOverrideColor);
        auto const cursorBg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.color);

        return tuple{cursorFg, cursorBg};
    }

    void logRenderBufferSwap(bool _success, uint64_t _frameID)
    {
        if (!RenderBufferLog)
            return;

        if (_success)
            LOGSTORE(RenderBufferLog)("Render buffer {} swapped.", _frameID);
        else
            LOGSTORE(RenderBufferLog)("Render buffer {} swapping failed.", _frameID);
    }
}
// }}}

Terminal::Terminal(Pty& _pty,
                   int _ptyReadBufferSize,
                   Terminal::Events& _eventListener,
                   optional<LineCount> _maxHistoryLineCount,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   string const& _wordDelimiters,
                   Modifier _mouseProtocolBypassModifier,
                   ImageSize _maxImageSize,
                   int _maxImageColorRegisters,
                   bool _sixelCursorConformance,
                   ColorPalette _colorPalette,
                   double _refreshRate,
                   bool _allowReflowOnResize
) :
    changes_{ 0 },
    ptyReadBufferSize_{ _ptyReadBufferSize },
    eventListener_{ _eventListener },
    refreshInterval_{ static_cast<long long>(1000.0 / _refreshRate) },
    renderBuffer_{},
    pty_{ _pty },
    startTime_{ _now },
    currentTime_{ _now },
    lastCursorBlink_{ _now },
    cursorDisplay_{ CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_{ CursorShape::Block }, // TODO: pass via param
    cursorBlinkInterval_{ _cursorBlinkInterval },
    cursorBlinkState_{ 1 },
    wordDelimiters_{ unicode::from_utf8(_wordDelimiters) },
    mouseProtocolBypassModifier_{ _mouseProtocolBypassModifier },
    inputGenerator_{},
    screen_{
        pty_.screenSize(),
        *this,
        true, // logs raw output by default?
        true, // logs trace output by default?
        _maxHistoryLineCount,
        _maxImageSize,
        _maxImageColorRegisters,
        _sixelCursorConformance,
        _colorPalette,
        _allowReflowOnResize
    },
    screenUpdateThread_{},
    viewport_{ screen_, [this]() { breakLoopAndRefreshRenderBuffer(); } }
{
}

Terminal::~Terminal()
{
    pty_.wakeupReader();

    if (screenUpdateThread_)
        screenUpdateThread_->join();
}

void Terminal::start()
{
    screenUpdateThread_ = make_unique<std::thread>(bind(&Terminal::mainLoop, this));
}

void Terminal::setRefreshRate(double _refreshRate)
{
    refreshInterval_ = std::chrono::milliseconds(static_cast<long long>(1000.0 / _refreshRate));
}

void Terminal::mainLoop()
{
    mainLoopThreadID_ = this_thread::get_id();

    LOGSTORE(TerminalLog)(
        "Starting main loop with thread id {}",
        [&]() {
            stringstream sstr;
            sstr << mainLoopThreadID_;
            return sstr.str();
        }()
    );

    for (;;)
    {
        if (!processInputOnce())
            break;
    }

    LOGSTORE(TerminalLog)("Event loop terminating (PTY {}).", pty_.isClosed() ? "closed" : "open");
    eventListener_.onClosed();
}

bool Terminal::processInputOnce()
{
    auto const timeout =
        renderBuffer_.state == RenderBufferState::WaitingForRefresh && !screenDirty_
            ? std::chrono::seconds(4)
            : refreshInterval_ // std::chrono::seconds(0)
            ;

    auto const bufOpt = pty_.read(ptyReadBufferSize_, timeout);
    if (!bufOpt)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            LOGSTORE(TerminalLog)("PTY read failed (timeout: {}). {}",
                                  timeout,
                                  strerror(errno));
            pty_.close();
        }
        return errno == EINTR || errno == EAGAIN;
    }
    auto const buf = *bufOpt;

    if (buf.empty())
    {
        LOGSTORE(TerminalLog)("PTY read returned with zero bytes. Closing PTY.");
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
        //renderBuffer_.state = RenderBufferState::WaitingForRefresh;
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
        case RenderBufferState::TrySwapBuffers:
            {
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
    auto const _l = lock_guard{*this};
    refreshRenderBufferInternal(_output);
}

void Terminal::refreshRenderBufferInternal(RenderBuffer& _output)
{
    auto const reverseVideo = screen_.isModeEnabled(terminal::DECMode::ReverseVideo);
    auto const baseLine =
        viewport_.absoluteScrollOffset().
        value_or(boxed_cast<StaticScrollbackPosition>(screen_.historyLineCount())).
        as<int>();

    #if defined(LIBTERMINAL_HYPERLINKS)
    auto const renderHyperlinks = screen_.contains(currentMousePosition_);
    #endif

    auto const currentMousePositionRel = Coordinate{
        currentMousePosition_.row - unbox<int>(viewport_.relativeScrollOffset()),
        currentMousePosition_.column
    };

    changes_.store(0);
    screenDirty_ = false;
    ++lastFrameID_;

    _output.clear();
    _output.frameID = lastFrameID_;

    enum class State { Gap, Sequence };
    State state = State::Gap;

#if defined(CONTOUR_PERF_STATS)
    if (TerminalLog)
        LOGSTORE(TerminalLog)("{}: Refreshing render buffer.\n", lastFrameID_.load());
#endif

    #if defined(LIBTERMINAL_HYPERLINKS)
    if (renderHyperlinks)
    {
        auto& cellAtMouse = screen_.at(currentMousePositionRel);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
    }
    #endif

    // {{{ void appendCell(pos, cell, fg, bg)
    auto const appendCell = [&](Coordinate const& _pos, Cell const& _cell,
                                RGBColor fg, RGBColor bg)
    {
        RenderCell cell;
        cell.backgroundColor = bg;
        cell.foregroundColor = fg;
        cell.decorationColor = _cell.attributes().getUnderlineColor(screen_.colorPalette(), fg);
        cell.position = _pos;
        cell.flags = _cell.attributes().styles;

        if (!_cell.codepoints().empty())
            cell.codepoints = _cell.codepoints();

#if defined(LIBTERMINAL_IMAGES)
        if (optional<ImageFragment> const& fragment = _cell.imageFragment(); fragment.has_value())
        {
            cell.flags |= CellFlags::Image; // TODO: this should already be there.
            cell.image = _cell.imageFragment();
        }
#endif

        #if defined(LIBTERMINAL_HYPERLINKS)
        if (_cell.hyperlink())
        {
            auto const& color = _cell.hyperlink()->state == HyperlinkState::Hover
                                ? screen_.colorPalette().hyperlinkDecoration.hover
                                : screen_.colorPalette().hyperlinkDecoration.normal;
            // TODO(decoration): Move property into Terminal.
            auto const decoration = _cell.hyperlink()->state == HyperlinkState::Hover
                                    ? CellFlags::Underline          // TODO: decorationRenderer_.hyperlinkHover()
                                    : CellFlags::DottedUnderline;   // TODO: decorationRenderer_.hyperlinkNormal();
            cell.flags |= decoration; // toCellStyle(decoration);
            cell.decorationColor = color;
        }
        #endif

        _output.screen.emplace_back(std::move(cell));
    }; // }}}

    _output.cursor = renderCursor();
    int lineNr = 1;
    screen_.render(
        [&](Coordinate _pos, Cell const& _cell)
        {
            auto const absolutePos = Coordinate{baseLine + (_pos.row - 1), _pos.column};
            auto const selected = isSelectedAbsolute(absolutePos);
            auto const hasCursor = viewport_.translateScreenToGridCoordinate(_pos) == screen_.realCursorPosition();
            bool const paintCursor = hasCursor
                                  && _output.cursor.has_value()
                                  && _output.cursor->shape == CursorShape::Block;
            auto const [fg, bg] = makeColors(screen_.colorPalette(), _cell,
                                             reverseVideo, selected,
                                             paintCursor);

            auto const cellEmpty = _cell.empty();
            auto const customBackground = bg != screen_.colorPalette().defaultBackground
                                       || !!_cell.attributes().styles;

            bool isNewLine = false;
            if (lineNr != _pos.row)
            {
                isNewLine = true;
                lineNr = _pos.row;
                if (!_output.screen.empty())
                    _output.screen.back().flags |= CellFlags::CellSequenceEnd;
            }

            switch (state)
            {
                case State::Gap:
                    if (!cellEmpty || customBackground)
                    {
                        state = State::Sequence;
                        appendCell(_pos, _cell, fg, bg);
                        _output.screen.back().flags |= CellFlags::CellSequenceStart;
                    }
                    break;
                case State::Sequence:
                    if (cellEmpty && !customBackground)
                    {
                        _output.screen.back().flags |= CellFlags::CellSequenceEnd;
                        state = State::Gap;
                    }
                    else
                    {
                        appendCell(_pos, _cell, fg, bg);

                        if (isNewLine)
                            _output.screen.back().flags |= CellFlags::CellSequenceStart;
                    }
                    break;
            }
        },
        viewport_.absoluteScrollOffset()
    );

    #if defined(LIBTERMINAL_HYPERLINKS)
    if (renderHyperlinks)
    {
        auto& cellAtMouse = screen_.at(currentMousePositionRel);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
    }
    #endif
}

optional<RenderCursor> Terminal::renderCursor()
{
    if (!cursorCurrentlyVisible() || !viewport().isLineVisible(screen_.cursor().position.row))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    Cell const& cursorCell = screen_.at(screen_.cursor().position);

    auto const shape = screen_.focused() ? cursorShape()
                                         : CursorShape::Rectangle;

    return RenderCursor{
        Coordinate(
            screen_.cursor().position.row + viewport_.relativeScrollOffset().as<int>(),
            screen_.cursor().position.column
        ),
        shape,
        cursorCell.width()
    };
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
    bool const success = inputGenerator_.generate(_key, _modifier);
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

    auto const success = inputGenerator_.generate(_value, _modifier);

    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendMousePressEvent(MouseButton _button, Modifier _modifier, Timestamp _now)
{
    respectMouseProtocol_ = mouseProtocolBypassModifier_ == Modifier::None
                         || !_modifier.contains(mouseProtocolBypassModifier_);

    if (respectMouseProtocol_ && inputGenerator_.generateMousePress(_button, _modifier, currentMousePosition_))
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
    double const diff_ms = chrono::duration<double, milli>(_now - lastClick_).count();
    lastClick_ = _now;
    speedClicks_ = diff_ms >= 0.0 && diff_ms <= 500.0 ? speedClicks_ + 1 : 1;
    leftMouseButtonPressed_ = true;

    Selector::Mode const selectionMode = [](unsigned _speedClicks, Modifier _modifier) {
        if (_speedClicks == 3)
            return Selector::Mode::FullLine;
        else if (_modifier.contains(Modifier::Control))
            return Selector::Mode::Rectangular;
        else if (_speedClicks == 2)
            return Selector::Mode::LinearWordWise;
        else
            return Selector::Mode::Linear;
    }(speedClicks_, _modifier);

    if (!selectionAvailable()
        || selector()->state() == Selector::State::Waiting
        || speedClicks_ >= 2)
    {
        setSelector(make_unique<Selector>(
            selectionMode,
            wordDelimiters_,
            screen_,
            absoluteCoordinate(currentMousePosition_)
        ));

        if (selectionMode != Selector::Mode::Linear)
            selector()->extend(absoluteCoordinate(currentMousePosition_));
    }
    else if (selector()->state() == Selector::State::Complete)
        clearSelection();

    breakLoopAndRefreshRenderBuffer();
    return true;
}

void Terminal::clearSelection()
{
    selector_.reset();
    breakLoopAndRefreshRenderBuffer();
}

bool Terminal::sendMouseMoveEvent(Coordinate newPosition, Modifier _modifier, Timestamp /*_now*/)
{
    speedClicks_ = 0;

    if (newPosition == currentMousePosition_)
        return false;

    currentMousePosition_ = newPosition;

    bool changed = updateCursorHoveringState();

    // Do not handle mouse-move events in sub-cell dimensions.
    if (respectMouseProtocol_ && inputGenerator_.generateMouseMove(currentMousePosition_, _modifier))
    {
        flushInput();
        return true;
    }

    if (leftMouseButtonPressed_ && !selectionAvailable())
    {
        changed = true;
        setSelector(make_unique<Selector>(
            Selector::Mode::Linear,
            wordDelimiters_,
            screen_,
            absoluteCoordinate(currentMousePosition_)
        ));
    }

    if (selectionAvailable() && selector()->state() != Selector::State::Complete)
    {
        changed = true;
        selector()->extend(absoluteCoordinate(newPosition));
        breakLoopAndRefreshRenderBuffer();
        return true;
    }

    // TODO: adjust selector's start lines according the the current viewport

    return changed;
}

bool Terminal::sendMouseReleaseEvent(MouseButton _button, Modifier _modifier, Timestamp /*_now*/)
{
    if (respectMouseProtocol_ && inputGenerator_.generateMouseRelease(_button, _modifier, currentMousePosition_))
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
                case Selector::State::Waiting:
                    clearSelection();
                    break;
                case Selector::State::InProgress:
                    selector()->stop();
                    eventListener_.onSelectionCompleted();
                    break;
                case Selector::State::Complete:
                    break;
            }
        }
    }

    return true;
}

bool Terminal::sendFocusInEvent()
{
    screen_.setFocus(true);
    breakLoopAndRefreshRenderBuffer();

    if (inputGenerator_.generateFocusInEvent())
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

    if (inputGenerator_.generateFocusOutEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

void Terminal::sendPaste(string_view _text)
{
    inputGenerator_.generatePaste(_text);
    flushInput();
}

void Terminal::sendRaw(string_view _text)
{
    inputGenerator_.generateRaw(_text);
    flushInput();
}

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);
    if (pendingInput_.empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    pty_.write(pendingInput_.data(), pendingInput_.size());
    pendingInput_.clear();
}

void Terminal::writeToScreen(string_view _data)
{
    auto const _l = lock_guard{*this};
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
    if (!screen_.contains(currentMousePosition_))
        return false;

    auto const relCursorPos = terminal::Coordinate{
        currentMousePosition_.row - viewport_.relativeScrollOffset().as<int>(),
        currentMousePosition_.column
    };

    auto const newState = screen_.contains(currentMousePosition_)
#if defined(LIBTERMINAL_HYPERLINKS)
                        && screen_.at(relCursorPos).hyperlink()
#endif
        ;
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
    auto const _l = lock_guard{*this};

    screen_.resize(_cells);
    if (_pixels)
    {
        auto width = Width(*_pixels->width / _cells.columns.as<unsigned>());
        auto height = Height(*_pixels->height / _cells.lines.as<unsigned>());
        screen_.setCellPixelSize(ImageSize{width, height});
    }

    pty_.resizeScreen(_cells, _pixels);
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
    int lastColumn = 0;
    string text;
    string currentLine;

    renderSelection([&](Coordinate const& _pos, Cell const& _cell) {
        auto const _lock = scoped_lock{ *this };
        auto const isNewLine = _pos.column <= lastColumn;
        auto const isLineWrapped = lineWrapped(_pos.row);
        bool const touchesRightPage = _pos.row > 0
            && isSelectedAbsolute({_pos.row - 1, screen_.size().columns.as<int>()});
        if (isNewLine && (!isLineWrapped || !touchesRightPage))
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
    using terminal::Coordinate;
    using terminal::Cell;

    auto const _l = std::lock_guard{*this};

    auto const colCount = *screen_.size().columns;
    auto const bottomLine = *screen_.historyLineCount() + screen_.cursor().position.row - 1;

    auto const marker1 = optional{bottomLine};

    auto const marker0 = screen_.findMarkerBackward(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 - screen_.historyLineCount().as<int>() + 2;
    auto const lastLine = *marker1 - screen_.historyLineCount().as<int>();

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        for (auto colNum = 1; colNum < colCount; ++colNum)
            text += screen_.at({lineNum, colNum}).toUtf8();
        trimSpaceRight(text);
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
    selector_.reset();
    viewport_.forceScrollToBottom();
    eventListener_.bufferChanged(_type);
}

void Terminal::scrollbackBufferCleared()
{
    selector_.reset();
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

void Terminal::dumpState()
{
    eventListener_.dumpState();
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
    inputGenerator_.setApplicationKeypadMode(_enabled);
}

void Terminal::setBracketedPaste(bool _enabled)
{
    inputGenerator_.setBracketedPaste(_enabled);
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
    inputGenerator_.setGenerateFocusEvents(_enabled);
}

void Terminal::setMouseProtocol(MouseProtocol _protocol, bool _enabled)
{
    inputGenerator_.setMouseProtocol(_protocol, _enabled);
}

void Terminal::setMouseTransport(MouseTransport _transport)
{
    inputGenerator_.setMouseTransport(_transport);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode _mode)
{
    inputGenerator_.setMouseWheelMode(_mode);
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
    inputGenerator_.setCursorKeysMode(keyMode);
}

void Terminal::hardReset()
{
    // NB: Screen was already reset.
    inputGenerator_.reset();
}

void Terminal::discardImage(Image const& _image)
{
    eventListener_.discardImage(_image);
}

void Terminal::markRegionDirty(LinePosition _line, ColumnPosition _column)
{
    if (!selector_)
        return;

    auto const y = screen_.toAbsoluteLine(*_line);
    auto const coord = Coordinate{y, *_column};
    if (selector_->contains(coord))
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
// }}}

}  // namespace terminal
