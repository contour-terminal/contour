/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include "terminal/InputGenerator.h"
#include <terminal/Terminal.h>

#include <terminal/OutputGenerator.h>
#include <terminal/ControlCode.h>
#include <terminal/Util.h>

#include <chrono>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

namespace terminal {

Terminal::Terminal(WindowSize _winSize,
                   optional<size_t> _maxHistoryLineCount,
                   chrono::milliseconds _cursorBlinkInterval,
                   function<void()> _onWindowTitleChanged,
                   function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                   chrono::steady_clock::time_point _now,
                   Logger _logger,
                   Hook _onScreenCommands,
                   function<void()> _onClosed,
                   string const& _wordDelimiters,
                   function<void()> _onSelectionComplete,
                   Screen::OnBufferChanged _onScreenBufferChanged,
                   std::function<void()> _bell,
                   std::function<RGBColor(DynamicColorName)> _requestDynamicColor,
                   std::function<void(DynamicColorName)> _resetDynamicColor,
                   std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor
) :
    changes_{ 0 },
    logger_{ _logger },
    pty_{ _winSize },
    cursorDisplay_{ CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_{ CursorShape::Block }, // TODO: pass via param
    cursorBlinkInterval_{ _cursorBlinkInterval },
    cursorBlinkState_{ 1 },
    lastCursorBlink_{ _now },
    startTime_{ _now },
    wordDelimiters_{ utf8::decode(_wordDelimiters) },
    selector_{},
    onSelectionComplete_{ move(_onSelectionComplete) },
    inputGenerator_{},
    screen_{
        _winSize,
        _maxHistoryLineCount,
        bind(&Terminal::useApplicationCursorKeys, this, _1),
        move(_onWindowTitleChanged),
        move(_resizeWindow),
        bind(&InputGenerator::setApplicationKeypadMode, &inputGenerator_, _1),
        bind(&InputGenerator::setBracketedPaste, &inputGenerator_, _1),
        bind(&InputGenerator::setMouseProtocol, &inputGenerator_, _1, _2),
        bind(&InputGenerator::setMouseTransport, &inputGenerator_, _1),
        bind(&InputGenerator::setMouseWheelMode, &inputGenerator_, _1),
        bind(&Terminal::onSetCursorStyle, this, _1, _2),
        bind(&Terminal::onScreenReply, this, _1),
        logger_,
        true, // logs raw output by default?
        true, // logs trace output by default?
        bind(&Terminal::onScreenCommands, this, _1),
        move(_onScreenBufferChanged),
        move(_bell),
        move(_requestDynamicColor),
        move(_resetDynamicColor),
        move(_setDynamicColor),
        bind(&InputGenerator::setGenerateFocusEvents, &inputGenerator_, _1)
    },
    onScreenCommands_{ move(_onScreenCommands) },
    screenUpdateThread_{ bind(&Terminal::screenUpdateThread, this) },
    onClosed_{ move(_onClosed) }
{
}

Terminal::~Terminal()
{
    screenUpdateThread_.join();
}

void Terminal::useApplicationCursorKeys(bool _enable)
{
    auto const keyMode = _enable ? KeyMode::Application : KeyMode::Normal;
    inputGenerator_.setCursorKeysMode(keyMode);
}

void Terminal::onScreenReply(string_view const& reply)
{
    pty_.write(reply.data(), reply.size());
}

void Terminal::onScreenCommands(vector<Command> const& _commands)
{
    changes_++;

    // Screen output commands be here - anything this terminal is interested in?
    if (onScreenCommands_)
        onScreenCommands_(_commands);
}

void Terminal::screenUpdateThread()
{
    constexpr size_t BufSize = 32 * 1024;
    vector<char> buf;
    buf.resize(BufSize);

    for (;;)
    {
        if (auto const n = pty_.read(buf.data(), buf.size()); n != -1)
        {
            //log("outputThread.data: {}", terminal::escape(buf, buf + n));
            lock_guard<decltype(screenLock_)> _l{ screenLock_ };
            screen_.write(buf.data(), n);
        }
        else
        {
            if (onClosed_)
                onClosed_();
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

    if (utf8::isASCII(_charEvent.value) && isprint(_charEvent.value))
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
            Selector::Mode const selectionMode = [](int _speedClicks, Modifier _modifier) {
                if (_speedClicks == 3)
                    return Selector::Mode::FullLine;
                else if (_modifier == Modifier::Control)
                    return Selector::Mode::Rectangular;
                else if (_speedClicks == 2)
                    return Selector::Mode::LinearWordWise;
                else
                    return Selector::Mode::Linear;
            }(speedClicks_, _mousePress.modifier);

            if (speedClicks_ >= 2)
            {
                selector_ = make_unique<Selector>(
                    selectionMode,
                    bind(&Terminal::absoluteAt, this, _1),
                    wordDelimiters(),
                    screenSize().rows + static_cast<cursor_pos_t>(historyLineCount()),
                    screenSize(),
                    absoluteCoordinate(currentMousePosition_)
                );
            }
            else if (selector_)
                clearSelection();

            changes_++;

            return true;
        }
    }
    return false;
}

bool Terminal::send(MouseMoveEvent const& _mouseMove, chrono::steady_clock::time_point /*_now*/)
{
    auto const newPosition = terminal::Coordinate{_mouseMove.row, _mouseMove.column};

    currentMousePosition_ = newPosition;

    if (inputGenerator_.generate(_mouseMove))
    {
        flushInput();
        return true;
    }

    if (leftMouseButtonPressed_ && !selector_)
    {
        selector_ = make_unique<Selector>(
            Selector::Mode::Linear,
            bind(&Terminal::absoluteAt, this, _1),
            wordDelimiters(),
            screenSize().rows + static_cast<cursor_pos_t>(historyLineCount()),
            screenSize(),
            absoluteCoordinate(currentMousePosition_)
        );
    }

    if (selector_ && selector_->state() != Selector::State::Complete)
    {
        selector_->extend(absoluteCoordinate(newPosition));
        changes_++;
    }

    speedClicks_ = 0;

    return true;
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
        if (selector_ && selector_->state() == Selector::State::InProgress)
        {
            selector_->stop();

            if (onSelectionComplete_)
                onSelectionComplete_();
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
    pty_.write(pendingInput_.data(), pendingInput_.size());
    logger_(RawInputEvent{escape(begin(pendingInput_), end(pendingInput_))});
    pendingInput_.clear();
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    screen_.write(data, size);
}

Terminal::Cursor Terminal::cursor() const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    return screen_.realCursor();
}

string Terminal::screenshot() const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    return screen_.screenshot();
}

bool Terminal::shouldRender(chrono::steady_clock::time_point const& _now) const
{
    return changes_.load() || (
        cursorDisplay_ == CursorDisplay::Blink &&
        chrono::duration_cast<chrono::milliseconds>(_now - lastCursorBlink_) >= cursorBlinkInterval());
}

// void Terminal::render(steady_clock::time_point _now, Screen::Renderer const& pass1) const
// {
//     lock_guard<decltype(screenLock_)> _l{ screenLock_ };
//     changes_.store(0);
//     updateCursorVisibilityState(_now);
//     screen_.render(pass1, screen_.scrollOffset());
// }
//
// void Terminal::render(steady_clock::time_point _now, Screen::Renderer const& pass1, Screen::Renderer const& pass2) const
// {
//     lock_guard<decltype(screenLock_)> _l{ screenLock_ };
//     changes_.store(0);
//     updateCursorVisibilityState(_now);
//     screen_.render(pass1, screen_.scrollOffset());
//     screen_.render(pass2, screen_.scrollOffset());
// }

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

Screen::Cell const& Terminal::absoluteAt(Coordinate const& _coord) const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    return screen_.absoluteAt(_coord);
}

void Terminal::resizeScreen(WindowSize const& _newWindowSize)
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    screen_.resize(_newWindowSize);
    pty_.resizeScreen(_newWindowSize);
}

Coordinate Terminal::absoluteCoordinate(Coordinate _viewportCoordinate, size_t _scrollOffset) const noexcept
{
    // TODO: unit test case me BEFORE merge, yo !
    auto const row = screen_.historyLineCount() - min(_scrollOffset, screen_.historyLineCount()) + _viewportCoordinate.row;
    auto const col = _viewportCoordinate.column;
    return {static_cast<cursor_pos_t>(row), col};
}

void Terminal::onSetCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    setCursorDisplay(_display);
    cursorShape_ = _shape;
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
    wordDelimiters_ = utf8::decode(_wordDelimiters);
}

vector<Selector::Range> Terminal::selection() const
{
    if (selector_)
        return selector_->selection();
    else
        return {};
}

void Terminal::renderSelection(terminal::Screen::Renderer const& _render) const
{
    if (selector_)
        selector_->render(_render);
}

void Terminal::clearSelection()
{
    selector_.reset();
    changes_++;
}

}  // namespace terminal
