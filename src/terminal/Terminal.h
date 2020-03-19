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
#pragma once

#include <terminal/Commands.h>
#include <terminal/Logger.h>
#include <terminal/InputGenerator.h>
#include <terminal/PseudoTerminal.h>
#include <terminal/Screen.h>
#include <terminal/Selector.h>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace terminal {

/// Terminal API to manage keyboard and screen of a pseudo terminal.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
class Terminal {
  public:
    using Hook = std::function<void(std::vector<Command> const& commands)>;

    explicit Terminal(
        WindowSize _winSize,
        std::optional<size_t> _maxHistoryLineCount = std::nullopt,
        std::chrono::milliseconds _cursorBlinkInterval = std::chrono::milliseconds{500},
        std::function<void()> _changeWindowTitleCallback = {},
        std::function<void(unsigned int, unsigned int, bool)> _resizeWindow = {},
        std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now(),
        Logger _logger = {},
        Hook _onScreenCommands = {},
        std::function<void()> _onClosed = {},
        std::string const& _wordDelimiters = "",
        std::function<RGBColor(DynamicColorName)> _requestDynamicColor = {},
        std::function<void(DynamicColorName)> _resetDynamicColor = {},
        std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor = {}
    );
    ~Terminal();

    /// Retrieves reference to the underlying PTY device.
    PseudoTerminal& device() noexcept { return pty_; }

    /// Retrieves the time point this terminal instance has been spawned.
    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);
    size_t historyLineCount() const noexcept;

    // Sends given input event to connected slave.
    bool send(KeyInputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(CharInputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MousePressEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MouseReleaseEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MouseMoveEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);

    bool send(MouseEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(InputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);

    /// Sends verbatim text in bracketed mode to application.
    void sendPaste(std::string_view const& _text);

    /// Writes a given VT-sequence to screen.
    void writeToScreen(char const* data, size_t size);
    void writeToScreen(std::string_view const& _text) { writeToScreen(_text.data(), _text.size()); }
    void writeToScreen(std::string const& _text) { writeToScreen(_text.data(), _text.size()); }

    /// Checks if a render() method should be called by checking the dirty bit,
    /// and if so, clears the dirty bit and returns true, false otherwise.
    bool shouldRender(std::chrono::steady_clock::time_point const& _now) const;

    /// Thread-safe access to screen data for rendering.
    template <typename... RenderPasses>
    uint64_t render(std::chrono::steady_clock::time_point _now, Screen::Renderer const& pass, RenderPasses... passes) const
    {
        auto _l = std::lock_guard{screenLock_};
        auto const changes = changes_.exchange(0);
        updateCursorVisibilityState(_now);
        renderPass(pass, std::forward<RenderPasses>(passes)...);
        return changes;
    }

    std::chrono::milliseconds nextRender(std::chrono::steady_clock::time_point _now) const;

    Screen::Cell const& absoluteAt(Coordinate const& _coord) const;

    /// @returns absolute coordinate of given _viewportCoordinate and _scrollOffset.
    Coordinate absoluteCoordinate(Coordinate _viewportCoordinate, size_t _scrollOffset) const noexcept;

    /// @returns absolute coordinate of given _viewportCoordinate at the current viewport scroll offset.
    Coordinate absoluteCoordinate(Coordinate _viewportCoordinate) const noexcept
    {
        return absoluteCoordinate(_viewportCoordinate, scrollOffset_);
    }

    using Cursor = Screen::Cursor; //TODO: CursorShape shape;

    /// @returns the current Cursor state.
    Cursor cursor() const;

    std::string const& windowTitle() const noexcept { return screen_.windowTitle(); }

    /// @returns a screenshot, that is, a VT-sequence reproducing the current screen buffer.
    std::string screenshot() const;

    void resizeScreen(WindowSize const& _newWindowSize);

    void setTabWidth(unsigned int _tabWidth);

    /// Tests whether given absolute line number [1..num] is within scrolling region
    bool isAbsoluteLineVisible(cursor_pos_t _row) const noexcept;

    size_t scrollOffset() const noexcept { return scrollOffset_; }

    bool scrollUp(size_t _numLines);
    bool scrollDown(size_t _numLines);
    bool scrollToTop();
    bool scrollToBottom();

    // -----------------------------------------------------------------------------------

    void setCursorDisplay(CursorDisplay _value);
    void setCursorShape(CursorShape _value);
    CursorShape cursorShape() const noexcept { return cursorShape_; }

    bool shouldDisplayCursor() const noexcept
    {
        return cursor().visible && (cursorDisplay_ != CursorDisplay::Blink || cursorBlinkState_);
    }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept
    {
        return lastCursorBlink_;
    }

    // -----------------------------------------------------------------------------------

    void setWordDelimiters(std::string const& _wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return wordDelimiters_; }

    // -----------------------------------------------------------------------------------

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept { return selector_ && selector_->state() != Selector::State::Waiting; }

    /// Returns list of ranges that have been selected.
    std::vector<Selector::Range> selection() const;

    /// Clears current selection, if any currently present.
    void clearSelection();

    /// Renders only the selected area.
    void renderSelection(terminal::Screen::Renderer const& _render) const;

    constexpr std::chrono::milliseconds cursorBlinkInterval() const noexcept
    {
        return cursorBlinkInterval_;
    }

    WindowSize screenSize() const noexcept { return pty_.screenSize(); }

  private:
    void flushInput();
    void screenUpdateThread();
    void useApplicationCursorKeys(bool _enable);
    void onScreenReply(std::string_view const& reply);
    void onScreenCommands(std::vector<Command> const& commands);
    void onSetCursorStyle(CursorDisplay _display, CursorShape _shape);
    void updateCursorVisibilityState(std::chrono::steady_clock::time_point _now) const;

    template <typename... RemainingPasses>
    void renderPass(Screen::Renderer const& pass, RemainingPasses... remainingPasses) const
    {
        screen_.render(pass, scrollOffset_);

        if constexpr (sizeof...(RemainingPasses) != 0)
            renderPass(std::forward<RemainingPasses>(remainingPasses)...);
    }

  private:
    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> changes_;

    Logger logger_;
    PseudoTerminal pty_;

    CursorDisplay cursorDisplay_;
    CursorShape cursorShape_;
    std::chrono::milliseconds cursorBlinkInterval_;
	mutable unsigned cursorBlinkState_;
	mutable std::chrono::steady_clock::time_point lastCursorBlink_;

    std::chrono::steady_clock::time_point startTime_;

    std::u32string wordDelimiters_;
    std::unique_ptr<Selector> selector_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_{};
    unsigned int speedClicks_ = 0;

    size_t scrollOffset_ = 0;
    terminal::Coordinate currentMousePosition_{0, 0}; // current mouse position

    InputGenerator inputGenerator_;
    InputGenerator::Sequence pendingInput_;
    Screen screen_;
    Screen::Hook onScreenCommands_;
    std::recursive_mutex mutable screenLock_;
    std::thread screenUpdateThread_;

    std::function<void()> onClosed_;
};

}  // namespace terminal
