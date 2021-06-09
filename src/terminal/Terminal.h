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

#include <terminal/InputGenerator.h>
#include <terminal/pty/Pty.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Screen.h>
#include <terminal/Selector.h>
#include <terminal/Viewport.h>
#include <terminal/RenderBuffer.h>

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

        virtual void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) = 0;
        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void renderBufferUpdated() = 0;
        virtual void screenUpdated() {}
        virtual FontDef getFontDef() { return {}; }
        virtual void setFontDef(FontDef const& /*_fontSpec*/) {}
        virtual void copyToClipboard(std::string_view const& /*_data*/) {}
        virtual void dumpState() {}
        virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
        virtual void reply(std::string_view const& /*_response*/) {}
        virtual void onClosed() {}
        virtual void onSelectionComplete() {}
        virtual void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) {}
        virtual void setWindowTitle(std::string_view const& /*_title*/) {}
        virtual void setTerminalProfile(std::string const& /*_configProfileName*/) {}
        virtual void discardImage(Image const&) {}
    };

    Terminal(std::unique_ptr<Pty> _pty,
             Events& _eventListener,
             std::optional<size_t> _maxHistoryLineCount = std::nullopt,
             std::chrono::milliseconds _cursorBlinkInterval = std::chrono::milliseconds{500},
             std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now(),
             std::string const& _wordDelimiters = "",
             Modifier _mouseProtocolBypassModifier = Modifier::Shift,
             crispy::Size _maxImageSize = crispy::Size{800, 600},
             int _maxImageColorRegisters = 256,
             bool _sixelCursorConformance = true,
             ColorPalette _colorPalette = {},
             double _refreshRate = 30.0);
    ~Terminal();

    void start();

    void setRefreshRate(double _refreshRate);

    /// Retrieves the time point this terminal instance has been spawned.
    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }

    /// Retrieves reference to the underlying PTY device.
    Pty& device() noexcept { return *pty_; }

    crispy::Size screenSize() const noexcept { return pty_->screenSize(); }
    void resizeScreen(crispy::Size _cells, std::optional<crispy::Size> _pixels);

    void setMouseProtocolBypassModifier(Modifier _value) { mouseProtocolBypassModifier_ = _value; }

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

    void sendRaw(std::string_view const& _text);
    // }}}

    // {{{ screen proxy
    /// @returns absolute coordinate of @p _pos with scroll offset and applied.
    Coordinate absoluteCoordinate(Coordinate const& _pos) const noexcept
    {
        // TODO: unit test case me BEFORE merge, yo !
        auto const row = viewport_.absoluteScrollOffset().value_or(screen_.historyLineCount()) + (_pos.row - 1);
        auto const col = _pos.column;
        return Coordinate{row, col};
    }

    /// Writes a given VT-sequence to screen.
    void writeToScreen(char const* data, size_t size);
    void writeToScreen(std::string_view _text) { writeToScreen(_text.data(), _text.size()); }
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
        auto const changes = tick(_now);
        renderPass(pass, std::forward<RenderPasses>(passes)...);
        return changes;
    }

    uint64_t tick(std::chrono::steady_clock::time_point _now) const
    {
        auto const changes = changes_.exchange(0);
        updateCursorVisibilityState(_now);
        return changes;
    }
    // }}}

    // {{{ RenderBuffer synchronization API

    /// Ensures the terminals event loop is interrupted
    /// and the render buffer is refreshed.
    ///
    void breakLoopAndRefreshRenderBuffer();

    /// Refreshes the render buffer.
    /// When this function returns, the back buffer is updated
    /// and it is attempted to swap the back/front buffers.
    /// but the swap has NOT been invoked yet.
    ///
    /// @retval true   front buffer now contains the refreshed render buffer.
    /// @retval false  back buffer contains the refreshed render buffer,
    ///                and RenderDoubleBuffer::swapBuffers() must again
    ///                be successfully invoked to swap back/front buffers
    ///                in order to access the refreshed render buffer.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    ///
    bool refreshRenderBuffer(std::chrono::steady_clock::time_point _now);

    /// Eventually refreshes the render buffer iff
    /// - the screen contents has changed AND refresh rate satisfied,
    /// - viewport has changed, or
    /// - refreshing the render buffer was explicitly requested.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    void ensureFreshRenderBuffer(std::chrono::steady_clock::time_point _now);

    /// Aquuires read-access handle to front render buffer.
    ///
    /// This also acquires the reader lock and releases it automatically
    /// upon RenderBufferRef destruction.
    ///
    /// @see ensureFreshRenderBuffer()
    /// @see refreshRenderBuffer()
    RenderBufferRef renderBuffer() { return renderBuffer_.frontBuffer(); }
    // }}}

    void lock() const { outerLock_.lock(); innerLock_.lock(); }
    void unlock() const { outerLock_.unlock(); innerLock_.unlock(); }

    /// Only access this when having the terminal object locked.
    Screen const& screen() const noexcept { return screen_; }

    /// Only access this when having the terminal object locked.
    Screen& screen() noexcept { return screen_; }

    bool lineWrapped(int _lineNumber) const { return screen_.lineWrapped(_lineNumber); }

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

    std::string extractSelectionText() const;
    std::string extractLastMarkRange() const;

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    bool isMouseHoveringHyperlink() const noexcept { return hoveringHyperlink_.load(); }

  private:
    void flushInput();
    void mainLoop();
    void refreshRenderBuffer(RenderBuffer& _output);
    std::optional<RenderCursor> renderCursor();
    void updateCursorVisibilityState(std::chrono::steady_clock::time_point _now) const;
    bool updateCursorHoveringState();

    template <typename Renderer, typename... RemainingPasses>
    void renderPass(Renderer const& pass, RemainingPasses... remainingPasses) const
    {
        screen_.render(pass, viewport_.absoluteScrollOffset());

        if constexpr (sizeof...(RemainingPasses) != 0)
            renderPass(std::forward<RemainingPasses>(remainingPasses)...);
    }

    // overrides
    //
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) override;
    void bell() override;
    void bufferChanged(ScreenType) override;
    void scrollbackBufferCleared() override;
    void screenUpdated() override;
    FontDef getFontDef() override;
    void setFontDef(FontDef const& _fontDef) override;
    void copyToClipboard(std::string_view const& _data) override;
    void dumpState() override;
    void notify(std::string_view const& _title, std::string_view const& _body) override;
    void reply(std::string_view const& _response) override;
    void resizeWindow(int _width, int _height, bool _unitInPixels) override;
    void setApplicationkeypadMode(bool _enabled) override;
    void setBracketedPaste(bool _enabled) override;
    void setCursorStyle(CursorDisplay _display, CursorShape _shape) override;
    void setCursorVisibility(bool _visible) override;
    void setGenerateFocusEvents(bool _enabled) override;
    void setMouseProtocol(MouseProtocol _protocol, bool _enabled) override;
    void setMouseTransport(MouseTransport _transport) override;
    void setMouseWheelMode(InputGenerator::MouseWheelMode _mode) override;
    void setWindowTitle(std::string_view const& _title) override;
    void setTerminalProfile(std::string const& _configProfileName) override;
    void useApplicationCursorKeys(bool _enabled) override;
    void hardReset() override;
    void discardImage(Image const&) override;

    // private data
    //

    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> changes_;

    std::thread::id mainLoopThreadID_{};
    Events& eventListener_;

    std::chrono::milliseconds refreshInterval_;
    bool screenDirty_ = false;
    RenderDoubleBuffer renderBuffer_{};

    std::unique_ptr<Pty> pty_;

    CursorDisplay cursorDisplay_;
    CursorShape cursorShape_;
    bool cursorVisibility_ = true;
    std::chrono::milliseconds cursorBlinkInterval_;
	mutable unsigned cursorBlinkState_;
	mutable std::chrono::steady_clock::time_point lastCursorBlink_;

    std::chrono::steady_clock::time_point startTime_;

    std::u32string wordDelimiters_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_{};
    unsigned int speedClicks_ = 0;

    terminal::Coordinate currentMousePosition_{0, 0}; // current mouse position
    Modifier mouseProtocolBypassModifier_ = Modifier::Shift;
    bool respectMouseProtocol_ = true; // shift-click can disable that, button release sets it back to true
    bool leftMouseButtonPressed_ = false; // tracks left-mouse button pressed state (used for cell selection).

    InputGenerator inputGenerator_;
    InputGenerator::Sequence pendingInput_;
    Screen screen_;
    std::mutex mutable outerLock_;
    std::mutex mutable innerLock_;
    std::unique_ptr<std::thread> screenUpdateThread_;
    Viewport viewport_;
    std::unique_ptr<Selector> selector_;
    std::atomic<bool> hoveringHyperlink_ = false;
};

}  // namespace terminal
