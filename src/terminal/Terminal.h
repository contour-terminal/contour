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

template <typename EventListener> class Screen;

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
class Terminal
{
public:
    class Events {
      public:
        virtual ~Events() = default;

        virtual void requestCaptureBuffer(int /*_absoluteStartLine*/, int /*_lineCount*/) {}
        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void renderBufferUpdated() {}
        virtual void screenUpdated() {}
        virtual FontDef getFontDef() { return {}; }
        virtual void setFontDef(FontDef const& /*_fontSpec*/) {}
        virtual void copyToClipboard(std::string_view /*_data*/) {}
        virtual void dumpState() {}
        virtual void notify(std::string_view /*_title*/, std::string_view /*_body*/) {}
        virtual void onClosed() {}
        virtual void onSelectionCompleted() {}
        virtual void resizeWindow(LineCount, ColumnCount) {}
        virtual void resizeWindow(Width, Height) {}
        virtual void setWindowTitle(std::string_view /*_title*/) {}
        virtual void setTerminalProfile(std::string const& /*_configProfileName*/) {}
        virtual void discardImage(Image const&) {}
    };

    Terminal(Pty& _pty,
             int _ptyReadBufferSize,
             Events& _eventListener,
             LineCount _maxHistoryLineCount = LineCount(0),
             LineOffset _copyLastMarkRangeOffset = LineOffset(0),
             std::chrono::milliseconds _cursorBlinkInterval = std::chrono::milliseconds{500},
             std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now(),
             std::string const& _wordDelimiters = "",
             Modifier _mouseProtocolBypassModifier = Modifier::Shift,
             ImageSize _maxImageSize = ImageSize{Width(800), Height(600)},
             int _maxImageColorRegisters = 256,
             bool _sixelCursorConformance = true,
             ColorPalette _colorPalette = {},
             double _refreshRate = 30.0,
             bool _allowReflowOnResize = true);
    ~Terminal();

    void start();

    void resetHard();

    void setRefreshRate(double _refreshRate);
    void setLastMarkRangeOffset(LineOffset _value) noexcept;

    /// Retrieves the time point this terminal instance has been spawned.
    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }
    std::chrono::steady_clock::time_point currentTime() const noexcept { return currentTime_; }

    /// Retrieves reference to the underlying PTY device.
    Pty& device() noexcept { return pty_; }

    PageSize screenSize() const noexcept { return pty_.screenSize(); }
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels);

    void setMouseProtocolBypassModifier(Modifier _value) { mouseProtocolBypassModifier_ = _value; }
    void setMouseBlockSelectionModifier(Modifier _value) { mouseBlockSelectionModifier_ = _value; }

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    bool sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now);
    bool sendCharPressEvent(char32_t _char, Modifier _modifier, Timestamp _now);
    bool sendMousePressEvent(MouseButton _button, Modifier _modifier, Timestamp _now);
    bool sendMouseMoveEvent(Coordinate _pos, Modifier _modifier, Timestamp _now);
    bool sendMouseReleaseEvent(MouseButton _button, Modifier _modifier, Timestamp _now);
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view _text); // Sends verbatim text in bracketed mode to application.
    void sendRaw(std::string_view _text);   // Sends raw string to the application.

    bool handleMouseSelection(Modifier _modifier, Timestamp _now);

    bool applicationCursorKeys() const noexcept { return inputGenerator_.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return inputGenerator_.applicationKeypad(); }
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view _text);

    // viewport management
    Viewport<Terminal>& viewport() noexcept { return viewport_; }
    Viewport<Terminal> const& viewport() const noexcept { return viewport_; }

    // {{{ Screen Render Proxy
    std::optional<std::chrono::milliseconds> nextRender() const;

    uint64_t tick(std::chrono::steady_clock::time_point _now)
    {
        auto const changes = changes_.exchange(0);
        currentTime_ = _now;
        updateCursorVisibilityState();
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
    /// @param _locked whether or not the Terminal object's lock is already held by the caller.
    ///
    /// @retval true   front buffer now contains the refreshed render buffer.
    /// @retval false  back buffer contains the refreshed render buffer,
    ///                and RenderDoubleBuffer::swapBuffers() must again
    ///                be successfully invoked to swap back/front buffers
    ///                in order to access the refreshed render buffer.
    ///
    /// @note The current time must have been updated in order to get the
    ///       correct cursor blinking state drawn.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    ///
    bool refreshRenderBuffer(bool _locked = false);

    /// Eventually refreshes the render buffer iff
    /// - the screen contents has changed AND refresh rate satisfied,
    /// - viewport has changed, or
    /// - refreshing the render buffer was explicitly requested.
    ///
    /// @param _now    the current time
    /// @param _locked whether or not the Terminal object's lock is already held by the caller.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    bool ensureFreshRenderBuffer(bool _locked = false);

    /// Aquuires read-access handle to front render buffer.
    ///
    /// This also acquires the reader lock and releases it automatically
    /// upon RenderBufferRef destruction.
    ///
    /// @see ensureFreshRenderBuffer()
    /// @see refreshRenderBuffer()
    RenderBufferRef renderBuffer() const { return renderBuffer_.frontBuffer(); }

    RenderBufferState renderBufferState() const noexcept { return renderBuffer_.state; }
    // }}}

    void lock() const { outerLock_.lock(); innerLock_.lock(); }
    void unlock() const { outerLock_.unlock(); innerLock_.unlock(); }

    /// Only access this when having the terminal object locked.
    Screen<Terminal> const& screen() const noexcept { return screen_; }

    /// Only access this when having the terminal object locked.
    Screen<Terminal>& screen() noexcept { return screen_; }

    bool isLineWrapped(LineOffset _lineNumber) const noexcept { return screen_.isLineWrapped(_lineNumber); }

    Coordinate const& currentMousePosition() const noexcept { return currentMousePosition_; }

    // {{{ cursor management
    CursorDisplay cursorDisplay() const noexcept { return cursorDisplay_; }
    void setCursorDisplay(CursorDisplay _value);

    CursorShape cursorShape() const noexcept { return cursorShape_; }
    void setCursorShape(CursorShape _value);

    bool cursorBlinkActive() const noexcept { return cursorBlinkState_; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return screen_.cursor().visible
            && (cursorDisplay_ == CursorDisplay::Steady || cursorBlinkState_);
    }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept
    {
        return lastCursorBlink_;
    }

    constexpr void setCursorBlinkingInterval(std::chrono::milliseconds _value)
    {
        cursorBlinkInterval_ = _value;
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

    Selection const* selector() const noexcept { return selection_.get(); }
    Selection* selector() noexcept { return selection_.get(); }

    template <typename RenderTarget>
    void renderSelection(RenderTarget _renderTarget) const
    {
        if (selection_)
            terminal::renderSelection(*selection_, [&](Coordinate _pos) { _renderTarget(_pos, screen_.at(_pos)); });
    }

    void clearSelection();

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept { return selection_ && selection_->state() != Selection::State::Waiting; }
    bool isSelectionInProgress() const noexcept { return selection_ && selection_->state() != Selection::State::Complete; }
    bool isSelectionComplete() const noexcept { return selection_ && selection_->state() == Selection::State::Complete; }

    /// Tests whether given absolute coordinate is covered by a current selection.
    bool isSelected(Coordinate _coord) const noexcept
    {
        return selection_
            && selection_->state() != Selection::State::Waiting
            && selection_->contains(_coord);
    }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selection> _selector) { selection_ = std::move(selection_); }

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!selection_; }
    // }}}

    std::string extractSelectionText() const;
    std::string extractLastMarkRange() const;

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    bool isMouseHoveringHyperlink() const noexcept { return hoveringHyperlink_.load(); }

    bool processInputOnce();

    void markScreenDirty() { screenDirty_ = true; }
    bool screenDirty() const noexcept { return screenDirty_; }

    uint64_t lastFrameID() const noexcept { return lastFrameID_.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount);
    void bell();
    void bufferChanged(ScreenType);
    void scrollbackBufferCleared();
    void screenUpdated();
    FontDef getFontDef();
    void setFontDef(FontDef const& _fontDef);
    void copyToClipboard(std::string_view _data);
    void dumpState();
    void notify(std::string_view _title, std::string_view _body);
    void reply(std::string_view _response);
    void resizeWindow(PageSize);
    void resizeWindow(ImageSize);
    void setApplicationkeypadMode(bool _enabled);
    void setBracketedPaste(bool _enabled);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setCursorVisibility(bool _visible);
    void setGenerateFocusEvents(bool _enabled);
    void setMouseProtocol(MouseProtocol _protocol, bool _enabled);
    void setMouseTransport(MouseTransport _transport);
    void setMouseWheelMode(InputGenerator::MouseWheelMode _mode);
    void setWindowTitle(std::string_view _title);
    void setTerminalProfile(std::string const& _configProfileName);
    void useApplicationCursorKeys(bool _enabled);
    void hardReset();
    void discardImage(Image const&);
    void markCellDirty(Coordinate _position) noexcept;
    void markRegionDirty(Rect _area) noexcept;
    void synchronizedOutput(bool _enabled);
    void onBufferScrolled(LineCount _n) noexcept;

    void verifyState();

  private:
    void flushInput();
    void mainLoop();
    void refreshRenderBuffer(RenderBuffer& _output); // <- acquires the lock
    void refreshRenderBufferInternal(RenderBuffer& _output);
    std::optional<RenderCursor> renderCursor();
    void updateCursorVisibilityState() const;
    bool updateCursorHoveringState();

    template <typename Renderer, typename... RemainingPasses>
    void renderPass(Renderer && pass, RemainingPasses... remainingPasses) const
    {
        screen_.render(std::forward<Renderer>(pass), viewport_.scrollOffset());

        if constexpr (sizeof...(RemainingPasses) != 0)
            renderPass(std::forward<RemainingPasses>(remainingPasses)...);
    }

    // private data
    //

    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> changes_;

    std::thread::id mainLoopThreadID_{};
    int ptyReadBufferSize_;
    Events& eventListener_;

    std::chrono::milliseconds refreshInterval_;
    bool screenDirty_ = false;
    RenderDoubleBuffer renderBuffer_{};

    Pty& pty_;

    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point currentTime_;

	mutable std::chrono::steady_clock::time_point lastCursorBlink_;
    CursorDisplay cursorDisplay_;
    CursorShape cursorShape_;
    std::chrono::milliseconds cursorBlinkInterval_;
	mutable unsigned cursorBlinkState_;

    std::u32string wordDelimiters_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_{};
    unsigned int speedClicks_ = 0;

    terminal::Coordinate currentMousePosition_{}; // current mouse position
    Modifier mouseProtocolBypassModifier_ = Modifier::Shift;
    Modifier mouseBlockSelectionModifier_ = Modifier::Control;
    bool respectMouseProtocol_ = true; // shift-click can disable that, button release sets it back to true
    bool leftMouseButtonPressed_ = false; // tracks left-mouse button pressed state (used for cell selection).

    InputGenerator inputGenerator_;
    InputGenerator::Sequence pendingInput_;
    LineOffset copyLastMarkRangeOffset_;
    Screen<Terminal> screen_;
    std::mutex mutable outerLock_;
    std::mutex mutable innerLock_;
    std::unique_ptr<std::thread> screenUpdateThread_;
    Viewport<Terminal> viewport_;
    std::unique_ptr<Selection> selection_;
    std::atomic<bool> hoveringHyperlink_ = false;
    std::atomic<bool> renderBufferUpdateEnabled_ = true;

    std::atomic<uint64_t> lastFrameID_ = 0;

    struct SelectionHelper: public terminal::SelectionHelper
    {
        Terminal* terminal;
        explicit SelectionHelper(Terminal* self): terminal{self} {}
        PageSize pageSize() const noexcept override;
        bool wordDelimited(Coordinate _pos) const noexcept override;
        bool wrappedLine(LineOffset _line) const noexcept override;
        bool cellEmpty(Coordinate _pos) const noexcept override;
        int cellWidth(Coordinate _pos) const noexcept override;
    };
    SelectionHelper selectionHelper_;
};

}  // namespace terminal
