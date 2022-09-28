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
#include <terminal/InputHandler.h>
#include <terminal/RenderBuffer.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Selector.h>
#include <terminal/Sequence.h>
#include <terminal/TerminalState.h>
#include <terminal/ViInputHandler.h>
#include <terminal/Viewport.h>
#include <terminal/primitives.h>
#include <terminal/pty/Pty.h>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

namespace terminal
{

template <typename Cell>
class Screen;

/// Helping information to visualize IME text that has not been comitted yet.
struct InputMethodData
{
    // If this string is non-empty, the IME is active and the given data
    // shall be displayed at the cursor's location.
    std::string preeditString;
};

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
class Terminal
{
  public:
    class Events
    {
      public:
        virtual ~Events() = default;

        virtual void requestCaptureBuffer(LineCount /*lines*/, bool /*logical*/) {}
        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void renderBufferUpdated() {}
        virtual void screenUpdated() {}
        virtual FontDef getFontDef() { return {}; }
        virtual void setFontDef(FontDef const& /*_fontSpec*/) {}
        virtual void copyToClipboard(std::string_view /*_data*/) {}
        virtual void inspect() {}
        virtual void notify(std::string_view /*_title*/, std::string_view /*_body*/) {}
        virtual void onClosed() {}
        virtual void pasteFromClipboard(unsigned /*count*/) {}
        virtual void onSelectionCompleted() {}
        virtual void requestWindowResize(LineCount, ColumnCount) {}
        virtual void requestWindowResize(Width, Height) {}
        virtual void setWindowTitle(std::string_view /*_title*/) {}
        virtual void setTerminalProfile(std::string const& /*_configProfileName*/) {}
        virtual void discardImage(Image const&) {}
        virtual void inputModeChanged(ViMode /*mode*/) {}
        virtual void updateHighlights() {}
        virtual void playSound([[maybe_unused]] terminal::Sequence::Parameters const& _) {}
        virtual void onScrollOffsetChanged(ScrollOffset) {}
    };

    Terminal(std::unique_ptr<Pty> _pty,
             size_t ptyBufferObjectSize,
             size_t _ptyReadBufferSize,
             Events& _eventListener,
             LineCount _maxHistoryLineCount = LineCount(0),
             LineOffset _copyLastMarkRangeOffset = LineOffset(0),
             std::chrono::milliseconds _cursorBlinkInterval = std::chrono::milliseconds { 500 },
             std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now(),
             std::string const& _wordDelimiters = "",
             Modifier _mouseProtocolBypassModifier = Modifier::Shift,
             ImageSize _maxImageSize = ImageSize { Width(800), Height(600) },
             unsigned _maxImageColorRegisters = 256,
             bool _sixelCursorConformance = true,
             ColorPalette _colorPalette = {},
             double _refreshRate = 30.0,
             bool _allowReflowOnResize = true,
             std::chrono::milliseconds _highlightTimeout = std::chrono::milliseconds { 150 });
    ~Terminal() = default;

    void start();

    void setRefreshRate(double _refreshRate);
    void setLastMarkRangeOffset(LineOffset _value) noexcept;

    void setMaxHistoryLineCount(LineCount _maxHistoryLineCount);
    LineCount maxHistoryLineCount() const noexcept;

    void setTerminalId(VTType _id) noexcept { state_.terminalId = _id; }
    void setSixelCursorConformance(bool _value) noexcept { state_.sixelCursorConformance = _value; }

    void setMaxImageSize(ImageSize size) noexcept { state_.maxImageSize = size; }

    void setMaxImageSize(ImageSize _effective, ImageSize _limit)
    {
        state_.maxImageSize = _effective;
        state_.maxImageSizeLimit = _limit;
    }

    bool isModeEnabled(AnsiMode m) const noexcept { return state_.modes.enabled(m); }
    bool isModeEnabled(DECMode m) const noexcept { return state_.modes.enabled(m); }
    void setMode(AnsiMode _mode, bool _enable);
    void setMode(DECMode _mode, bool _enable);

    void setTopBottomMargin(std::optional<LineOffset> _top, std::optional<LineOffset> _bottom);
    void setLeftRightMargin(std::optional<ColumnOffset> _left, std::optional<ColumnOffset> _right);

    bool isFullHorizontalMargins() const noexcept
    {
        return state_.margin.horizontal.to.value + 1 == state_.pageSize.columns.value;
    }

    void moveCursorTo(LineOffset _line, ColumnOffset _column);
    void saveCursor();
    void restoreCursor();
    void restoreCursor(Cursor const& _savedCursor);

    void setGraphicsRendition(GraphicsRendition _rendition);
    void setForegroundColor(Color _color);
    void setBackgroundColor(Color _color);
    void setUnderlineColor(Color _color);
    void setHighlightRange(HighlightRange _range);

    // {{{ cursor
    Cursor const& cursor() const noexcept { return state_.cursor; }
    constexpr CellLocation realCursorPosition() const noexcept { return state_.cursor.position; }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    CellLocation toRealCoordinate(CellLocation pos) const noexcept
    {
        if (!state_.cursor.originMode)
            return pos;
        else
            return { pos.line + state_.margin.vertical.from, pos.column + state_.margin.horizontal.from };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    CellLocation clampCoordinate(CellLocation coord) const noexcept
    {
        if (state_.cursor.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    CellLocation clampToOrigin(CellLocation coord) const noexcept
    {
        return { std::clamp(coord.line, LineOffset { 0 }, state_.margin.vertical.to),
                 std::clamp(coord.column, ColumnOffset { 0 }, state_.margin.horizontal.to) };
    }

    LineOffset clampedLine(LineOffset _line) const noexcept
    {
        return std::clamp(_line, LineOffset(0), boxed_cast<LineOffset>(state_.pageSize.lines) - 1);
    }

    ColumnOffset clampedColumn(ColumnOffset _column) const noexcept
    {
        return std::clamp(_column, ColumnOffset(0), boxed_cast<ColumnOffset>(state_.pageSize.columns) - 1);
    }

    CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return { clampedLine(coord.line), clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(CellLocation _coord) const noexcept
    {
        return LineOffset(0) <= _coord.line && _coord.line < boxed_cast<LineOffset>(state_.pageSize.lines)
               && ColumnOffset(0) <= _coord.column
               && _coord.column <= boxed_cast<ColumnOffset>(state_.pageSize.columns);
    }

    bool isCursorInsideMargins() const noexcept
    {
        bool const insideVerticalMargin = state_.margin.vertical.contains(state_.cursor.position.line);
        bool const insideHorizontalMargin =
            !isModeEnabled(DECMode::LeftRightMargin)
            || state_.margin.horizontal.contains(state_.cursor.position.column);
        return insideVerticalMargin && insideHorizontalMargin;
    }

    [[nodiscard]] bool isCursorInViewport() const noexcept
    {
        return viewport().isLineVisible(cursor().position.line);
    }
    // }}}

    constexpr ImageSize cellPixelSize() const noexcept { return state_.cellPixelSize; }
    constexpr void setCellPixelSize(ImageSize _cellPixelSize) { state_.cellPixelSize = _cellPixelSize; }

    /// Retrieves the time point this terminal instance has been spawned.
    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }
    std::chrono::steady_clock::time_point currentTime() const noexcept { return currentTime_; }

    /// Retrieves reference to the underlying PTY device.
    Pty& device() noexcept { return *pty_; }

    PageSize pageSize() const noexcept { return pty_->pageSize(); }

    PageSize totalPageSize() const noexcept
    {
        switch (state_.statusDisplayType)
        {
            case StatusDisplayType::None:
                //.
                return pageSize();
            case StatusDisplayType::Indicator:
            case StatusDisplayType::HostWritable:
                return pageSize() + hostWritableStatusLineScreen_.pageSize().lines;
        }
        crispy::unreachable();
    }

    /// Resizes the terminal screen to the given amount of grid cells with their pixel dimensions.
    /// Important! In case a status line is currently visible, the status line count is being
    /// accumulated into the screen size, too.
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt);
    void resizeScreenInternal(PageSize _cells, std::optional<ImageSize> _pixels);

    /// Implements semantics for  DECCOLM / DECSCPP.
    void resizeColumns(ColumnCount _newColumnCount, bool _clear);

    void clearScreen();

    void setMouseProtocolBypassModifier(Modifier _value) { mouseProtocolBypassModifier_ = _value; }
    void setMouseBlockSelectionModifier(Modifier _value) { mouseBlockSelectionModifier_ = _value; }

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    bool sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now);
    bool sendCharPressEvent(char32_t _char, Modifier _modifier, Timestamp _now);
    bool sendMousePressEvent(Modifier _modifier,
                             MouseButton _button,
                             PixelCoordinate _pixelPosition,
                             Timestamp _now);
    bool sendMouseMoveEvent(Modifier _modifier,
                            CellLocation _pos,
                            PixelCoordinate _pixelPosition,
                            Timestamp _now);
    bool sendMouseReleaseEvent(Modifier _modifier,
                               MouseButton _button,
                               PixelCoordinate _pixelPosition,
                               Timestamp _now);
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view _text); // Sends verbatim text in bracketed mode to application.
    void sendPasteFromClipboard(unsigned count = 1) { eventListener_.pasteFromClipboard(count); }

    bool handleMouseSelection(Modifier _modifier, Timestamp _now);

    void inputModeChanged(ViMode mode) { eventListener_.inputModeChanged(mode); }
    void updateHighlights() { eventListener_.updateHighlights(); }
    void playSound(terminal::Sequence::Parameters const& params_) { eventListener_.playSound(params_); }

    bool applicationCursorKeys() const noexcept { return state_.inputGenerator.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return state_.inputGenerator.applicationKeypad(); }

    bool hasInput() const noexcept;
    size_t pendingInputBytes() const noexcept;
    void flushInput();

    std::string_view peekInput() const noexcept { return state_.inputGenerator.peek(); }
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view _text);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    void writeToScreenInternal(std::string_view _text);

    // viewport management
    Viewport& viewport() noexcept { return viewport_; }
    Viewport const& viewport() const noexcept { return viewport_; }

    // {{{ Screen Render Proxy
    std::optional<std::chrono::milliseconds> nextRender() const;

    uint64_t tick(std::chrono::steady_clock::time_point _now)
    {
        auto const changes = changes_.exchange(0);
        currentTime_ = _now;
        updateCursorVisibilityState();
        if (isBlinkOnScreen())
        {
            tie(_rapidBlinker.state, _lastRapidBlink) = nextBlinkState(_rapidBlinker, _lastRapidBlink);
            tie(_slowBlinker.state, _lastBlink) = nextBlinkState(_slowBlinker, _lastBlink);
        }
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

    /// Updates the IME preedit-string to be rendered when IME is composing a new input.
    /// Passing an empty string effectively disables IME rendering.
    void updateInputMethodPreeditString(std::string preeditString);
    // }}}

    void lock() const
    {
        outerLock_.lock();
        innerLock_.lock();
    }
    void unlock() const
    {
        outerLock_.unlock();
        innerLock_.unlock();
    }

    ColorPalette const& colorPalette() const noexcept { return state_.colorPalette; }
    ColorPalette& colorPalette() noexcept { return state_.colorPalette; }
    ColorPalette& defaultColorPalette() noexcept { return state_.defaultColorPalette; }

    void pushColorPalette(size_t slot);
    void popColorPalette(size_t slot);
    void reportColorPaletteStack();

    ScreenBase& currentScreen() noexcept { return currentScreen_.get(); }
    ScreenBase const& currentScreen() const noexcept { return currentScreen_.get(); }

    ScreenBase& activeDisplay() noexcept
    {
        switch (state_.activeStatusDisplay)
        {
            case ActiveStatusDisplay::Main: return currentScreen_.get();
            case ActiveStatusDisplay::StatusLine: return hostWritableStatusLineScreen_;
        }
        crispy::unreachable();
    }

    bool isPrimaryScreen() const noexcept { return state_.screenType == ScreenType::Primary; }
    bool isAlternateScreen() const noexcept { return state_.screenType == ScreenType::Alternate; }
    ScreenType screenType() const noexcept { return state_.screenType; }
    void setScreen(ScreenType screenType);
    void setHighlightTimeout(std::chrono::milliseconds timeout) noexcept { highlightTimeout_ = timeout; }

    Screen<Cell> const& primaryScreen() const noexcept { return primaryScreen_; }
    Screen<Cell>& primaryScreen() noexcept { return primaryScreen_; }

    Screen<Cell> const& alternateScreen() const noexcept { return alternateScreen_; }
    Screen<Cell>& alternateScreen() noexcept { return alternateScreen_; }

    bool isLineWrapped(LineOffset _lineNumber) const noexcept
    {
        return isPrimaryScreen() && primaryScreen_.isLineWrapped(_lineNumber);
    }

    CellLocation currentMousePosition() const noexcept { return currentMousePosition_; }

    std::optional<CellLocation> currentMouseGridPosition() const noexcept
    {
        if (currentScreen_.get().contains(currentMousePosition_))
            return viewport_.translateScreenToGridCoordinate(currentMousePosition_);
        return std::nullopt;
    }

    // {{{ cursor management
    CursorDisplay cursorDisplay() const noexcept { return cursorDisplay_; }
    void setCursorDisplay(CursorDisplay _value);

    CursorShape cursorShape() const noexcept { return cursorShape_; }
    void setCursorShape(CursorShape _value);

    bool cursorBlinkActive() const noexcept { return cursorBlinkState_; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return state_.cursor.visible && (cursorDisplay_ == CursorDisplay::Steady || cursorBlinkState_);
    }

    bool isBlinkOnScreen() const noexcept { return _lastRenderPassHints.containsBlinkingCells; }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept { return lastCursorBlink_; }

    constexpr void setCursorBlinkingInterval(std::chrono::milliseconds _value)
    {
        cursorBlinkInterval_ = _value;
    }

    constexpr std::chrono::milliseconds cursorBlinkInterval() const noexcept { return cursorBlinkInterval_; }
    // }}}

    // {{{ selection management
    // TODO: move you, too?
    void setWordDelimiters(std::string const& _wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return wordDelimiters_; }

    Selection const* selector() const noexcept { return selection_.get(); }
    Selection* selector() noexcept { return selection_.get(); }
    std::chrono::milliseconds highlightTimeout() const noexcept { return highlightTimeout_; }

    template <typename RenderTarget>
    void renderSelection(RenderTarget _renderTarget) const
    {
        if (!selection_)
            return;

        if (isPrimaryScreen())
            terminal::renderSelection(
                *selection_, [&](CellLocation _pos) { _renderTarget(_pos, primaryScreen_.at(_pos)); });
        else
            terminal::renderSelection(
                *selection_, [&](CellLocation _pos) { _renderTarget(_pos, alternateScreen_.at(_pos)); });
    }

    void clearSelection();

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept
    {
        return selection_ && selection_->state() != Selection::State::Waiting;
    }
    bool isSelectionInProgress() const noexcept
    {
        return selection_ && selection_->state() != Selection::State::Complete;
    }
    bool isSelectionComplete() const noexcept
    {
        return selection_ && selection_->state() == Selection::State::Complete;
    }

    /// Tests whether given absolute coordinate is covered by a current selection.
    bool isSelected(CellLocation _coord) const noexcept
    {
        return selection_ && selection_->state() != Selection::State::Waiting && selection_->contains(_coord);
    }

    bool isHighlighted(CellLocation _cell) const noexcept;
    bool blinkState() const noexcept { return _slowBlinker.state; }
    bool rapidBlinkState() const noexcept { return _rapidBlinker.state; }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selection> _selector) { selection_ = std::move(_selector); }

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!selection_; }
    // }}}

    std::string extractSelectionText() const;
    std::string extractLastMarkRange() const;

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    bool isMouseHoveringHyperlink() const noexcept { return hoveringHyperlink_.load(); }

    /// Retrieves the HyperlinkInfo that is currently behing hovered by the mouse, if so,
    /// or a nothing otherwise.
    std::shared_ptr<HyperlinkInfo const> tryGetHoveringHyperlink() const noexcept
    {
        if (auto const gridPosition = currentMouseGridPosition())
            return currentScreen_.get().hyperlinkAt(*gridPosition);
        return {};
    }

    bool processInputOnce();

    void markScreenDirty() { screenDirty_ = true; }
    bool screenDirty() const noexcept { return screenDirty_; }

    uint64_t lastFrameID() const noexcept { return lastFrameID_.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(LineCount lines, bool logical);
    void bell();
    void bufferChanged(ScreenType);
    void scrollbackBufferCleared();
    void screenUpdated();
    FontDef getFontDef();
    void setFontDef(FontDef const& _fontDef);
    void copyToClipboard(std::string_view _data);
    void inspect();
    void notify(std::string_view _title, std::string_view _body);
    void reply(std::string_view _response);

    template <typename... T>
    void reply(fmt::format_string<T...> fmt, T&&... args)
    {
        reply(fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    void requestWindowResize(PageSize);
    void requestWindowResize(ImageSize);
    void setApplicationkeypadMode(bool _enabled);
    void setBracketedPaste(bool _enabled);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setCursorVisibility(bool _visible);
    void setGenerateFocusEvents(bool _enabled);
    void setMouseProtocol(MouseProtocol _protocol, bool _enabled);
    void setMouseTransport(MouseTransport _transport);
    void setMouseWheelMode(InputGenerator::MouseWheelMode _mode);
    void setWindowTitle(std::string_view _title);
    std::string const& windowTitle() const noexcept;
    void saveWindowTitle();
    void restoreWindowTitle();
    void setTerminalProfile(std::string const& _configProfileName);
    void useApplicationCursorKeys(bool _enabled);
    void softReset();
    void hardReset();
    void discardImage(Image const&);
    void markCellDirty(CellLocation _position) noexcept;
    void markRegionDirty(Rect _area) noexcept;
    void synchronizedOutput(bool _enabled);
    void onBufferScrolled(LineCount _n) noexcept;

    void setMaxImageColorRegisters(unsigned value) noexcept { state_.maxImageColorRegisters = value; }

    /// @returns either an empty string or a file:// URL of the last set working directory.
    std::string const& currentWorkingDirectory() const noexcept { return state_.currentWorkingDirectory; }

    void verifyState();

    TerminalState& state() noexcept { return state_; }
    TerminalState const& state() const noexcept { return state_; }

    void applyPageSizeToCurrentBuffer();

    crispy::BufferObjectPtr currentPtyBuffer() const noexcept { return currentPtyBuffer_; }

    terminal::SelectionHelper& selectionHelper() noexcept { return selectionHelper_; }

    ViInputHandler& inputHandler() noexcept { return state_.inputHandler; }
    ViInputHandler const& inputHandler() const noexcept { return state_.inputHandler; }
    void resetHighlight();

    void setStatusDisplay(StatusDisplayType statusDisplayType);
    void setActiveStatusDisplay(ActiveStatusDisplay activeDisplay);

    void pushStatusDisplay(StatusDisplayType statusDisplayType);
    void popStatusDisplay();

    bool allowInput() const noexcept { return !isModeEnabled(AnsiMode::KeyboardAction); }

    void setAllowInput(bool enabled);

    // Sets the current search term to the given text and
    // moves the viewport accordingly to make sure the given text is visible,
    // or it will not move at all if the input text was not found.
    std::optional<CellLocation> searchReverse(std::u32string text, CellLocation searchPosition);
    std::optional<CellLocation> searchReverse(CellLocation searchPosition);

    // Searches from current position the next item downwards.
    std::optional<CellLocation> search(std::u32string text, CellLocation searchPosition);
    std::optional<CellLocation> search(CellLocation searchPosition);

    // Tests if the grid cell at the given location does contain a word delimiter.
    [[nodiscard]] bool wordDelimited(CellLocation position) const noexcept;

    [[nodiscard]] std::tuple<std::u32string, CellLocationRange> extractWordUnderCursor(
        CellLocation position) const noexcept;

  private:
    void mainLoop();
    void refreshRenderBuffer(RenderBuffer& _output); // <- acquires the lock
    void refreshRenderBufferInternal(RenderBuffer& _output);
    void updateIndicatorStatusLine();
    void updateCursorVisibilityState() const;
    bool updateCursorHoveringState();
    template <typename BlinkerState>
    std::pair<bool, std::chrono::steady_clock::time_point> nextBlinkState(
        BlinkerState blinker, std::chrono::steady_clock::time_point lastBlink) const noexcept
    {
        auto const passed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime_ - lastBlink);
        if (passed < blinker.interval)
            return { blinker.state, lastBlink };
        return { !blinker.state, currentTime_ };
    }

    // Reads from PTY.
    Pty::ReadResult readFromPty();

    // Writes partially or all input data to the PTY buffer object and returns a string view to it.
    std::string_view lockedWriteToPtyBuffer(std::string_view data);

    // private data
    //

    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> changes_;

    Events& eventListener_;

    std::chrono::milliseconds refreshInterval_;
    bool screenDirty_ = false;
    RenderDoubleBuffer renderBuffer_ {};

    std::unique_ptr<Pty> pty_;

    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point currentTime_;

    mutable std::chrono::steady_clock::time_point lastCursorBlink_;
    CursorDisplay cursorDisplay_;
    CursorShape cursorShape_;
    std::chrono::milliseconds cursorBlinkInterval_;
    mutable unsigned cursorBlinkState_;

    std::u32string wordDelimiters_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_ {};
    unsigned int speedClicks_ = 0;

    terminal::CellLocation currentMousePosition_ {}; // current mouse position
    Modifier mouseProtocolBypassModifier_ = Modifier::Shift;
    Modifier mouseBlockSelectionModifier_ = Modifier::Control;
    bool respectMouseProtocol_ = true;    // shift-click can disable that, button release sets it back to true
    bool leftMouseButtonPressed_ = false; // tracks left-mouse button pressed state (used for cell selection).

    LineOffset copyLastMarkRangeOffset_;

    TerminalState state_;
    crispy::BufferObjectPool ptyBufferPool_;
    crispy::BufferObjectPtr currentPtyBuffer_;
    size_t ptyReadBufferSize_;
    Screen<Cell> primaryScreen_;
    Screen<Cell> alternateScreen_;
    Screen<Cell> hostWritableStatusLineScreen_;
    Screen<Cell> indicatorStatusScreen_;
    std::reference_wrapper<ScreenBase> currentScreen_;

    InputMethodData inputMethodData_;

    std::mutex mutable outerLock_;
    std::mutex mutable innerLock_;
    Viewport viewport_;
    std::unique_ptr<Selection> selection_;
    std::atomic<bool> hoveringHyperlink_ = false;
    std::atomic<bool> renderBufferUpdateEnabled_ = true;

    std::atomic<uint64_t> lastFrameID_ = 0;

    struct SelectionHelper: public terminal::SelectionHelper
    {
        Terminal* terminal;
        explicit SelectionHelper(Terminal* self): terminal { self } {}
        [[nodiscard]] PageSize pageSize() const noexcept override;
        [[nodiscard]] bool wordDelimited(CellLocation _pos) const noexcept override;
        [[nodiscard]] bool wrappedLine(LineOffset _line) const noexcept override;
        [[nodiscard]] bool cellEmpty(CellLocation _pos) const noexcept override;
        [[nodiscard]] int cellWidth(CellLocation _pos) const noexcept override;
    };
    SelectionHelper selectionHelper_;
    std::optional<HighlightRange> highlightRange_ = std::nullopt;
    std::chrono::milliseconds highlightTimeout_;
    struct BlinkerState
    {
        bool state = false;
        std::chrono::milliseconds const interval;
    };
    RenderPassHints _lastRenderPassHints;
    mutable BlinkerState _slowBlinker { false, std::chrono::milliseconds { 500 } };
    mutable BlinkerState _rapidBlinker { false, std::chrono::milliseconds { 300 } };
    mutable std::chrono::steady_clock::time_point _lastBlink;
    mutable std::chrono::steady_clock::time_point _lastRapidBlink;
};

} // namespace terminal
