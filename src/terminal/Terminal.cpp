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
#include <crispy/debuglog.h>

#include <chrono>
#include <utility>

#include <iostream>

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
    };

    tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette, Cell const& _cell, bool _reverseVideo, bool _selected)
    {
        auto const [fg, bg] = _cell.attributes().makeColors(_colorPalette, _reverseVideo);
        if (!_selected)
            return tuple{fg, bg};

        auto const a = _colorPalette.selectionForeground.value_or(bg);
        auto const b = _colorPalette.selectionBackground.value_or(fg);
        return tuple{a, b};
    }
}
// }}}

Terminal::Terminal(Pty& _pty,
                   int _ptyReadBufferSize,
                   Terminal::Events& _eventListener,
                   optional<size_t> _maxHistoryLineCount,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   string const& _wordDelimiters,
                   Modifier _mouseProtocolBypassModifier,
                   Size _maxImageSize,
                   int _maxImageColorRegisters,
                   bool _sixelCursorConformance,
                   ColorPalette _colorPalette,
                   double _refreshRate
) :
    changes_{ 0 },
    ptyReadBufferSize_{ _ptyReadBufferSize },
    eventListener_{ _eventListener },
    refreshInterval_{ static_cast<long long>(1000.0 / _refreshRate) },
    renderBuffer_{},
    pty_{ _pty },
    cursorDisplay_{ CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_{ CursorShape::Block }, // TODO: pass via param
    cursorBlinkInterval_{ _cursorBlinkInterval },
    cursorBlinkState_{ 1 },
    lastCursorBlink_{ _now },
    startTime_{ _now },
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
        _colorPalette
    },
    screenUpdateThread_{},
    viewport_{ screen_, [this]() { breakLoopAndRefreshRenderBuffer(); } }
{
    readBuffer_.resize(ptyReadBufferSize_);
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

    debuglog(TerminalTag).write(
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

    eventListener_.onClosed();
}

bool Terminal::processInputOnce()
{
    auto const timeout =
        renderBuffer_.state == RenderBufferState::WaitingForRefresh && !screenDirty_
            ? std::chrono::seconds(4)
            : refreshInterval_ // std::chrono::seconds(0)
            ;

    auto const n = pty_.read(readBuffer_.data(), readBuffer_.size(), timeout);

    if (n > 0)
    {
        writeToScreen(readBuffer_.data(), n);

        #if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
        auto const now = std::chrono::steady_clock::now();
        ensureFreshRenderBuffer(now);
        #endif
    }
    else if (n == 0)
    {
        debuglog(TerminalTag).write("PTY read returned with zero bytes.");
    }
    else if (n < 0 && (errno != EINTR && errno != EAGAIN))
    {
        debuglog(TerminalTag).write("PTY read failed. {}", strerror(errno));
        return false;
    }

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

bool Terminal::refreshRenderBuffer(std::chrono::steady_clock::time_point _now)
{
    renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
    ensureFreshRenderBuffer(_now);
    return renderBuffer_.state == RenderBufferState::WaitingForRefresh;
}

void Terminal::ensureFreshRenderBuffer(std::chrono::steady_clock::time_point _now)
{
    if (!renderBufferUpdateEnabled_)
    {
        renderBuffer_.state = RenderBufferState::WaitingForRefresh;
        return;
    }

    auto const elapsed = _now - renderBuffer_.lastUpdate;
    auto const avoidRefresh = elapsed < refreshInterval_;

    switch (renderBuffer_.state)
    {
        case RenderBufferState::WaitingForRefresh:
            if (avoidRefresh || !screenDirty_)
                break;
            renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
            [[fallthrough]];
        case RenderBufferState::RefreshBuffersAndTrySwap:
            refreshRenderBuffer(renderBuffer_.backBuffer());
            renderBuffer_.state = RenderBufferState::TrySwapBuffers;
            [[fallthrough]];
        case RenderBufferState::TrySwapBuffers:
            #if !defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
                // We have been actively invoked by the render thread, so don't inform it about updates.
                renderBuffer_.swapBuffers(_now);
            #else
                // Passively invoked by the terminal thread, so do inform render thread about updates.
                if (renderBuffer_.swapBuffers(_now))
                    eventListener_.renderBufferUpdated();
            #endif
            break;
    }
}

void Terminal::refreshRenderBuffer(RenderBuffer& _output)
{
    auto const _l = lock_guard{*this};
    auto const reverseVideo = screen_.isModeEnabled(terminal::DECMode::ReverseVideo);
    auto const baseLine = viewport_.absoluteScrollOffset().value_or(screen_.historyLineCount());
    auto const renderHyperlinks = screen_.contains(currentMousePosition_);
    auto const currentMousePositionRel = Coordinate{
        currentMousePosition_.row - viewport_.relativeScrollOffset(),
        currentMousePosition_.column
    };

    changes_.store(0);

    if (renderHyperlinks)
    {
        auto& cellAtMouse = screen_.at(currentMousePositionRel);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
    }

    // {{{ void appendCell(pos, cell, fg, bg)
    auto const appendCell = [&](Coordinate const& _pos, Cell const& _cell,
                                RGBColor fg, RGBColor bg)
    {
        RenderCell cell;
        cell.backgroundColor = bg;
        cell.foregroundColor = fg;
        cell.decorationColor = _cell.attributes().getUnderlineColor(screen_.colorPalette());
        cell.position = _pos;
        cell.flags = _cell.attributes().styles;

        if (!_cell.codepoints().empty())
        {
#if defined(LIBTERMINAL_IMAGES)
            assert(!_cell.imageFragment().has_value());
#endif
            cell.codepoints = _cell.codepoints();
        }
#if defined(LIBTERMINAL_IMAGES)
        else if (optional<ImageFragment> const& fragment = _cell.imageFragment(); fragment.has_value())
        {
            assert(_cell.codepoints().empty());
            cell.flags |= CellFlags::Image; // TODO: this should already be there.
            cell.image = _cell.imageFragment();
        }
#endif

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

        _output.screen.emplace_back(std::move(cell));
    }; // }}}

    screenDirty_ = false;
    _output.clear();

    enum class State {
        Gap,
        Sequence,
    };
    State state = State::Gap;

    int lineNr = 1;
    screen_.render(
        [&](Coordinate const& _pos, Cell const& _cell) // mutable
        {
            auto const absolutePos = Coordinate{baseLine + (_pos.row - 1), _pos.column};
            auto const selected = isSelectedAbsolute(absolutePos);
            auto const [fg, bg] = makeColors(screen_.colorPalette(), _cell, reverseVideo, selected);

            auto const cellEmpty = (_cell.codepoints().empty() || _cell.codepoints()[0] == 0x20)
#if defined(LIBTERMINAL_IMAGES)
                                && !_cell.imageFragment().has_value()
#endif
                                ;
            auto const customBackground = bg != screen_.colorPalette().defaultBackground;

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

    if (renderHyperlinks)
    {
        auto& cellAtMouse = screen_.at(currentMousePositionRel);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
    }

    _output.cursor = renderCursor();
}

optional<RenderCursor> Terminal::renderCursor()
{
    bool const shouldDisplayCursor = screen_.cursor().visible
        && (cursorDisplay() == CursorDisplay::Steady || cursorBlinkActive());

    if (!shouldDisplayCursor || !viewport().isLineVisible(screen_.cursor().position.row))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    Cell const& cursorCell = screen_.at(screen_.cursor().position);

    auto const shape = screen_.focused() ? cursorShape()
                                         : CursorShape::Rectangle;

    return RenderCursor{
        Coordinate(
            screen_.cursor().position.row + viewport_.relativeScrollOffset(),
            screen_.cursor().position.column
        ),
        shape,
        cursorCell.width()
    };
}
// }}}

bool Terminal::sendKeyPressEvent(KeyInputEvent const& _keyEvent, chrono::steady_clock::time_point _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    viewport_.scrollToBottom();
    bool const success = inputGenerator_.generate(_keyEvent.key, _keyEvent.modifier);
    if (success)
        debuglog(InputTag).write("Sending {}.", _keyEvent);

    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendCharPressEvent(CharInputEvent const& _charEvent, steady_clock::time_point _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    auto const success = inputGenerator_.generate(_charEvent.value, _charEvent.modifier);
    if (success)
        debuglog(InputTag).write("Sending {}.", _charEvent);

    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendMousePressEvent(MousePressEvent const& _mousePress, chrono::steady_clock::time_point _now)
{
    respectMouseProtocol_ = mouseProtocolBypassModifier_ == Modifier::None
                         || !_mousePress.modifier.contains(mouseProtocolBypassModifier_);

    MousePressEvent const withPosition{_mousePress.button,
                                       _mousePress.modifier,
                                       currentMousePosition_.row,
                                       currentMousePosition_.column};
    if (respectMouseProtocol_ && inputGenerator_.generate(withPosition))
    {
        // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
        // under the current position
        debuglog(InputTag).write("Sending {}.", withPosition);
        flushInput();
        return true;
    }

    if (_mousePress.button != MouseButton::Left)
        return false;

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
    }(speedClicks_, _mousePress.modifier);

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

bool Terminal::sendMouseMoveEvent(MouseMoveEvent const& _mouseMove, chrono::steady_clock::time_point /*_now*/)
{
    auto const newPosition = _mouseMove.coordinates();
    bool const positionChanged = newPosition != currentMousePosition_;

    currentMousePosition_ = newPosition;

    bool changed = updateCursorHoveringState();

    // Do not handle mouse-move events in sub-cell dimensions.
    if (respectMouseProtocol_ && inputGenerator_.generate(_mouseMove))
    {
        debuglog(InputTag).write("Sending {}.", _mouseMove);
        flushInput();
        return true;
    }

    speedClicks_ = 0;

    if (!positionChanged)
        return true;

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

bool Terminal::sendMouseReleaseEvent(MouseReleaseEvent const& _mouseRelease, chrono::steady_clock::time_point /*_now*/)
{
    MouseReleaseEvent const withPosition{_mouseRelease.button,
                                         _mouseRelease.modifier,
                                         currentMousePosition_.row,
                                         currentMousePosition_.column};

    if (respectMouseProtocol_ && inputGenerator_.generate(withPosition))
    {
        debuglog(InputTag).write("Sending {}.", withPosition);
        flushInput();
        return true;
    }
    respectMouseProtocol_ = true;

    if (_mouseRelease.button == MouseButton::Left)
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

    if (inputGenerator_.generate(FocusInEvent{}))
    {
        debuglog(InputTag).write("Sending {}.", FocusInEvent{});
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::sendFocusOutEvent()
{
    screen_.setFocus(false);
    breakLoopAndRefreshRenderBuffer();

    if (inputGenerator_.generate(FocusOutEvent{}))
    {
        debuglog(InputTag).write("Sending {}.", FocusOutEvent{});
        flushInput();
        return true;
    }

    return false;
}

void Terminal::sendPaste(string_view _text)
{
    debuglog(InputTag).write("Sending paste of {} bytes.", _text.size());
    inputGenerator_.generatePaste(_text);
    flushInput();
}

void Terminal::sendRaw(string_view _text)
{
    inputGenerator_.generate(_text);
    flushInput();
}

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);
    if (pendingInput_.empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    debuglog(InputTag).write("Flushing input: \"{}\"", crispy::escape(begin(pendingInput_), end(pendingInput_)));
    pty_.write(pendingInput_.data(), pendingInput_.size());
    pendingInput_.clear();
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    auto const _l = lock_guard{*this};
    screen_.write(data, size);
    renderBufferUpdateEnabled_ = !screen_.isModeEnabled(DECMode::BatchedRendering);
}

// TODO: this family of functions seems we don't need anymore
bool Terminal::shouldRender(steady_clock::time_point const& _now) const
{
    return changes_.load() || (
        cursorDisplay_ == CursorDisplay::Blink &&
        chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_) >= cursorBlinkInterval());
}

void Terminal::updateCursorVisibilityState(std::chrono::steady_clock::time_point _now) const
{
    auto const diff = chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_);
    if (diff >= cursorBlinkInterval())
    {
        lastCursorBlink_ = _now;
        cursorBlinkState_ = (cursorBlinkState_ + 1) % 2;
    }
}

bool Terminal::updateCursorHoveringState()
{
    if (!screen_.contains(currentMousePosition_))
        return false;

    auto const relCursorPos = terminal::Coordinate{
        currentMousePosition_.row - viewport_.relativeScrollOffset(),
        currentMousePosition_.column
    };

    auto const newState = screen_.contains(currentMousePosition_) && screen_.at(relCursorPos).hyperlink();
    auto const oldState = hoveringHyperlink_.exchange(newState);
    return newState != oldState;
}

std::chrono::milliseconds Terminal::nextRender(chrono::steady_clock::time_point _now) const
{
    auto const diff = chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_);
    if (diff <= cursorBlinkInterval())
        return {diff};
    else
        return chrono::milliseconds::min();
}

void Terminal::resizeScreen(Size _cells, optional<Size> _pixels)
{
    auto const _l = lock_guard{*this};

    screen_.resize(_cells);
    if (_pixels)
        screen_.setCellPixelSize(*_pixels / _cells);

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
            && isSelectedAbsolute({_pos.row - 1, screen_.size().width});
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

    auto const colCount = screen_.size().width;
    auto const bottomLine = screen_.historyLineCount() + screen_.cursor().position.row - 1;

    auto const marker1 = optional{bottomLine};

    auto const marker0 = screen_.findMarkerBackward(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 - screen_.historyLineCount() + 2;
    auto const lastLine = *marker1 - screen_.historyLineCount();

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
    screenDirty_ = true;
    //pty_.wakeupReader();
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

void Terminal::resizeWindow(int _width, int _height, bool _unitInPixels)
{
    eventListener_.resizeWindow(_width, _height, _unitInPixels);
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

void Terminal::setCursorVisibility(bool _visible)
{
    cursorVisibility_ = _visible;
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
// }}}

}  // namespace terminal
