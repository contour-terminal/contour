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

namespace {
    void trimSpaceRight(string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    };
}

Terminal::Terminal(std::unique_ptr<Pty> _pty,
                   Terminal::Events& _eventListener,
                   optional<size_t> _maxHistoryLineCount,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   string const& _wordDelimiters,
                   Modifier _mouseProtocolBypassModifier,
                   Size _maxImageSize,
                   int _maxImageColorRegisters,
                   bool _sixelCursorConformance,
                   ColorPalette _colorPalette
) :
    changes_{ 0 },
    eventListener_{ _eventListener },
    pty_{ move(_pty) },
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
        pty_->screenSize(),
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
    viewport_{ screen_ }
{
}

Terminal::~Terminal()
{
    if (screenUpdateThread_)
        screenUpdateThread_->join();
}

void Terminal::start()
{
    screenUpdateThread_ = make_unique<std::thread>(bind(&Terminal::screenUpdateThread, this));
}

void Terminal::screenUpdateThread()
{
    constexpr size_t BufSize = 32 * 1024;
    vector<char> buf;
    buf.resize(BufSize);

    for (;;)
    {
        if (auto const n = pty_->read(buf.data(), buf.size()); n != -1)
        {
            //log("outputThread.data: {}", crispy::escape(buf, buf + n));
            auto const _l = lock_guard{*this};
            screen_.write(buf.data(), n);
        }
        else
        {
            eventListener_.onClosed();
            break;
        }
    }
}

bool Terminal::send(KeyInputEvent const& _keyEvent, chrono::steady_clock::time_point _now)
{
    debuglog(KeyboardTag).write("key: {}; keyEvent: {}", to_string(_keyEvent.key), to_string(_keyEvent.modifier));

    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    bool const success = inputGenerator_.generate(_keyEvent);
    flushInput();
    return success;
}

bool Terminal::send(CharInputEvent const& _charEvent, chrono::steady_clock::time_point _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    if (_charEvent.value <= 0x7F && isprint(static_cast<int>(_charEvent.value)))
        debuglog(KeyboardTag).write("char: {} ({})", static_cast<char>(_charEvent.value), to_string(_charEvent.modifier));
    else
        debuglog(KeyboardTag).write("char: 0x{:04X} ({})", static_cast<uint32_t>(_charEvent.value), to_string(_charEvent.modifier));

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    bool const success = inputGenerator_.generate(_charEvent);
    flushInput();
    return success;
}

bool Terminal::send(MousePressEvent const& _mousePress, chrono::steady_clock::time_point _now)
{
    // TODO: anything else? logging?

    respectMouseProtocol_ = !(_mousePress.modifier.any() && _mousePress.modifier.contains(mouseProtocolBypassModifier_));

    MousePressEvent const withPosition{_mousePress.button,
                                       _mousePress.modifier,
                                       currentMousePosition_.row,
                                       currentMousePosition_.column};
    if (respectMouseProtocol_ && inputGenerator_.generate(withPosition))
    {
        // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
        // under the current position
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

    changes_++;
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

    return true;
}

void Terminal::clearSelection()
{
    selector_.reset();
    changes_++;
}


bool Terminal::send(MouseMoveEvent const& _mouseMove, chrono::steady_clock::time_point /*_now*/)
{
    auto const newPosition = _mouseMove.coordinates();
    bool const positionChanged = newPosition != currentMousePosition_;

    currentMousePosition_ = newPosition;

    // Do not handle mouse-move events in sub-cell dimensions.
    if (respectMouseProtocol_ && inputGenerator_.generate(_mouseMove))
    {
        flushInput();
        return true;
    }

    speedClicks_ = 0;

    if (!positionChanged)
        return true;

    if (leftMouseButtonPressed_ && !selectionAvailable())
    {
        setSelector(make_unique<Selector>(
            Selector::Mode::Linear,
            wordDelimiters_,
            screen_,
            absoluteCoordinate(currentMousePosition_)
        ));
    }

    if (selectionAvailable() && selector()->state() != Selector::State::Complete)
    {
        selector()->extend(absoluteCoordinate(newPosition));
        changes_++;
        return true;
    }

    // TODO: adjust selector's start lines according the the current viewport

    return false;
}

bool Terminal::send(MouseReleaseEvent const& _mouseRelease, chrono::steady_clock::time_point /*_now*/)
{
    MouseReleaseEvent const withPosition{_mouseRelease.button,
                                         _mouseRelease.modifier,
                                         currentMousePosition_.row,
                                         currentMousePosition_.column};
    if (respectMouseProtocol_ && inputGenerator_.generate(withPosition))
    {
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
                    eventListener_.onSelectionComplete();
                    break;
                case Selector::State::Complete:
                    break;
            }
        }
    }

    return true;
}

bool Terminal::send(FocusInEvent const& _focusEvent,
                    [[maybe_unused]] std::chrono::steady_clock::time_point _now)
{
    if (inputGenerator_.generate(_focusEvent))
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::send(FocusOutEvent const& _focusEvent,
                    [[maybe_unused]] std::chrono::steady_clock::time_point _now)
{
    if (inputGenerator_.generate(_focusEvent))
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::send(MouseEvent const& _inputEvent, std::chrono::steady_clock::time_point _now)
{
    return visit(overloaded{
        [=](MousePressEvent const& ev) -> bool { return send(InputEvent{ev}, _now); },
        [=](MouseReleaseEvent const& ev) -> bool { return send(InputEvent{ev}, _now); },
        [=](MouseMoveEvent const& ev) -> bool { return send(InputEvent{ev}, _now); }
    }, _inputEvent);
}

bool Terminal::send(InputEvent const& _inputEvent, chrono::steady_clock::time_point _now)
{
    return visit(overloaded{
        [=](KeyInputEvent const& _key) -> bool { return send(_key, _now); },
        [=](CharInputEvent const& _char) -> bool { return send(_char, _now); },
        [=](MousePressEvent const& _mouse) -> bool { return send(_mouse, _now); },
        [=](MouseMoveEvent const& _mouseMove) -> bool { return send(_mouseMove, _now); },
        [=](MouseReleaseEvent const& _mouseRelease) -> bool { return send(_mouseRelease, _now); },
        [=](FocusInEvent const& _event) -> bool { return send(_event, _now); },
        [=](FocusOutEvent const& _event) -> bool { return send(_event, _now); },
    }, _inputEvent);
}

void Terminal::sendPaste(string_view const& _text)
{
    inputGenerator_.generatePaste(_text);
    flushInput();
}

void Terminal::sendRaw(std::string_view const& _text)
{
    inputGenerator_.generate(_text);
    flushInput();
}

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);
    if (!pendingInput_.empty())
    {
        // XXX should be the only location that does write to the PTY's stdin to avoid race conditions.
        pty_->write(pendingInput_.data(), pendingInput_.size());
        debuglog(KeyboardTag).write(crispy::escape(begin(pendingInput_), end(pendingInput_)));
        pendingInput_.clear();
    }
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    auto const _l = lock_guard{*this};
    screen_.write(data, size);
}

bool Terminal::shouldRender(chrono::steady_clock::time_point const& _now) const
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

    pty_->resizeScreen(_cells, _pixels);
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
    changes_++;
}

void Terminal::screenUpdated()
{
    changes_++;

    // Screen output commands be here - anything this terminal is interested in?
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

void Terminal::copyToClipboard(std::string_view const& _data)
{
    eventListener_.copyToClipboard(_data);
}

void Terminal::dumpState()
{
    eventListener_.dumpState();
}

void Terminal::notify(std::string_view const& _title, std::string_view const& _body)
{
    eventListener_.notify(_title, _body);
}

void Terminal::reply(string_view const& _reply)
{
    eventListener_.reply(_reply);
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

void Terminal::setWindowTitle(std::string_view const& _title)
{
    eventListener_.setWindowTitle(_title);
}

void Terminal::setTerminalProfile(std::string const& _configProfileName)
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
