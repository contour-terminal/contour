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
#include <terminal/InputGenerator.h>
#include <terminal/Terminal.h>

#include <terminal/ControlCode.h>

#include <crispy/escape.h>
#include <crispy/stdfs.h>

#include <chrono>
#include <utility>

#include <iostream>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using std::move;

namespace terminal {

Terminal::Terminal(std::unique_ptr<Pty> _pty,
                   Terminal::Events& _eventListener,
                   optional<size_t> _maxHistoryLineCount,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   Logger _logger,
                   string const& _wordDelimiters,
                   Size _maxImageSize,
                   int _maxImageColorRegisters,
                   bool _sixelCursorConformance
) :
    changes_{ 0 },
    eventListener_{ _eventListener },
    logger_{ move(_logger) },
    pty_{ move(_pty) },
    cursorDisplay_{ CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_{ CursorShape::Block }, // TODO: pass via param
    cursorBlinkInterval_{ _cursorBlinkInterval },
    cursorBlinkState_{ 1 },
    lastCursorBlink_{ _now },
    startTime_{ _now },
    wordDelimiters_{ unicode::from_utf8(_wordDelimiters) },
    inputGenerator_{},
    screen_{
        pty_->screenSize(),
        *this,
        logger_,
        true, // logs raw output by default?
        true, // logs trace output by default?
        _maxHistoryLineCount,
        _maxImageSize,
        _maxImageColorRegisters,
        _sixelCursorConformance
    },
    screenUpdateThread_{ [this]() { screenUpdateThread(); } },
    viewport_{ screen_ }
{
}

Terminal::~Terminal()
{
    screenUpdateThread_.join();
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
            lock_guard<decltype(screenLock_)> _l{ screenLock_ };
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
    logger_(TraceInputEvent{ fmt::format("key: {}", to_string(_keyEvent.key), to_string(_keyEvent.modifier)) });

    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(Mode::KeyboardAction))
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
        logger_(TraceInputEvent{ fmt::format("char: {} ({})", static_cast<char>(_charEvent.value), to_string(_charEvent.modifier)) });
    else
        logger_(TraceInputEvent{ fmt::format("char: 0x{:04X} ({})", static_cast<uint32_t>(_charEvent.value), to_string(_charEvent.modifier)) });

    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(Mode::KeyboardAction))
        return true;

    bool const success = inputGenerator_.generate(_charEvent);
    flushInput();
    return success;
}

bool Terminal::send(MousePressEvent const& _mousePress, chrono::steady_clock::time_point _now)
{
    // TODO: anything else? logging?

    MousePressEvent const withPosition{_mousePress.button,
                                       _mousePress.modifier,
                                       currentMousePosition_.row,
                                       currentMousePosition_.column};
    if (inputGenerator_.generate(withPosition))
    {
        // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
        // under the current position
        flushInput();
        return true;
    }

    if (_mousePress.button == MouseButton::Left)
    {
        double const diff_ms = chrono::duration<double, milli>(_now - lastClick_).count();
        lastClick_ = _now;
        speedClicks_ = diff_ms >= 0.0 && diff_ms <= 500.0 ? speedClicks_ + 1 : 1;
        leftMouseButtonPressed_ = true;

        if (_mousePress.modifier == Modifier::None || _mousePress.modifier == Modifier::Control)
        {
            Selector::Mode const selectionMode = [](unsigned _speedClicks, Modifier _modifier) {
                if (_speedClicks == 3)
                    return Selector::Mode::FullLine;
                else if (_modifier == Modifier::Control)
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
    }
    return false;
}

void Terminal::clearSelection()
{
    selector_.reset();
    changes_++;
}


bool Terminal::send(MouseMoveEvent const& _mouseMove, chrono::steady_clock::time_point /*_now*/)
{
    auto const newPosition = _mouseMove.coordinates();

    currentMousePosition_ = newPosition;

    if (inputGenerator_.generate(_mouseMove))
    {
        flushInput();
        return true;
    }

    speedClicks_ = 0;

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
    if (inputGenerator_.generate(withPosition))
    {
        flushInput();
        return true;
    }

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
                    if (onSelectionComplete_)
                        onSelectionComplete_();
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

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);
    pty_->write(pendingInput_.data(), pendingInput_.size());
    logger_(RawInputEvent{crispy::escape(begin(pendingInput_), end(pendingInput_))});
    pendingInput_.clear();
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
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
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
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

// {{{ ScreenEvents overrides
optional<RGBColor> Terminal::requestDynamicColor(DynamicColorName _name)
{
    return eventListener_.requestDynamicColor(_name);
}

void Terminal::bell()
{
    eventListener_.bell();
}

void Terminal::bufferChanged(ScreenBuffer::Type _type)
{
    selector_.reset();
    viewport_.scrollToBottom();
    eventListener_.bufferChanged(_type);
}

void Terminal::scrollbackBufferCleared()
{
    selector_.reset();
    viewport_.scrollToBottom();
    changes_++;
}

void Terminal::commands()
{
    changes_++;

    // Screen output commands be here - anything this terminal is interested in?
    eventListener_.commands();
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

void Terminal::reply(string_view const& reply)
{
    pty_->write(reply.data(), reply.size());
}

void Terminal::resetDynamicColor(DynamicColorName _name)
{
    eventListener_.resetDynamicColor(_name);
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
    setCursorDisplay(_display);
    cursorShape_ = _shape;
}

void Terminal::setCursorVisibility(bool _visible)
{
    cursorVisibility_ = _visible;
}

void Terminal::setDynamicColor(DynamicColorName _name, RGBColor const& _color)
{
    eventListener_.setDynamicColor(_name, _color);
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

void Terminal::useApplicationCursorKeys(bool _enable)
{
    auto const keyMode = _enable ? KeyMode::Application : KeyMode::Normal;
    inputGenerator_.setCursorKeysMode(keyMode);
}

void Terminal::discardImage(Image const& _image)
{
    eventListener_.discardImage(_image);
}
// }}}

}  // namespace terminal
