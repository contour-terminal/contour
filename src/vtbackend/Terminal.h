// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/ColorPalette.h>
#include <vtbackend/Cursor.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/InputHandler.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Selector.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/SequenceBuilder.h>
#include <vtbackend/Settings.h>
#include <vtbackend/StatusLineBuilder.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/Viewport.h>
#include <vtbackend/cell/CellConcept.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>
#include <crispy/assert.h>
#include <crispy/defines.h>

#include <gsl/pointers>

#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <string_view>
#include <utility>

namespace vtbackend
{

template <CellConcept Cell>
class Screen;

class ScreenBase;

/// Helping information to visualize IME text that has not been comitted yet.
struct InputMethodData
{
    // If this string is non-empty, the IME is active and the given data
    // shall be displayed at the cursor's location.
    std::string preeditString;
};

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes
{
  public:
    void set(AnsiMode mode, bool enabled) { _ansi.set(static_cast<size_t>(mode), enabled); }

    bool set(DECMode mode, bool enabled)
    {
        if (_decFrozen[static_cast<size_t>(mode)])
        {
            errorLog()("Attempt to change frozen DEC mode {}. Ignoring.", mode);
            return false;
        }
        _dec.set(static_cast<size_t>(mode), enabled);
        return true;
    }

    [[nodiscard]] bool enabled(AnsiMode mode) const noexcept { return _ansi[static_cast<size_t>(mode)]; }
    [[nodiscard]] bool enabled(DECMode mode) const noexcept { return _dec[static_cast<size_t>(mode)]; }

    [[nodiscard]] bool frozen(DECMode mode) const noexcept
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));
        return _decFrozen.test(static_cast<size_t>(mode));
    }

    void freeze(DECMode mode)
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));

        if (mode == DECMode::BatchedRendering)
        {
            errorLog()("Attempt to freeze batched rendering. Ignoring.");
            return;
        }

        _decFrozen.set(static_cast<size_t>(mode), true);
        terminalLog()("Freezing {} DEC mode to permanently-{}.", mode, enabled(mode) ? "set" : "reset");
    }

    void unfreeze(DECMode mode)
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));
        _decFrozen.set(static_cast<size_t>(mode), false);
        terminalLog()("Unfreezing permanently-{} DEC mode {}.", mode, enabled(mode));
    }

    void save(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
            _savedModes[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
        {
            if (auto i = _savedModes.find(mode); i != _savedModes.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

  private:
    std::bitset<32> _ansi;                            // AnsiMode
    std::bitset<8452 + 1> _dec;                       // DECMode
    std::bitset<8452 + 1> _decFrozen;                 // DECMode
    std::map<DECMode, std::vector<bool>> _savedModes; // saved DEC modes
};
// }}}

struct Search
{
    std::u32string pattern;
    ScrollOffset initialScrollOffset {};
    bool initiatedByDoubleClick = false;
};

struct Prompt
{
    std::string prompt;
    std::string text;
};

// Mandates what execution mode the terminal will take to process VT sequences.
//
enum class ExecutionMode : uint8_t
{
    // Normal execution mode, with no tracing enabled.
    Normal,

    // Trace mode is enabled and waiting for command to continue execution.
    Waiting,

    // Tracing mode is enabled and execution is stopped after each VT sequence.
    SingleStep,

    // Tracing mode is enabled, execution is stopped after queue of pending VT sequences is empty.
    BreakAtEmptyQueue,

    // Trace mode is enabled and execution is stopped at frame marker.
    // TODO: BreakAtFrame,
};

enum class WrapPending : uint8_t
{
    Yes,
    No,
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
    void writeTextEnd() override;

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
    gsl::not_null<Terminal*> _terminal;
    PendingSequenceQueue _pendingSequences = {};
};

struct TabsInfo
{
    struct Tab
    {
        std::optional<std::string> name;
        Color color;
    };

    std::vector<Tab> tabs;
    size_t activeTabPosition = 1;
};

/// Terminal API to manage input and output devices of a pseudo terminal, such as keyboard, mouse, and screen.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) // TODO
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
        virtual void setFontDef(FontDef const& /*fontSpec*/) {}
        virtual void copyToClipboard(std::string_view /*data*/) {}
        virtual void openDocument(std::string_view /*fileOrUrl*/) = 0;
        virtual void inspect() {}
        virtual void notify(std::string_view /*title*/, std::string_view /*body*/) {}
        virtual void onClosed() {}
        virtual void pasteFromClipboard(unsigned /*count*/, bool /*strip*/) {}
        virtual void onSelectionCompleted() {}
        virtual void requestWindowResize(LineCount, ColumnCount) {}
        virtual void requestWindowResize(Width, Height) {}
        virtual void requestShowHostWritableStatusLine() {}
        virtual void setWindowTitle(std::string_view /*title*/) {}
        virtual void setTabName(std::string_view /*title*/) {}
        virtual void setTerminalProfile(std::string const& /*configProfileName*/) {}
        virtual void discardImage(Image const&) {}
        virtual void inputModeChanged(ViMode /*mode*/) {}
        virtual void updateHighlights() {}
        virtual void playSound(Sequence::Parameters const&) {}
        virtual void cursorPositionChanged() {}
        virtual void onScrollOffsetChanged(ScrollOffset) {}
    };

    class NullEvents: public Events
    {
      public:
        void requestCaptureBuffer(LineCount /*lines*/, bool /*logical*/) override {}
        void bell() override {}
        void bufferChanged(ScreenType) override {}
        void renderBufferUpdated() override {}
        void screenUpdated() override {}
        FontDef getFontDef() override { return {}; }
        void setFontDef(FontDef const& /*fontSpec*/) override {}
        void copyToClipboard(std::string_view /*data*/) override {}
        void openDocument(std::string_view /*fileOrUrl*/) override {}
        void inspect() override {}
        void notify(std::string_view /*title*/, std::string_view /*body*/) override {}
        void onClosed() override {}
        void pasteFromClipboard(unsigned /*count*/, bool /*strip*/) override {}
        void onSelectionCompleted() override {}
        void requestWindowResize(LineCount, ColumnCount) override {}
        void requestWindowResize(Width, Height) override {}
        void requestShowHostWritableStatusLine() override {}
        void setWindowTitle(std::string_view /*title*/) override {}
        void setTabName(std::string_view /*title*/) override {}
        void setTerminalProfile(std::string const& /*configProfileName*/) override {}
        void discardImage(Image const&) override {}
        void inputModeChanged(ViMode /*mode*/) override {}
        void updateHighlights() override {}
        void playSound(Sequence::Parameters const&) override {}
        void cursorPositionChanged() override {}
        void onScrollOffsetChanged(ScrollOffset) override {}
    };

    Terminal(Events& eventListener,
             std::unique_ptr<vtpty::Pty> pty,
             Settings factorySettings,
             std::chrono::steady_clock::time_point now /* = std::chrono::steady_clock::now()*/);
    ~Terminal() = default;

    void start();

    void setRefreshRate(RefreshRate refreshRate);
    void setLastMarkRangeOffset(LineOffset value) noexcept;

    void setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount);
    LineCount maxHistoryLineCount() const noexcept;

    void setTerminalId(VTType id) noexcept;
    VTType terminalId() const noexcept { return _terminalId; }

    void setMaxImageSize(ImageSize size) noexcept { _effectiveImageCanvasSize = size; }
    ImageSize maxImageSize() const noexcept { return _effectiveImageCanvasSize; }

    void setMaxImageSize(ImageSize effective, ImageSize limit)
    {
        _effectiveImageCanvasSize = effective;
        _settings.maxImageSize = limit;
    }

    // {{{ Modes handling
    bool isModeEnabled(AnsiMode m) const noexcept { return _modes.enabled(m); }
    bool isModeEnabled(DECMode m) const noexcept { return _modes.enabled(m); }
    void setMode(AnsiMode mode, bool enable);
    void setMode(DECMode mode, bool enable);
    void saveModes(std::vector<DECMode> const& modes) { _modes.save(modes); }
    void restoreModes(std::vector<DECMode> const& modes) { _modes.restore(modes); }
    void freezeMode(DECMode mode, bool enable)
    {
        setMode(mode, enable);
        _modes.freeze(mode);
    }
    void unfreezeMode(DECMode mode) { _modes.unfreeze(mode); }
    // }}}

    void setTopBottomMargin(std::optional<LineOffset> top, std::optional<LineOffset> bottom);
    void setLeftRightMargin(std::optional<ColumnOffset> left, std::optional<ColumnOffset> right);

    void moveCursorTo(LineOffset line, ColumnOffset column);

    void setGraphicsRendition(GraphicsRendition rendition);
    void setForegroundColor(Color color);
    void setBackgroundColor(Color color);
    void setUnderlineColor(Color color);
    void setHighlightRange(HighlightRange range);

    // {{{ cursor
    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    // [[nodiscard]] CellLocation clampToOrigin(CellLocation coord) const noexcept
    // {
    //     return { std::clamp(coord.line, LineOffset { 0 }, _margin.vertical.to),
    //              std::clamp(coord.column, ColumnOffset { 0 }, _margin.horizontal.to) };
    // }

    [[nodiscard]] LineOffset clampedLine(LineOffset line) const noexcept
    {
        return std::clamp(line, LineOffset(0), boxed_cast<LineOffset>(_settings.pageSize.lines) - 1);
    }

    [[nodiscard]] ColumnOffset clampedColumn(ColumnOffset column) const noexcept
    {
        return std::clamp(column, ColumnOffset(0), boxed_cast<ColumnOffset>(_settings.pageSize.columns) - 1);
    }

    [[nodiscard]] CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return { .line = clampedLine(coord.line), .column = clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] constexpr bool contains(CellLocation coord) const noexcept
    {
        return LineOffset(0) <= coord.line && coord.line < boxed_cast<LineOffset>(_settings.pageSize.lines)
               && ColumnOffset(0) <= coord.column
               && coord.column <= boxed_cast<ColumnOffset>(_settings.pageSize.columns);
    }

    [[nodiscard]] bool isCursorInViewport() const noexcept
    {
        return viewport().isLineVisible(currentScreen().cursor().position.line);
    }
    // }}}

    [[nodiscard]] constexpr ImageSize cellPixelSize() const noexcept { return _cellPixelSize; }
    constexpr void setCellPixelSize(ImageSize cellPixelSize) { _cellPixelSize = cellPixelSize; }

    /// Retrieves the time point this terminal instance has been spawned.
    [[nodiscard]] std::chrono::steady_clock::time_point currentTime() const noexcept { return _currentTime; }

    /// Retrieves reference to the underlying PTY device.
    [[nodiscard]] vtpty::Pty& device() noexcept { return *_pty; }
    [[nodiscard]] vtpty::Pty const& device() const noexcept { return *_pty; }

    [[nodiscard]] PageSize pageSize() const noexcept { return _pty->pageSize(); }

    [[nodiscard]] PageSize totalPageSize() const noexcept { return _settings.pageSize; }

    [[nodiscard]] ImageSize pixelSize() const noexcept { return cellPixelSize() * pageSize(); }

    // Returns number of lines for the currently displayed status line,
    // or 0 if status line is currently not displayed.
    [[nodiscard]] LineCount statusLineHeight() const noexcept
    {
        switch (_statusDisplayType)
        {
            case StatusDisplayType::None: return LineCount(0);
            case StatusDisplayType::Indicator: return _indicatorStatusScreen.pageSize().lines;
            case StatusDisplayType::HostWritable: return _hostWritableStatusLineScreen.pageSize().lines;
        }
        crispy::unreachable();
    }

    /// Resizes the terminal screen to the given amount of grid cells with their pixel dimensions.
    /// Important! In case a status line is currently visible, the status line count is being
    /// accumulated into the screen size, too.
    void resizeScreen(PageSize totalPageSize, std::optional<ImageSize> pixels = std::nullopt);

    void clearScreen();

    void setMouseProtocolBypassModifiers(Modifiers value) { _settings.mouseProtocolBypassModifiers = value; }
    void setMouseBlockSelectionModifiers(Modifiers value) { _settings.mouseBlockSelectionModifiers = value; }

    // {{{ input proxy
    using Timestamp = std::chrono::steady_clock::time_point;
    Handled sendKeyEvent(Key key, Modifiers modifiers, KeyboardEventType eventType, Timestamp now);
    Handled sendCharEvent(
        char32_t ch, uint32_t physicalKey, Modifiers modifiers, KeyboardEventType eventType, Timestamp now);
    Handled sendMousePressEvent(Modifiers modifiers,
                                MouseButton button,
                                PixelCoordinate pixelPosition,
                                bool uiHandledHint);
    void sendMouseMoveEvent(Modifiers modifiers,
                            CellLocation newPosition,
                            PixelCoordinate pixelPosition,
                            bool uiHandledHint);
    Handled sendMouseReleaseEvent(Modifiers modifiers,
                                  MouseButton button,
                                  PixelCoordinate pixelPosition,
                                  bool uiHandledHint);
    bool sendFocusInEvent();
    bool sendFocusOutEvent();
    void sendPaste(std::string_view text); // Sends verbatim text in bracketed mode to application.
    void sendPasteFromClipboard(unsigned count, bool strip)
    {
        _eventListener.pasteFromClipboard(count, strip);
    }
    void sendRawInput(std::string_view text);

    void inputModeChanged(ViMode mode) { _eventListener.inputModeChanged(mode); }
    void updateHighlights() { _eventListener.updateHighlights(); }
    void playSound(vtbackend::Sequence::Parameters const& params) { _eventListener.playSound(params); }

    bool applicationCursorKeys() const noexcept { return _inputGenerator.applicationCursorKeys(); }
    bool applicationKeypad() const noexcept { return _inputGenerator.applicationKeypad(); }

    bool hasInput() const noexcept;
    void flushInput();

    std::string_view peekInput() const noexcept { return _inputGenerator.peek(); }
    // }}}

    /// Writes a given VT-sequence to screen.
    void writeToScreen(std::string_view vtStream);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    void writeToScreenInternal(std::string_view vtStream);

    /// Writes a given VT-sequence to screen - but without acquiring the lock (must be already acquired).
    /// This version of the function is used to write to the status line and should not be used by the shell.
    void writeToScreenInternal(Screen<StatusDisplayCell>& screen, std::string_view vtStream);

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

    void lock() const { _stateMutex.lock(); }
    void unlock() const { _stateMutex.unlock(); }

    [[nodiscard]] ColorPalette const& colorPalette() const noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& colorPalette() noexcept { return _colorPalette; }
    [[nodiscard]] ColorPalette& defaultColorPalette() noexcept { return _defaultColorPalette; }

    [[nodiscard]] std::vector<ColorPalette> const& savedColorPalettes() const noexcept
    {
        return _savedColorPalettes;
    }

    void setColorPalette(ColorPalette const& palette) noexcept;
    void resetColorPalette() noexcept { setColorPalette(defaultColorPalette()); }
    void resetColorPalette(ColorPalette const& colors);

    void pushColorPalette(size_t slot);
    void popColorPalette(size_t slot);
    void reportColorPaletteStack();

    [[nodiscard]] ScreenBase& currentScreen() noexcept { return *_currentScreen; }
    [[nodiscard]] ScreenBase const& currentScreen() const noexcept { return *_currentScreen; }

    [[nodiscard]] ScreenBase& activeDisplay() noexcept
    {
        switch (_activeStatusDisplay)
        {
            case ActiveStatusDisplay::Main: return *_currentScreen;
            case ActiveStatusDisplay::StatusLine: return _hostWritableStatusLineScreen;
            case ActiveStatusDisplay::IndicatorStatusLine: return _indicatorStatusScreen;
        }
        crispy::unreachable();
    }

    [[nodiscard]] SequenceHandler& sequenceHandler() noexcept
    {
        // TODO: avoid double-switch by introducing a `SequenceHandler& sequenceHandler` member.
        switch (_executionMode.load())
        {
            case ExecutionMode::Normal: return activeDisplay();
            case ExecutionMode::BreakAtEmptyQueue:
            case ExecutionMode::Waiting: [[fallthrough]];
            case ExecutionMode::SingleStep: return _traceHandler;
        }
        crispy::unreachable();
    }

    bool isPrimaryScreen() const noexcept { return _currentScreenType == ScreenType::Primary; }
    bool isAlternateScreen() const noexcept { return _currentScreenType == ScreenType::Alternate; }
    ScreenType screenType() const noexcept { return _currentScreenType; }
    void setScreen(ScreenType screenType);

    ScreenBase& screenForType(ScreenType type) noexcept
    {
        switch (type)
        {
            case ScreenType::Primary: return _primaryScreen;
            case ScreenType::Alternate: return _alternateScreen;
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

    [[nodiscard]] bool isLineWrapped(LineOffset lineNumber) const noexcept
    {
        return isPrimaryScreen() && _primaryScreen.isLineWrapped(lineNumber);
    }

    [[nodiscard]] CellLocation currentMousePosition() const noexcept { return _currentMousePosition; }

    [[nodiscard]] std::optional<CellLocation> currentMouseGridPosition() const noexcept
    {
        if (_currentScreen->contains(_currentMousePosition))
            return _viewport.translateScreenToGridCoordinate(_currentMousePosition);
        return std::nullopt;
    }

    [[nodiscard]] CellLocation normalModeCursorPosition() const noexcept
    {
        return _viCommands.cursorPosition;
    }
    void moveNormalModeCursorTo(CellLocation pos) noexcept { _viCommands.moveCursorTo(pos); }
    void addLineOffsetToJumpHistory(LineOffset offset) noexcept
    {
        _viCommands.addLineOffsetToJumpHistory(offset);
    }

    // {{{ cursor management
    CursorDisplay cursorDisplay() const noexcept { return _settings.cursorDisplay; }
    void setCursorDisplay(CursorDisplay display);

    CursorShape cursorShape() const noexcept { return _settings.cursorShape; }
    void setCursorShape(CursorShape shape);

    bool cursorBlinkActive() const noexcept { return _cursorBlinkState; }

    bool cursorCurrentlyVisible() const noexcept
    {
        return isModeEnabled(DECMode::VisibleCursor)
               && (cursorDisplay() == CursorDisplay::Steady || _cursorBlinkState);
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
    void setWordDelimiters(std::string const& wordDelimiters);
    void setExtendedWordDelimiters(std::string const& wordDelimiters);
    std::u32string const& wordDelimiters() const noexcept { return _settings.wordDelimiters; }

    Selection const* selector() const noexcept { return _selection.get(); }
    Selection* selector() noexcept { return _selection.get(); }
    std::chrono::milliseconds highlightTimeout() const noexcept { return _settings.highlightTimeout; }

    void updateSelectionMatches();

    template <typename RenderTarget>
    void renderSelection(RenderTarget renderTarget) const
    {
        if (!_selection)
            return;

        if (isPrimaryScreen())
            vtbackend::renderSelection(*_selection,
                                       [&](CellLocation pos) { renderTarget(pos, _primaryScreen.at(pos)); });
        else
            vtbackend::renderSelection(
                *_selection, [&](CellLocation pos) { renderTarget(pos, _alternateScreen.at(pos)); });
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
    bool isSelected(CellLocation coord) const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting && _selection->contains(coord);
    }

    /// Tests whether given line offset is intersecting with selection.
    bool isSelected(LineOffset line) const noexcept
    {
        return _selection && _selection->state() != Selection::State::Waiting
               && _selection->containsLine(line);
    }

    bool isHighlighted(CellLocation cell) const noexcept;
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

    HyperlinkStorage& hyperlinks() noexcept { return _hyperlinks; }
    HyperlinkStorage const& hyperlinks() const noexcept { return _hyperlinks; }

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
            return _currentScreen->hyperlinkAt(*gridPosition);
        return {};
    }

    [[nodiscard]] ExecutionMode executionMode() const noexcept { return _executionMode; }
    void setExecutionMode(ExecutionMode mode);

    bool processInputOnce();

    void markScreenDirty() noexcept { _screenDirty = true; }

    [[nodiscard]] uint64_t lastFrameID() const noexcept { return _lastFrameID.load(); }

    // Screen's EventListener implementation
    //
    void requestCaptureBuffer(LineCount lines, bool logical);
    void requestShowHostWritableStatusLine();
    void bell();
    void bufferChanged(ScreenType);
    void scrollbackBufferCleared();
    void screenUpdated();
    void renderBufferUpdated();
    [[nodiscard]] FontDef getFontDef();
    void setFontDef(FontDef const& fontDef);
    void copyToClipboard(std::string_view data);
    void openDocument(std::string_view data);
    void inspect();
    void notify(std::string_view title, std::string_view body);
    void reply(std::string_view text);

    template <typename... Ts>
    void reply(std::string_view message, Ts const&... args)
    {
#if defined(__APPLE__) || defined(_MSC_VER)
        reply(std::vformat(message, std::make_format_args(args...)));
#else
        reply(std::vformat(message, std::make_format_args(args...)));
#endif
    }

    void requestWindowResize(PageSize);
    void requestWindowResize(ImageSize);
    void setApplicationkeypadMode(bool enabled);
    void setBracketedPaste(bool enabled);
    void setCursorStyle(CursorDisplay display, CursorShape shape);
    void setCursorVisibility(bool visible);
    void setGenerateFocusEvents(bool enabled);
    void setMouseProtocol(MouseProtocol protocol, bool enabled);
    void setMouseTransport(MouseTransport transport);
    void setMouseWheelMode(InputGenerator::MouseWheelMode mode);
    void setWindowTitle(std::string_view title);
    void setTabName(std::string_view title);
    [[nodiscard]] std::string const& windowTitle() const noexcept;
    [[nodiscard]] std::optional<std::string> tabName() const noexcept;
    [[nodiscard]] bool focused() const noexcept { return _focused; }
    [[nodiscard]] Search& search() noexcept { return _search; }
    [[nodiscard]] Search const& search() const noexcept { return _search; }
    [[nodiscard]] Prompt& prompt() noexcept { return _prompt; }
    [[nodiscard]] Prompt const& prompt() const noexcept { return _prompt; }
    void saveWindowTitle();
    void restoreWindowTitle();
    void setTerminalProfile(std::string const& configProfileName);
    void useApplicationCursorKeys(bool enabled);
    void softReset();
    void hardReset();
    void forceRedraw(std::function<void()> const& artificialSleep);
    void discardImage(Image const&);
    void markCellDirty(CellLocation position) noexcept;
    void markRegionDirty(Rect area) noexcept;
    void synchronizedOutput(bool enabled);
    void onBufferScrolled(LineCount n) noexcept;

    void onViewportChanged();

    /// @returns either an empty string or a file:// URL of the last set working directory.
    [[nodiscard]] std::string const& currentWorkingDirectory() const noexcept
    {
        return _currentWorkingDirectory;
    }

    void setCurrentWorkingDirectory(std::string text) { _currentWorkingDirectory = std::move(text); }

    void verifyState();

    void applyPageSizeToCurrentBuffer();
    void applyPageSizeToMainDisplay(ScreenType screenType);

    [[nodiscard]] crispy::buffer_object_ptr<char> currentPtyBuffer() const noexcept
    {
        return _currentPtyBuffer;
    }

    [[nodiscard]] vtbackend::SelectionHelper& selectionHelper() noexcept { return _selectionHelper; }

    [[nodiscard]] Selection::OnSelectionUpdated selectionUpdatedHelper()
    {
        return [this]() {
            onSelectionUpdated();
        };
    }

    void onSelectionUpdated();

    [[nodiscard]] ViInputHandler& inputHandler() noexcept { return _inputHandler; }
    [[nodiscard]] ViInputHandler const& inputHandler() const noexcept { return _inputHandler; }

    [[nodiscard]] ExtendedKeyboardInputGenerator& keyboardProtocol() noexcept
    {
        return _inputGenerator.keyboardProtocol();
    }

    void resetHighlight();

    StatusDisplayType statusDisplayType() const noexcept { return _statusDisplayType; }
    void setStatusDisplay(StatusDisplayType statusDisplayType);
    void setActiveStatusDisplay(ActiveStatusDisplay activeDisplay);
    constexpr ActiveStatusDisplay activeStatusDisplay() const noexcept { return _activeStatusDisplay; }

    void pushStatusDisplay(StatusDisplayType statusDisplayType);
    void popStatusDisplay();

    bool allowInput() const noexcept { return !isModeEnabled(AnsiMode::KeyboardAction); }

    void setAllowInput(bool enabled);

    // Sets the current search term to the given text and
    // moves the viewport accordingly to make sure the given text is visible,
    // or it will not move at all if the input text was not found.
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string text, CellLocation searchPosition);
    [[nodiscard]] std::optional<CellLocation> searchReverse(CellLocation searchPosition);

    // Searches from current position the next item downwards.
    [[nodiscard]] std::optional<CellLocation> search(CellLocation searchPosition);

    [[nodiscard]] std::optional<CellLocation> searchNextMatch(CellLocation cursorPosition);
    [[nodiscard]] std::optional<CellLocation> searchPrevMatch(CellLocation cursorPosition);

    bool setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick);
    void clearSearch();

    void setPrompt(std::string prompt);
    void setPromptText(std::string text);

    // Tests if the grid cell at the given location does contain a word delimiter.
    [[nodiscard]] bool wordDelimited(CellLocation position) const noexcept;
    [[nodiscard]] bool wordDelimited(CellLocation position,
                                     std::u32string_view wordDelimiters) const noexcept;

    [[nodiscard]] std::tuple<std::u32string, CellLocationRange> extractWordUnderCursor(
        CellLocation position) const noexcept;

    [[nodiscard]] Settings& factorySettings() noexcept { return _factorySettings; }
    [[nodiscard]] Settings const& factorySettings() const noexcept { return _factorySettings; }
    [[nodiscard]] Settings const& settings() const noexcept { return _settings; }
    [[nodiscard]] Settings& settings() noexcept { return _settings; }

    // Renders current visual terminal state to the render buffer.
    //
    // @param output target render buffer to write the current visual state to.
    // @param includeSelection boolean to indicate whether or not to include colorize selection.
    void fillRenderBuffer(RenderBuffer& output, bool includeSelection); // <- acquires the lock

    [[nodiscard]] gsl::span<Function const> activeSequences() const noexcept
    {
        return _supportedVTSequences.activeSequences();
    }

    // {{{ VT parser related

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept;

    [[nodiscard]] TraceHandler const& traceHandler() const noexcept { return _traceHandler; }

    [[nodiscard]] constexpr auto const& parser() const noexcept { return _parser; }
    [[nodiscard]] constexpr auto& parser() noexcept { return _parser; }

    [[nodiscard]] bool usingStdoutFastPipe() const noexcept { return _usingStdoutFastPipe; }

    void hookParser(std::unique_ptr<ParserExtension> parserExtension) noexcept
    {
        _sequenceBuilder.hookParser(std::move(parserExtension));
    }

    constexpr void resetInstructionCounter() noexcept { _instructionCounter = 0; }
    constexpr void incrementInstructionCounter(size_t n = 1) noexcept { _instructionCounter += n; }
    [[nodiscard]] constexpr uint64_t instructionCounter() const noexcept { return _instructionCounter; }
    // }}}

    std::vector<ColumnOffset>& tabs() noexcept { return _tabs; }
    std::vector<ColumnOffset> const& tabs() const noexcept { return _tabs; }

    ImagePool& imagePool() noexcept { return _imagePool; }
    ImagePool const& imagePool() const noexcept { return _imagePool; }

    bool syncWindowTitleWithHostWritableStatusDisplay() const noexcept
    {
        return _syncWindowTitleWithHostWritableStatusDisplay;
    }

    void setSyncWindowTitleWithHostWritableStatusDisplay(bool value) noexcept
    {
        _syncWindowTitleWithHostWritableStatusDisplay = value;
    }

    [[nodiscard]] std::optional<StatusDisplayType> savedStatusDisplayType() const noexcept
    {
        return _savedStatusDisplayType;
    }

    void setSavedStatusDisplayType(std::optional<StatusDisplayType> value) noexcept
    {
        _savedStatusDisplayType = value;
    }

    // {{{ Sixel image configuration
    void setMaxSixelColorRegisters(unsigned value) noexcept { _maxSixelColorRegisters = value; }
    [[nodiscard]] unsigned maxSixelColorRegisters() const noexcept { return _maxSixelColorRegisters; }
    std::shared_ptr<SixelColorPalette>& sixelColorPalette() noexcept { return _sixelColorPalette; }
    [[nodiscard]] bool usePrivateColorRegisters() const noexcept { return _usePrivateColorRegisters; }
    // }}}

    void triggerWordWiseSelectionWithCustomDelimiters(std::string const& delimiters);

    void setStatusLineDefinition(StatusLineDefinition&& definition);
    void resetStatusLineDefinition();

    TabsInfo guiTabsInfoForStatusLine() const noexcept { return _guiTabInfoForStatusLine; }
    void setGuiTabInfoForStatusLine(TabsInfo&& info) { _guiTabInfoForStatusLine = std::move(info); }

    TabsNamingMode getTabsNamingMode() const noexcept { return _settings.tabNamingMode; }
    void requestTabName();

  private:
    void mainLoop();
    void fillRenderBufferInternal(RenderBuffer& output, bool includeSelection);
    LineCount fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, LineOffset base);
    void updateIndicatorStatusLine();
    void updateCursorVisibilityState() const noexcept;
    void updateHoveringHyperlinkState();

    struct TheSelectionHelper: public vtbackend::SelectionHelper
    {
        Terminal* terminal;
        explicit TheSelectionHelper(Terminal* self): terminal { self } {}
        [[nodiscard]] PageSize pageSize() const noexcept override;
        [[nodiscard]] bool wrappedLine(LineOffset line) const noexcept override;
        [[nodiscard]] bool cellEmpty(CellLocation pos) const noexcept override;
        [[nodiscard]] int cellWidth(CellLocation pos) const noexcept override;
    };
    void triggerWordWiseSelection(CellLocation startPos, TheSelectionHelper const& selectionHelper);
    bool handleMouseSelection(Modifiers modifiers);

    /// Tests if the text selection should be extended by the given mouse position or not.
    ///
    /// @retval false if either no selection is available, selection is complete, or the new pixel position is
    /// not enough into the next grid cell yet
    /// @retval true otherwise
    bool shouldExtendSelectionByMouse(CellLocation newPosition, PixelCoordinate pixelPosition) const noexcept;

    // Tests if the App mouse protocol is explicitly being bypassed by the user,
    // by pressing a special bypass modifier (usualy Shift).
    bool allowBypassAppMouseGrabViaModifier(Modifiers modifiers) const noexcept
    {
        return _settings.mouseProtocolBypassModifiers != Modifier::None
               && modifiers.contains(_settings.mouseProtocolBypassModifiers);
    }

    bool allowPassMouseEventToApp(Modifiers currentlyPressedModifier) const noexcept
    {
        return _inputGenerator.mouseProtocol().has_value() && allowInput()
               && !allowBypassAppMouseGrabViaModifier(currentlyPressedModifier)
               && _inputHandler.mode() == ViMode::Insert;
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
    [[nodiscard]] std::optional<vtpty::Pty::ReadResult> readFromPty();

    // Writes partially or all input data to the PTY buffer object and returns a string view to it.
    [[nodiscard]] std::string_view lockedWriteToPtyBuffer(std::string_view data);

    // private data
    //

    Events& _eventListener;

    // configuration state
    Settings _factorySettings;
    Settings _settings;

    // synchronization
    std::mutex mutable _stateMutex;

    // terminal clock
    std::chrono::steady_clock::time_point _currentTime;

    // {{{ PTY and PTY read buffer management
    crispy::buffer_object_pool<char> _ptyBufferPool;
    crispy::buffer_object_ptr<char> _currentPtyBuffer;
    size_t _ptyReadBufferSize;
    std::unique_ptr<vtpty::Pty> _pty;
    // }}}

    // {{{ mouse related state (helpers for detecting double/tripple clicks)
    std::chrono::steady_clock::time_point _lastClick {};
    unsigned int _speedClicks = 0;
    vtbackend::CellLocation _currentMousePosition {}; // current mouse position
    vtbackend::PixelCoordinate _lastMousePixelPositionOnLeftClick {};
    bool _leftMouseButtonPressed = false; // tracks left-mouse button pressed state (used for cell selection).
    bool _respectMouseProtocol = true;    // shift-click can disable that, button release sets it back to true
    // }}}

    // {{{ blinking state helpers
    mutable std::chrono::steady_clock::time_point _lastCursorBlink;
    mutable unsigned _cursorBlinkState = 1;
    struct BlinkerState
    {
        bool state = false;
        std::chrono::milliseconds interval {};
    };
    mutable BlinkerState _slowBlinker { false, std::chrono::milliseconds { 500 } };
    mutable BlinkerState _rapidBlinker { false, std::chrono::milliseconds { 300 } };
    mutable std::chrono::steady_clock::time_point _lastBlink;
    mutable std::chrono::steady_clock::time_point _lastRapidBlink;
    // }}}

    // {{{ Displays this terminal manages
    Screen<PrimaryScreenCell> _primaryScreen;
    Screen<AlternateScreenCell> _alternateScreen;
    Screen<StatusDisplayCell> _hostWritableStatusLineScreen;
    Screen<StatusDisplayCell> _indicatorStatusScreen;
    gsl::not_null<ScreenBase*> _currentScreen;
    Viewport _viewport;
    StatusLineDefinition _indicatorStatusLineDefinition;

    // {{{ tabs info
    TabsInfo _guiTabInfoForStatusLine;
    // }}}

    // {{{ selection states
    std::unique_ptr<Selection> _selection;
    TheSelectionHelper _selectionHelper;
    TheSelectionHelper _extendedSelectionHelper;
    TheSelectionHelper _customSelectionHelper;
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
    SupportedSequences _supportedVTSequences;

    // Execution Trace mode
    std::atomic<ExecutionMode> _executionMode = ExecutionMode::Normal;
    std::mutex _breakMutex;
    std::condition_variable _breakCondition;
    TraceHandler _traceHandler;

    /// contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.
    ImageSize _cellPixelSize;

    ColorPalette _defaultColorPalette;
    ColorPalette _colorPalette;
    std::vector<ColorPalette> _savedColorPalettes;
    size_t _lastSavedColorPalette = 0;

    bool _focused = true;

    VTType _terminalId = VTType::VT525;

    Modes _modes;
    std::map<DECMode, std::vector<bool>> _savedModes; //!< saved DEC modes

    // Screen margin - shared across all screens that are covering the main area,
    // i.e. the primary screen and alternate screen.
    // This excludes all status lines, title lines, etc.
    Margin _mainScreenMargin;
    Margin _hostWritableScreenMargin;
    Margin _indicatorScreenMargin;

    unsigned _maxSixelColorRegisters = 256;
    ImageSize _effectiveImageCanvasSize;
    std::shared_ptr<SixelColorPalette> _sixelColorPalette;
    ImagePool _imagePool;

    std::vector<ColumnOffset> _tabs;

    ScreenType _currentScreenType = ScreenType::Primary;
    StatusDisplayType _statusDisplayType = StatusDisplayType::None;
    bool _syncWindowTitleWithHostWritableStatusDisplay = false;
    std::optional<StatusDisplayType> _savedStatusDisplayType = std::nullopt;
    ActiveStatusDisplay _activeStatusDisplay = ActiveStatusDisplay::Main;

    Search _search;
    Prompt _prompt;

    CursorDisplay _cursorDisplay = CursorDisplay::Steady;
    CursorShape _cursorShape = CursorShape::Block;

    std::string _currentWorkingDirectory = {};

    unsigned _maxImageRegisterCount = 256;
    bool _usePrivateColorRegisters = false;

    bool _usingStdoutFastPipe = false;

    // Hyperlink related
    //
    HyperlinkStorage _hyperlinks {};

    std::string _windowTitle {};
    std::stack<std::string> _savedWindowTitles {};

    std::optional<std::string> _tabName {};

    struct ModeDependantSequenceHandler
    {
        Terminal& terminal;
        void executeControlCode(char controlCode)
        {
            terminal.sequenceHandler().executeControlCode(controlCode);
        }
        void processSequence(Sequence const& sequence)
        {
            terminal.sequenceHandler().processSequence(sequence);
        }
        void writeText(char32_t codepoint) { terminal.sequenceHandler().writeText(codepoint); }
        void writeText(std::string_view codepoints, size_t cellCount)
        {
            terminal.sequenceHandler().writeText(codepoints, cellCount);
        }
        void writeTextEnd() { terminal.sequenceHandler().writeTextEnd(); }
        [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept
        {
            return terminal.maxBulkTextSequenceWidth();
        }
    };

    struct TerminalInstructionCounter
    {
        Terminal& terminal;
        void operator()() noexcept { terminal.incrementInstructionCounter(); }
        void operator()(size_t increment) noexcept { terminal.incrementInstructionCounter(increment); }
    };

    using StandardSequenceBuilder = SequenceBuilder<ModeDependantSequenceHandler, TerminalInstructionCounter>;

    StandardSequenceBuilder _sequenceBuilder;
    vtparser::Parser<StandardSequenceBuilder, false> _parser;
    uint64_t _instructionCounter = 0;

    InputGenerator _inputGenerator {};

    ViCommands _viCommands;
    ViInputHandler _inputHandler;
};

} // namespace vtbackend

// {{{ fmt formatter specializations
template <>
struct std::formatter<vtbackend::TraceHandler::PendingSequence>: std::formatter<std::string>
{
    auto format(vtbackend::TraceHandler::PendingSequence const& pendingSequence, auto& ctx) const
    {
        std::string value;
        if (auto const* p = std::get_if<vtbackend::Sequence>(&pendingSequence))
            value = std::format("{}", p->text());
        else if (auto const* p = std::get_if<vtbackend::TraceHandler::CodepointSequence>(&pendingSequence))
            value = std::format("\"{}\"", crispy::escape(p->text));
        else if (auto const* p = std::get_if<char32_t>(&pendingSequence))
            value = std::format("'{}'", unicode::convert_to<char>(*p));
        else
            crispy::unreachable();

        return formatter<std::string>::format(value, ctx);
    }
};

template <>
struct std::formatter<vtbackend::AnsiMode>: std::formatter<std::string>
{
    auto format(vtbackend::AnsiMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct std::formatter<vtbackend::DECMode>: std::formatter<std::string>
{
    auto format(vtbackend::DECMode mode, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct std::formatter<vtbackend::DynamicColorName>: formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(vtbackend::DynamicColorName value, FormatContext& ctx) const
    {
        using vtbackend::DynamicColorName;
        string_view name;
        switch (value)
        {
            case DynamicColorName::DefaultForegroundColor: name = "DefaultForegroundColor"; break;
            case DynamicColorName::DefaultBackgroundColor: name = "DefaultBackgroundColor"; break;
            case DynamicColorName::TextCursorColor: name = "TextCursorColor"; break;
            case DynamicColorName::MouseForegroundColor: name = "MouseForegroundColor"; break;
            case DynamicColorName::MouseBackgroundColor: name = "MouseBackgroundColor"; break;
            case DynamicColorName::HighlightForegroundColor: name = "HighlightForegroundColor"; break;
            case DynamicColorName::HighlightBackgroundColor: name = "HighlightBackgroundColor"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ExecutionMode>: formatter<std::string_view>
{
    auto format(vtbackend::ExecutionMode value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ExecutionMode::Normal: name = "NORMAL"; break;
            case vtbackend::ExecutionMode::Waiting: name = "WAITING"; break;
            case vtbackend::ExecutionMode::SingleStep: name = "SINGLE STEP"; break;
            case vtbackend::ExecutionMode::BreakAtEmptyQueue: name = "BREAK AT EMPTY"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
