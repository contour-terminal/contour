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

#include <vtbackend/InputGenerator.h>
#include <vtbackend/InputHandler.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/ScreenEvents.h>
#include <vtbackend/Selector.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/Settings.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/Viewport.h>
#include <vtbackend/cell/CellConcept.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/primitives.h>

#include <vtpty/Pty.h>

#include <crispy/assert.h>
#include <crispy/defines.h>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

namespace terminal
{

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
class Screen;

/// Helping information to visualize IME text that has not been comitted yet.
struct InputMethodData
{
    // If this string is non-empty, the IME is active and the given data
    // shall be displayed at the cursor's location.
    std::string preeditString;
};

// Implements Trace mode handling for the given controls.
//
// It either directly forwards the sequences to the actually current main display,
// or puts them into a pending queue if execution is currently suspend.
//
// In case of single-step, only one sequence will be handled and the execution mode put to suspend mode,
// and in case of break-at-frame, the execution will conditionally break iff the currently
// pending VT sequence indicates a frame start.
class TraceHandler: public SequenceHandler
{
  public:
    explicit TraceHandler(Terminal& terminal);

    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& sequence) override;
    void writeText(char32_t codepoint) override;
    void writeText(std::string_view codepoints, size_t cellCount) override;

    struct CodepointSequence
    {
        std::string_view text;
        size_t cellCount;
    };
    using PendingSequence = std::variant<char32_t, CodepointSequence, Sequence>;
    using PendingSequenceQueue = std::deque<PendingSequence>;

    [[nodiscard]] PendingSequenceQueue const& pendingSequences() const noexcept { return _pendingSequences; }

    void flushAllPending();
    void flushOne();

  private:
    void flushOne(PendingSequence const& pendingSequence);
    Terminal& _terminal;
    PendingSequenceQueue _pendingSequences = {};
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
        virtual void bufferChanged(screen_type) {}
        virtual void renderBufferUpdated() {}
        virtual void screenUpdated() {}
        virtual FontDef getFontDef() { return {}; }
        virtual void setFontDef(FontDef const& /*fontSpec*/) {}
        virtual void copyToClipboard(std::string_view /*data*/) {}
        virtual void inspect() {}
        virtual void notify(std::string_view /*title*/, std::string_view /*body*/) {}
        virtual void onClosed() {}
        virtual void pasteFromClipboard(unsigned /*count*/, bool /*strip*/) {}
        virtual void onSelectionCompleted() {}
        virtual void requestWindowResize(LineCount, ColumnCount) {}
        virtual void requestWindowResize(width, height) {}
        virtual void requestShowHostWritableStatusLine() {}
        virtual void setWindowTitle(std::string_view /*title*/) {}
        virtual void setTerminalProfile(std::string const& /*configProfileName*/) {}
        virtual void discardImage(Image const&) {}
        virtual void inputModeChanged(vi_mode /*mode*/) {}
        virtual void updateHighlights() {}
        virtual void playSound(Sequence::Parameters const&) {}
        virtual void cursorPositionChanged() {}
        virtual void onScrollOffsetChanged(scroll_offset) {}
    };

    Terminal(Events& eventListener,
             std::unique_ptr<Pty> pty,
             Settings factorySettings,
             std::chrono::steady_clock::time_point now /* = std::chrono::steady_clock::now()*/);
    ~Terminal() = default;

    void start();

    void setRefreshRate(RefreshRate refreshRate);
    void setLastMarkRangeOffset(line_offset value) noexcept;

    void setMaxHistoryLineCount(max_history_line_count maxHistoryLineCount);
    LineCount maxHistoryLineCount() const noexcept;

    void setTerminalId(VTType id) noexcept { _state.terminalId = id; }

    void setMaxImageSize(image_size size) noexcept { _state.effectiveImageCanvasSize = size; }

    void setMaxImageSize(image_size effective, image_size limit)
    {
        _state.effectiveImageCanvasSize = effective;
        _settings.maxImageSize = limit;
    }

    bool isModeEnabled(ansi_mode m) const noexcept { return _state.modes.enabled(m); }
    bool isModeEnabled(dec_mode m) const noexcept { return _state.modes.enabled(m); }
    void setMode(ansi_mode mode, bool enable);
    void setMode(dec_mode mode, bool enable);

    void setTopBottomMargin(std::optional<line_offset> top, std::optional<line_offset> bottom);
    void setLeftRightMargin(std::optional<column_offset> left, std::optional<column_offset> right);

    void moveCursorTo(line_offset line, column_offset column);

    void setGraphicsRendition(graphics_rendition rendition);
    void setForegroundColor(color color);
    void setBackgroundColor(color color);
    void setUnderlineColor(color color);
    void setHighlightRange(HighlightRange range);

    // {{{ cursor
    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    [[nodiscard]] cell_location clampToOrigin(cell_location coord) const noexcept
    {
        return { std::clamp(coord.line, line_offset { 0 }, currentScreen().margin().vertical.to),
                 std::clamp(coord.column, column_offset { 0 }, currentScreen().margin().horizontal.to) };
    }

    [[nodiscard]] line_offset clampedLine(line_offset line) const noexcept
    {
        return std::clamp(line, line_offset(0), boxed_cast<line_offset>(_settings.pageSize.lines) - 1);
    }

    [[nodiscard]] column_offset clampedColumn(column_offset column) const noexcept
    {
        return std::clamp(
            column, column_offset(0), boxed_cast<column_offset>(_settings.pageSize.columns) - 1);
    }

    [[nodiscard]] cell_location clampToScreen(cell_location coord) const noexcept
    {
        return { clampedLine(coord.line), clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] constexpr bool contains(cell_location coord) const noexcept
    {
        return line_offset(0) <= coord.line && coord.line < boxed_cast<line_offset>(_settings.pageSize.lines)
               && column_offset(0) <= coord.column
               && coord.column <= boxed_cast<column_offset>(_settings.pageSize.columns);
    }

    [[nodiscard]] bool isCursorInViewport() const noexcept
    {
        return viewport().isLineVisible(currentScreen().cursor().position.line);
    }
    // }}}

    [[nodiscard]] constexpr image_size cellPixelSize() const noexcept { return _state.cellPixelSize; }
    constexpr void setCellPixelSize(image_size cellPixelSize) { _state.cellPixelSize = cellPixelSize; }

    /// Retrieves the time point this terminal instance has been spawned.
    [[nodiscard]] std::chrono::steady_clock::time_point currentTime() const noexcept { return _currentTime; }

    /// Retrieves reference to the underlying PTY device.
    [[nodiscard]] Pty& device() noexcept { return *_pty; }
    [[nodiscard]] Pty const& device() const noexcept { return *_pty; }

    [[nodiscard]] PageSize pageSize() const noexcept { return _pty->pageSize(); }

    [[nodiscard]] PageSize totalPageSize() const noexcept { return _settings.pageSize; }

    // Returns number of lines for the currently displayed status line,
    // or 0 if status line is currently not displayed.
    [[nodiscard]] LineCount statusLineHeight() const noexcept
    {
        switch (_state.statusDisplayType)
        {
            case status_display_type::None: return LineCount(0);
            case status_display_type::Indicator: return _indicatorStatusScreen.pageSize().lines;
            case status_display_type::HostWritable: return _hostWritableStatusLineScreen.pageSize().lines;
        }
        crispy::unreachable();
    }

    /// Resizes the terminal screen to the given amount of grid cells with their pixel dimensions.
    /// Important! In case a status line is currently visible, the status line count is being
    /// accumulated into the screen size, too.
    void resizeScreen(PageSize totalPageSize, std::optional<image_size> pixels = std::nullopt);
    void resizeScreenInternal(PageSize totalPageSize, std::optional<image_size> pixels);

    /// Implements semantics for  DECCOLM / DECSCPP.
    void resizeColumns(ColumnCount newColumnCount, bool clear);

    void clearScreen();

    void setMouseProtocolBypassModifier(Modifier value) { _settings.mouseProtocolBypassModifier = value; }
    void setMouseBlockSelectionModifier(Modifier value) { _settings.mouseBlockSelectionModifier = value; }

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    bool sendKeyPressEvent(Key key, Modifier modifier, Timestamp now);
    bool sendCharPressEvent(char32_t ch, Modifier modifier, Timestamp now);
    bool sendMousePressEvent(Modifier modifier,
                             MouseButton button,
                             pixel_coordinate pixelPosition,
                             bool uiHandledHint);
    void sendMouseMoveEvent(Modifier modifier,
                            cell_location newPosition,
                            pixel_coordinate pixelPosition,
                            bool uiHandledHint);
    bool sendMouseReleaseEvent(Modifier modifier,
                               MouseButton button,
                               pixel_coordinate pixelPosition,
                               bool uiHandledHint);
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view text); // Sends verbatim text in bracketed mode to application.
    void sendPasteFromClipboard(unsigned count, bool strip)
    {
        _eventListener.pasteFromClipboard(count, strip);
    }
    void sendRawInput(std::string_view text);

    void inputModeChanged(vi_mode mode) { _eventListener.inputModeChanged(mode); }
    void updateHighlights() { _eventListener.updateHighlights(); }
    void playSound(terminal::Sequence::Parameters const& params) { _eventListener.playSound(params); }

    bool applicationCursorKeys() const noexcept { return _state.inputGenerator.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return _state.inputGenerator.applicationKeypad(); }

    bool hasInput() const noexcept;
    void flushInput();

    std::string_view peekInput() const noexcept { return _state.inputGenerator.peek(); }
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view vtStream);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    void writeToScreenInternal(std::string_view vtStream);

    // viewport management
    [[nodiscard]] Viewport& viewport() noexcept { return _viewport; }
    [[nodiscard]] Viewport const& viewport() const noexcept { return _viewport; }

    // {{{ Screen Render Proxy
    std::optional<std::chrono::milliseconds> nextRender() const;

    /// Updates the internal clock to the given time point,
    /// and ensures internal time-dependant state is updated.
    void tick(std::chrono::steady_clock::time_point now) noexcept;
    void tick(std::chrono::milliseconds delta) { tick(_currentTime + delta); }
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
    /// @param locked whether or not the Terminal object's lock is already held by the caller.
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
    bool refreshRenderBuffer(bool locked = false);

    /// Eventually refreshes the render buffer iff
    /// - the screen contents has changed AND refresh rate satisfied,
    /// - viewport has changed, or
    /// - refreshing the render buffer was explicitly requested.
    ///
    /// @param now    the current time
    /// @param locked whether or not the Terminal object's lock is already held by the caller.
    ///
    /// @see RenderDoubleBuffer::swapBuffers()
    /// @see renderBuffer()
    bool ensureFreshRenderBuffer(bool locked = false);

    /// Aquuires read-access handle to front render buffer.
    ///
    /// This also acquires the reader lock and releases it automatically
    /// upon RenderBufferRef destruction.
    ///
    /// @see ensureFreshRenderBuffer()
    /// @see refreshRenderBuffer()
    [[nodiscard]] RenderBufferRef renderBuffer() const { return _renderBuffer.frontBuffer(); }

    [[nodiscard]] RenderBufferState renderBufferState() const noexcept { return _renderBuffer.state; }

    /// Updates the IME preedit-string to be rendered when IME is composing a new input.
    /// Passing an empty string effectively disables IME rendering.
    void updateInputMethodPreeditString(std::string preeditString);
    // }}}

    void lock() const
    {
        _outerLock.lock();
        _innerLock.lock();
    }

    void unlock() const
    {
        _outerLock.unlock();
        _innerLock.unlock();
    }

    [[nodiscard]] ColorPalette const& colorPalette() const noexcept { return _state.colorPalette; }
    [[nodiscard]] ColorPalette& colorPalette() noexcept { return _state.colorPalette; }
    [[nodiscard]] ColorPalette& defaultColorPalette() noexcept { return _state.defaultColorPalette; }

    void pushColorPalette(size_t slot);
    void popColorPalette(size_t slot);
    void reportColorPaletteStack();

    [[nodiscard]] ScreenBase& currentScreen() noexcept { return _currentScreen.get(); }
    [[nodiscard]] ScreenBase const& currentScreen() const noexcept { return _currentScreen.get(); }

    [[nodiscard]] ScreenBase& activeDisplay() noexcept
    {
        switch (_state.activeStatusDisplay)
        {
            case terminal::active_status_display::Main: return _currentScreen.get();
            case terminal::active_status_display::StatusLine: return _hostWritableStatusLineScreen;
            case terminal::active_status_display::IndicatorStatusLine: return _indicatorStatusScreen;
        }
        crispy::unreachable();
    }

    [[nodiscard]] SequenceHandler& sequenceHandler() noexcept
    {
        // TODO: avoid double-switch by introducing a `SequenceHandler& sequenceHandler` member.
        switch (_state.executionMode)
        {
            case ExecutionMode::Normal: return activeDisplay();
            case ExecutionMode::BreakAtEmptyQueue:
            case ExecutionMode::Waiting: [[fallthrough]];
            case ExecutionMode::SingleStep: return _traceHandler;
        }
        crispy::unreachable();
    }

    bool isPrimaryScreen() const noexcept { return _state.screenType == screen_type::Primary; }
    bool isAlternateScreen() const noexcept { return _state.screenType == screen_type::Alternate; }
    screen_type screenType() const noexcept { return _state.screenType; }
    void setScreen(screen_type screenType);

    ScreenBase& screenForType(screen_type type) noexcept
    {
        switch (type)
        {
            case screen_type::Primary: return _primaryScreen;
            case screen_type::Alternate: return _alternateScreen;
        }
        crispy::unreachable();
    }

    void setHighlightTimeout(std::chrono::milliseconds timeout) noexcept
    {
        _settings.highlightTimeout = timeout;
    }

    // clang-format off
    [[nodiscard]] Screen<PrimaryScreenCell> const& primaryScreen() const noexcept { return _primaryScreen; }
    [[nodiscard]] Screen<PrimaryScreenCell>& primaryScreen() noexcept { return _primaryScreen; }
    [[nodiscard]] Screen<AlternateScreenCell> const& alternateScreen() const noexcept { return _alternateScreen; }
    [[nodiscard]] Screen<AlternateScreenCell>& alternateScreen() noexcept { return _alternateScreen; }
    [[nodiscard]] Screen<StatusDisplayCell> const& hostWritableStatusLineDisplay() const noexcept { return _hostWritableStatusLineScreen; }
    [[nodiscard]] Screen<StatusDisplayCell> const& indicatorStatusLineDisplay() const noexcept { return _indicatorStatusScreen; }
    // clang-format on

    [[nodiscard]] bool isLineWrapped(line_offset lineNumber) const noexcept
    {
        return isPrimaryScreen() && _primaryScreen.isLineWrapped(lineNumber);
    }

    [[nodiscard]] cell_location currentMousePosition() const noexcept { return _currentMousePosition; }

    [[nodiscard]] std::optional<cell_location> currentMouseGridPosition() const noexcept
    {
        if (_currentScreen.get().contains(_currentMousePosition))
            return _viewport.translateScreenToGridCoordinate(_currentMousePosition);
        return std::nullopt;
    }

    // {{{ cursor management
    cursor_display cursorDisplay() const noexcept { return _settings.cursorDisplay; }
    void setCursorDisplay(cursor_display display);

    cursor_shape cursorShape() const noexcept { return _settings.cursorShape; }
    void setCursorShape(cursor_shape shape);

    bool cursorBlinkActive() const noexcept { return _cursorBlinkState; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return isModeEnabled(dec_mode::VisibleCursor)
               && (cursorDisplay() == cursor_display::Steady || _cursorBlinkState);
    }

    bool isBlinkOnScreen() const noexcept { return _lastRenderPassHints.containsBlinkingCells; }

    std::chrono::steady_clock::time_point lastCursorBlink() const noexcept { return _lastCursorBlink; }

    constexpr void setCursorBlinkingInterval(std::chrono::milliseconds value)
    {
        _settings.cursorBlinkInterval = value;
    }

    constexpr std::chrono::milliseconds cursorBlinkInterval() const noexcept
    {
        return _settings.cursorBlinkInterval;
    }
    // }}}

    // {{{ selection management
    // TODO: move you, too?
    void setWordDelimiters(std::string const& wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return _settings.wordDelimiters; }

    Selection const* selector() const noexcept { return _selection.get(); }
    Selection* selector() noexcept { return _selection.get(); }
    std::chrono::milliseconds highlightTimeout() const noexcept { return _settings.highlightTimeout; }

    template <typename RenderTarget>
    void renderSelection(RenderTarget renderTarget) const
    {
        if (!_selection)
            return;

        if (isPrimaryScreen())
            terminal::renderSelection(*_selection,
                                      [&](cell_location pos) { renderTarget(pos, _primaryScreen.at(pos)); });
        else
            terminal::renderSelection(
                *_selection, [&](cell_location pos) { renderTarget(pos, _alternateScreen.at(pos)); });
    }

    void clearSelection();

    /// Tests whether some area has been selected.
    bool isSelectionAvailable() const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting;
    }
    bool isSelectionInProgress() const noexcept
    {
        return _selection && _selection->state() != Selection::State::Complete;
    }
    bool isSelectionComplete() const noexcept
    {
        return _selection && _selection->state() == Selection::State::Complete;
    }

    /// Tests whether given absolute coordinate is covered by a current selection.
    bool isSelected(cell_location coord) const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting && _selection->contains(coord);
    }

    /// Tests whether given line offset is intersecting with selection.
    bool isSelected(line_offset line) const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting
               && _selection->containsLine(line);
    }

    bool isHighlighted(cell_location cell) const noexcept;
    bool blinkState() const noexcept { return _slowBlinker.state; }
    bool rapidBlinkState() const noexcept { return _rapidBlinker.state; }

    /// Sets or resets to a new selection.
    void setSelector(std::unique_ptr<Selection> selector);

    /// Tests whether or not some grid cells are selected.
    bool selectionAvailable() const noexcept { return !!_selection; }

    bool visualizeSelectedWord() const noexcept { return _settings.visualizeSelectedWord; }
    void setVisualizeSelectedWord(bool enabled) noexcept { _settings.visualizeSelectedWord = enabled; }
    // }}}

    [[nodiscard]] std::string extractSelectionText() const;
    [[nodiscard]] std::string extractLastMarkRange() const;

    /// Tests whether or not the mouse is currently hovering a hyperlink.
    [[nodiscard]] bool isMouseHoveringHyperlink() const noexcept
    {
        return _hoveringHyperlinkId.load().value != 0;
    }

    /// Retrieves the HyperlinkInfo that is currently behing hovered by the mouse, if so,
    /// or a nothing otherwise.
    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> tryGetHoveringHyperlink() const noexcept
    {
        if (auto const gridPosition = currentMouseGridPosition())
            return _currentScreen.get().hyperlinkAt(*gridPosition);
        return {};
    }

    [[nodiscard]] ExecutionMode executionMode() const noexcept { return _state.executionMode; }
    void setExecutionMode(ExecutionMode mode);

    bool processInputOnce();

    void markScreenDirty() noexcept { _screenDirty = true; }

    [[nodiscard]] uint64_t lastFrameID() const noexcept { return _lastFrameID.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(LineCount lines, bool logical);
    void requestShowHostWritableStatusLine();
    void bell();
    void bufferChanged(screen_type);
    void scrollbackBufferCleared();
    void screenUpdated();
    void renderBufferUpdated();
    [[nodiscard]] FontDef getFontDef();
    void setFontDef(FontDef const& fontDef);
    void copyToClipboard(std::string_view data);
    void inspect();
    void notify(std::string_view title, std::string_view body);
    void reply(std::string_view text);

    template <typename... T>
    void reply(fmt::format_string<T...> fmt, T&&... args)
    {
        reply(fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    void requestWindowResize(PageSize);
    void requestWindowResize(image_size);
    void setApplicationkeypadMode(bool enabled);
    void setBracketedPaste(bool enabled);
    void setCursorStyle(cursor_display display, cursor_shape shape);
    void setCursorVisibility(bool visible);
    void setGenerateFocusEvents(bool enabled);
    void setMouseProtocol(MouseProtocol protocol, bool enabled);
    void setMouseTransport(MouseTransport transport);
    void setMouseWheelMode(InputGenerator::MouseWheelMode mode);
    void setWindowTitle(std::string_view title);
    [[nodiscard]] std::string const& windowTitle() const noexcept;
    void saveWindowTitle();
    void restoreWindowTitle();
    void setTerminalProfile(std::string const& configProfileName);
    void useApplicationCursorKeys(bool enabled);
    void softReset();
    void hardReset();
    void forceRedraw(std::function<void()> const& artificialSleep);
    void discardImage(Image const&);
    void markCellDirty(cell_location position) noexcept;
    void markRegionDirty(rect area) noexcept;
    void synchronizedOutput(bool enabled);
    void onBufferScrolled(LineCount n) noexcept;

    void setMaxImageColorRegisters(unsigned value) noexcept { _state.maxImageColorRegisters = value; }

    /// @returns either an empty string or a file:// URL of the last set working directory.
    [[nodiscard]] std::string const& currentWorkingDirectory() const noexcept
    {
        return _state.currentWorkingDirectory;
    }

    void verifyState();

    [[nodiscard]] TerminalState& state() noexcept { return _state; }
    [[nodiscard]] TerminalState const& state() const noexcept { return _state; }

    void applyPageSizeToCurrentBuffer();
    void applyPageSizeToMainDisplay(screen_type screenType);

    [[nodiscard]] crispy::BufferObjectPtr<char> currentPtyBuffer() const noexcept
    {
        return _currentPtyBuffer;
    }

    [[nodiscard]] terminal::SelectionHelper& selectionHelper() noexcept { return _selectionHelper; }

    [[nodiscard]] Selection::OnSelectionUpdated selectionUpdatedHelper()
    {
        return [this]() {
            onSelectionUpdated();
        };
    }

    void onSelectionUpdated();

    [[nodiscard]] ViInputHandler& inputHandler() noexcept { return _state.inputHandler; }
    [[nodiscard]] ViInputHandler const& inputHandler() const noexcept { return _state.inputHandler; }
    void resetHighlight();

    status_display_type statusDisplayType() const noexcept { return _state.statusDisplayType; }
    void setStatusDisplay(status_display_type statusDisplayType);
    void setActiveStatusDisplay(active_status_display activeDisplay);

    void pushStatusDisplay(status_display_type statusDisplayType);
    void popStatusDisplay();

    bool allowInput() const noexcept { return !isModeEnabled(ansi_mode::KeyboardAction); }

    void setAllowInput(bool enabled);

    // Sets the current search term to the given text and
    // moves the viewport accordingly to make sure the given text is visible,
    // or it will not move at all if the input text was not found.
    [[nodiscard]] std::optional<cell_location> searchReverse(std::u32string text,
                                                             cell_location searchPosition);
    [[nodiscard]] std::optional<cell_location> searchReverse(cell_location searchPosition);

    // Searches from current position the next item downwards.
    [[nodiscard]] std::optional<cell_location> search(std::u32string text,
                                                      cell_location searchPosition,
                                                      bool initiatedByDoubleClick = false);
    [[nodiscard]] std::optional<cell_location> search(cell_location searchPosition);

    bool setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick);
    void clearSearch();

    // Tests if the grid cell at the given location does contain a word delimiter.
    [[nodiscard]] bool wordDelimited(cell_location position) const noexcept;

    [[nodiscard]] std::tuple<std::u32string, cell_location_range> extractWordUnderCursor(
        cell_location position) const noexcept;

    Settings const& factorySettings() const noexcept { return _factorySettings; }
    Settings const& settings() const noexcept { return _settings; }
    Settings& settings() noexcept { return _settings; }

    // Renders current visual terminal state to the render buffer.
    //
    // @param output target render buffer to write the current visual state to.
    // @param includeSelection boolean to indicate whether or not to include colorize selection.
    void fillRenderBuffer(RenderBuffer& output, bool includeSelection); // <- acquires the lock

  private:
    void mainLoop();
    void fillRenderBufferInternal(RenderBuffer& output, bool includeSelection);
    LineCount fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, line_offset base);
    void updateIndicatorStatusLine();
    void updateCursorVisibilityState() const noexcept;
    void updateHoveringHyperlinkState();
    bool handleMouseSelection(Modifier modifier);

    /// Tests if the text selection should be extended by the given mouse position or not.
    ///
    /// @retval false if either no selection is available, selection is complete, or the new pixel position is
    /// not enough into the next grid cell yet
    /// @retval true otherwise
    bool shouldExtendSelectionByMouse(cell_location newPosition,
                                      pixel_coordinate pixelPosition) const noexcept;

    // Tests if the App mouse protocol is explicitly being bypassed by the user,
    // by pressing a special bypass modifier (usualy Shift).
    bool allowBypassAppMouseGrabViaModifier(Modifier modifier) const noexcept
    {
        return _settings.mouseProtocolBypassModifier != Modifier::None
               && modifier.contains(_settings.mouseProtocolBypassModifier);
    }

    bool allowPassMouseEventToApp(Modifier currentlyPressedModifier) const noexcept
    {
        return _state.inputGenerator.mouseProtocol().has_value() && allowInput()
               && !allowBypassAppMouseGrabViaModifier(currentlyPressedModifier);
    }

    template <typename BlinkerState>
    [[nodiscard]] std::pair<bool, std::chrono::steady_clock::time_point> nextBlinkState(
        BlinkerState blinker, std::chrono::steady_clock::time_point lastBlink) const noexcept
    {
        auto const passed = std::chrono::duration_cast<std::chrono::milliseconds>(_currentTime - lastBlink);
        if (passed < blinker.interval)
            return { blinker.state, lastBlink };
        return { !blinker.state, _currentTime };
    }

    // Reads from PTY.
    [[nodiscard]] Pty::ReadResult readFromPty();

    // Writes partially or all input data to the PTY buffer object and returns a string view to it.
    [[nodiscard]] std::string_view lockedWriteToPtyBuffer(std::string_view data);

    // private data
    //

    Events& _eventListener;

    // configuration state
    Settings _factorySettings;
    Settings _settings;
    TerminalState _state;

    // synchronization
    std::mutex mutable _outerLock;
    std::mutex mutable _innerLock;

    // terminal clock
    std::chrono::steady_clock::time_point _currentTime;

    // {{{ PTY and PTY read buffer management
    crispy::BufferObjectPool<char> _ptyBufferPool;
    crispy::BufferObjectPtr<char> _currentPtyBuffer;
    size_t _ptyReadBufferSize;
    std::unique_ptr<Pty> _pty;
    // }}}

    // {{{ mouse related state (helpers for detecting double/tripple clicks)
    std::chrono::steady_clock::time_point _lastClick {};
    unsigned int _speedClicks = 0;
    terminal::cell_location _currentMousePosition {}; // current mouse position
    terminal::pixel_coordinate _lastMousePixelPositionOnLeftClick {};
    bool _leftMouseButtonPressed = false; // tracks left-mouse button pressed state (used for cell selection).
    bool _respectMouseProtocol = true;    // shift-click can disable that, button release sets it back to true
    // }}}

    // {{{ blinking state helpers
    mutable std::chrono::steady_clock::time_point _lastCursorBlink;
    mutable unsigned _cursorBlinkState = 1;
    struct BlinkerState
    {
        bool state = false;
        std::chrono::milliseconds const interval; // NOLINT(readability-identifier-naming)
    };
    mutable BlinkerState _slowBlinker { false, std::chrono::milliseconds { 500 } };
    mutable BlinkerState _rapidBlinker { false, std::chrono::milliseconds { 300 } };
    mutable std::chrono::steady_clock::time_point _lastBlink;
    mutable std::chrono::steady_clock::time_point _lastRapidBlink;
    // }}}

    // {{{ Displays this terminal manages
    // clang-format off
    Screen<PrimaryScreenCell> _primaryScreen;
    Screen<AlternateScreenCell> _alternateScreen;
    Screen<StatusDisplayCell> _hostWritableStatusLineScreen;
    Screen<StatusDisplayCell> _indicatorStatusScreen;
    std::reference_wrapper<ScreenBase> _currentScreen;
    Viewport _viewport;
    TraceHandler _traceHandler;
    // clang-format on
    // }}}

    // {{{ selection states
    std::unique_ptr<Selection> _selection;
    struct SelectionHelper: public terminal::SelectionHelper
    {
        Terminal* terminal;
        explicit SelectionHelper(Terminal* self): terminal { self } {}
        [[nodiscard]] PageSize pageSize() const noexcept override;
        [[nodiscard]] bool wordDelimited(cell_location pos) const noexcept override;
        [[nodiscard]] bool wrappedLine(line_offset line) const noexcept override;
        [[nodiscard]] bool cellEmpty(cell_location pos) const noexcept override;
        [[nodiscard]] int cellWidth(cell_location pos) const noexcept override;
    };
    SelectionHelper _selectionHelper;
    // }}}

    // {{{ Render buffer state
    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    mutable std::atomic<uint64_t> _changes { 0 };
    bool _screenDirty = false; // TODO: just inc _changes and delete this instead.
    RefreshInterval _refreshInterval;
    RenderDoubleBuffer _renderBuffer {};
    std::atomic<uint64_t> _lastFrameID = 0;
    RenderPassHints _lastRenderPassHints {};
    // }}}

    InputMethodData _inputMethodData {};
    std::atomic<HyperlinkId> _hoveringHyperlinkId = HyperlinkId {};
    std::atomic<bool> _renderBufferUpdateEnabled = true; // for "Synchronized Updates" feature
    std::optional<HighlightRange> _highlightRange = std::nullopt;
};

} // namespace terminal

template <>
struct fmt::formatter<terminal::TraceHandler::PendingSequence>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(terminal::TraceHandler::PendingSequence const& pendingSequence, format_context& ctx)
        -> format_context::iterator
    {
        if (auto const* p = std::get_if<terminal::Sequence>(&pendingSequence))
            return fmt::format_to(ctx.out(), "{}", p->text());
        else if (auto const* p = std::get_if<terminal::TraceHandler::CodepointSequence>(&pendingSequence))
            return fmt::format_to(ctx.out(), "\"{}\"", crispy::escape(p->text));
        else if (auto const* p = std::get_if<char32_t>(&pendingSequence))
            return fmt::format_to(ctx.out(), "'{}'", unicode::convert_to<char>(*p));
        else
            crispy::unreachable();
    }
};
