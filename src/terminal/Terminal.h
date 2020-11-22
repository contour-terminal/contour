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

#include <terminal/Logger.h>
#include <terminal/InputGenerator.h>
#include <terminal/pty/Pty.h>
#include <terminal/ScreenEvents.h>
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

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
class Terminal : public ScreenEvents {
  public:
    class Events {
      public:
        virtual ~Events() = default;

        virtual std::optional<RGBColor> requestDynamicColor(DynamicColorName /*_name*/) { return std::nullopt; }
        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void commands() {}
        virtual void copyToClipboard(std::string_view const& /*_data*/) {}
        virtual void dumpState() {}
        virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
        virtual void onClosed() {}
        virtual void onSelectionComplete() {}
        virtual void resetDynamicColor(DynamicColorName /*_name*/) {}
        virtual void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) {}
        virtual void setDynamicColor(DynamicColorName, RGBColor const&) {}
        virtual void setWindowTitle(std::string_view const& /*_title*/) {}
        virtual void discardImage(Image const&) {}
    };

    Terminal(std::unique_ptr<Pty> _pty,
             Events& _eventListener,
             std::optional<size_t> _maxHistoryLineCount = std::nullopt,
             std::chrono::milliseconds _cursorBlinkInterval = std::chrono::milliseconds{500},
             std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now(),
             Logger _logger = {},
             std::string const& _wordDelimiters = "",
             Size _maxImageSize = Size{800, 600},
             int _maxImageColorRegisters = 256,
             bool _sixelCursorConformance = true);
    ~Terminal();

    /// Retrieves the time point this terminal instance has been spawned.
    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }

    /// Retrieves reference to the underlying PTY device.
    Pty& device() noexcept { return *pty_; }

    Size screenSize() const noexcept { return pty_->screenSize(); }
    void resizeScreen(Size _cells, std::optional<Size> _pixels);

    // {{{ input proxy
    // Sends given input event to connected slave.
    bool send(KeyInputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(CharInputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MousePressEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MouseReleaseEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(MouseMoveEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(FocusInEvent const& _focusEvent, std::chrono::steady_clock::time_point _now);
    bool send(FocusOutEvent const& _focusEvent, std::chrono::steady_clock::time_point _now);

    bool send(MouseEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);
    bool send(InputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);

    /// Sends verbatim text in bracketed mode to application.
    void sendPaste(std::string_view const& _text);
    // }}}

    // {{{ screen proxy
    /// @returns absolute coordinate of @p _pos with scroll offset and applied.
    Coordinate absoluteCoordinate(Coordinate const& _pos) const noexcept
    {
        // TODO: unit test case me BEFORE merge, yo !
        auto const row = screen_.historyLineCount()
                       - viewport_.relativeScrollOffset()
                       + _pos.row;
        auto const col = _pos.column;
        return Coordinate{row, col};
    }

    /// Writes a given VT-sequence to screen.
    void writeToScreen(char const* data, size_t size);
    void writeToScreen(std::string_view const& _text) { writeToScreen(_text.data(), _text.size()); }
    void writeToScreen(std::string const& _text) { writeToScreen(_text.data(), _text.size()); }
    // }}}

    // viewport management
    Viewport& viewport() noexcept { return viewport_; }
    Viewport const& viewport() const noexcept { return viewport_; }

    // {{{ Screen Render Proxy
    /// Checks if a render() method should be called by checking the dirty bit,
    /// and if so, clears the dirty bit and returns true, false otherwise.
    bool shouldRender(std::chrono::steady_clock::time_point const& _now) const;

    std::chrono::milliseconds nextRender(std::chrono::steady_clock::time_point _now) const;

    /// Thread-safe access to screen data for rendering.
    template <typename Renderer, typename... RenderPasses>
    uint64_t render(std::chrono::steady_clock::time_point _now, Renderer const& pass, RenderPasses... passes) const
    {
        auto _l = std::lock_guard{*this};
        auto const changes = preRender(_now);
        renderPass(pass, std::forward<RenderPasses>(passes)...);
        return changes;
    }

    uint64_t preRender(std::chrono::steady_clock::time_point _now) const
    {
        auto const changes = changes_.exchange(0);
        updateCursorVisibilityState(_now);
        return changes;
    }
    // }}}

    void lock() const { screenLock_.lock(); }
    void unlock() const { screenLock_.unlock(); }

    /// Only access this when having locked.
    Screen const& screen() const noexcept { return screen_; }

    /// Only access this when having locked.
    Screen& screen() noexcept { return screen_; }

    Coordinate const& currentMousePosition() const noexcept { return currentMousePosition_; }

    // {{{ cursor management
    CursorDisplay cursorDisplay() const noexcept { return cursorDisplay_; }
    void setCursorDisplay(CursorDisplay _value);

    CursorShape cursorShape() const noexcept { return cursorShape_; }
    void setCursorShape(CursorShape _value);

    bool cursorVisibility() const noexcept { return cursorVisibility_; }

    bool cursorBlinkActive() const noexcept { return cursorBlinkState_; }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept
    {
        return lastCursorBlink_;
    }

    constexpr std::chrono::milliseconds cursorBlinkInterval() const noexcept
    {
        return cursorBlinkInterval_;
    }
    // }}}

    // {{{ selection management
    // TODO: move you, too?
    void setWordDelimiters(std::string const& _wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return wordDelimiters_; }

    Selector const* selector() const noexcept { return selector_.get(); }
    Selector* selector() noexcept { return selector_.get(); }

    template <typename RenderTarget>
    void renderSelection(RenderTarget _renderTarget) const
    {
        if (selector_)
            selector_->render(std::forward<RenderTarget>(_renderTarget));
    }

    void clearSelection();

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept { return selector_ && selector_->state() != Selector::State::Waiting; }

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
    // }}}

  private:
    void flushInput();
    void screenUpdateThread();
    void onScreenReply(std::string_view const& reply);
    void onScreenCommands();
    void updateCursorVisibilityState(std::chrono::steady_clock::time_point _now) const;

    template <typename Renderer, typename... RemainingPasses>
    void renderPass(Renderer const& pass, RemainingPasses... remainingPasses) const
    {
        screen_.render(pass, viewport_.absoluteScrollOffset());

        if constexpr (sizeof...(RemainingPasses) != 0)
            renderPass(std::forward<RemainingPasses>(remainingPasses)...);
    }

  private:
    std::optional<RGBColor> requestDynamicColor(DynamicColorName _name) override;
    void bell() override;
    void bufferChanged(ScreenType) override;
    void scrollbackBufferCleared() override;
    void commands() override;
    void copyToClipboard(std::string_view const& _data) override;
    void dumpState() override;
    void notify(std::string_view const& _title, std::string_view const& _body) override;
    void reply(std::string_view const& _response) override;
    void resetDynamicColor(DynamicColorName _name) override;
    void resizeWindow(int _width, int _height, bool _unitInPixels) override;
    void setApplicationkeypadMode(bool _enabled) override;
    void setBracketedPaste(bool _enabled) override;
    void setCursorStyle(CursorDisplay _display, CursorShape _shape) override;
    void setCursorVisibility(bool _visible) override;
    void setDynamicColor(DynamicColorName _name, RGBColor const& _value) override;
    void setGenerateFocusEvents(bool _enabled) override;
    void setMouseProtocol(MouseProtocol _protocol, bool _enabled) override;
    void setMouseTransport(MouseTransport _transport) override;
    void setMouseWheelMode(InputGenerator::MouseWheelMode _mode) override;
    void setWindowTitle(std::string_view const& _title) override;
    void useApplicationCursorKeys(bool _enabled) override;
    void discardImage(Image const&) override;

  private:
    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> changes_;

    Events& eventListener_;

    Logger logger_;
    std::unique_ptr<Pty> pty_;

    CursorDisplay cursorDisplay_;
    CursorShape cursorShape_;
    bool cursorVisibility_ = true;
    std::chrono::milliseconds cursorBlinkInterval_;
	mutable unsigned cursorBlinkState_;
	mutable std::chrono::steady_clock::time_point lastCursorBlink_;

    std::chrono::steady_clock::time_point startTime_;

    std::u32string wordDelimiters_;
    std::function<void()> onSelectionComplete_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_{};
    unsigned int speedClicks_ = 0;

    terminal::Coordinate currentMousePosition_{0, 0}; // current mouse position
    bool leftMouseButtonPressed_ = false; // tracks left-mouse button pressed state (used for cell selection).

    InputGenerator inputGenerator_;
    InputGenerator::Sequence pendingInput_;
    Screen screen_;
    std::mutex mutable screenLock_;
    std::thread screenUpdateThread_;
    Viewport viewport_;
    std::unique_ptr<Selector> selector_;
};

}  // namespace terminal
