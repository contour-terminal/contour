// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/Functions.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/RenderBufferBuilder.h>
#include <vtbackend/SequenceBuilder.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <vtpty/MockPty.h>

#include <crispy/assert.h>
#include <crispy/escape.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <gsl/pointers>

#include <sys/types.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using crispy::size;
using std::nullopt;
using std::optional;
using std::string;
using std::string_view;
using std::u32string;
using std::u32string_view;

namespace chrono = std::chrono;

using namespace std::placeholders;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace vtbackend
{

namespace // {{{ helpers
{
    constexpr size_t MaxColorPaletteSaveStackSize = 10;

    void trimSpaceRight(string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    }

#if defined(CONTOUR_PERF_STATS)
    void logRenderBufferSwap(bool success, uint64_t frameID)
    {
        if (!renderBufferLog)
            return;

        if (success)
            renderBufferLog()("Render buffer {} swapped.", frameID);
        else
            renderBufferLog()("Render buffer {} swapping failed.", frameID);
    }
#endif

    int makeSelectionTypeId(Selection const& selection) noexcept
    {
        if (dynamic_cast<LinearSelection const*>(&selection))
            return 1;

        if (dynamic_cast<WordWiseSelection const*>(&selection))
            // To the application, this is nothing more than a linear selection.
            return 1;

        if (dynamic_cast<FullLineSelection const*>(&selection))
            return 2;

        if (dynamic_cast<RectangularSelection const*>(&selection))
            return 3;

        assert(false && "Invalid code path. Should never be reached.");
        return 0;
    }

    constexpr CellLocation raiseToMinimum(CellLocation location, LineOffset minimumLine) noexcept
    {
        return CellLocation { .line = std::max(location.line, minimumLine), .column = location.column };
    }

} // namespace
// }}}

Terminal::Terminal(Events& eventListener,
                   std::unique_ptr<vtpty::Pty> pty,
                   Settings factorySettings,
                   chrono::steady_clock::time_point now):
    _eventListener { eventListener },
    _factorySettings { std::move(factorySettings) },
    _settings { _factorySettings },
    _currentTime { now },
    _ptyBufferPool { crispy::nextPowerOfTwo(_settings.ptyBufferObjectSize) },
    _currentPtyBuffer { _ptyBufferPool.allocateBufferObject() },
    _ptyReadBufferSize { crispy::nextPowerOfTwo(_settings.ptyReadBufferSize) },
    _pty { std::move(pty) },
    _lastCursorBlink { now },
    _primaryScreen { *this,
                     &_mainScreenMargin,
                     _settings.pageSize,
                     _settings.primaryScreen.allowReflowOnResize,
                     _settings.maxHistoryLineCount,
                     "primary" },
    _alternateScreen { *this, &_mainScreenMargin, _settings.pageSize, false, LineCount(0), "alternate" },
    _hostWritableStatusLineScreen { *this,
                                    &_hostWritableScreenMargin,
                                    PageSize { LineCount(1), _settings.pageSize.columns },
                                    false,
                                    LineCount(0),
                                    "host-writable-status-line" },
    _indicatorStatusScreen {
        *this,        &_indicatorScreenMargin, PageSize { LineCount(1), _settings.pageSize.columns }, false,
        LineCount(0), "indicator-status-line"
    },
    _currentScreen { &_primaryScreen },
    _viewport { *this, std::bind(&Terminal::onViewportChanged, this) },
    _indicatorStatusLineDefinition { parseStatusLineDefinition(_settings.indicatorStatusLine.left,
                                                               _settings.indicatorStatusLine.middle,
                                                               _settings.indicatorStatusLine.right) },
    _selectionHelper { this },
    _extendedSelectionHelper { this },
    _customSelectionHelper { this },
    _refreshInterval { _settings.refreshRate },
    _traceHandler { *this },
    _cellPixelSize {},
    _defaultColorPalette { _settings.colorPalette },
    _colorPalette { _settings.colorPalette },
    _mainScreenMargin {
        .vertical =
            Margin::Vertical { .from = {}, .to = _settings.pageSize.lines.as<LineOffset>() - LineOffset(1) },
        .horizontal =
            Margin::Horizontal { .from = {},
                                 .to = _settings.pageSize.columns.as<ColumnOffset>() - ColumnOffset(1) }
    },
    _hostWritableScreenMargin { .vertical = Margin::Vertical { .from = {}, .to = LineOffset(0) },
                                .horizontal =
                                    Margin::Horizontal { .from = {},
                                                         .to = _settings.pageSize.columns.as<ColumnOffset>()
                                                               - ColumnOffset(1) } },
    _maxSixelColorRegisters { _settings.maxImageRegisterCount },
    _effectiveImageCanvasSize { _settings.maxImageSize },
    _sixelColorPalette { std::make_shared<SixelColorPalette>(_maxSixelColorRegisters,
                                                             _maxSixelColorRegisters) },
    _imagePool { [this](Image const* image) {
        discardImage(*image);
    } },
    _hyperlinks { .cache = HyperlinkCache { 1024 } },
    _sequenceBuilder { ModeDependantSequenceHandler { *this }, TerminalInstructionCounter { *this } },
    _parser { std::ref(_sequenceBuilder) },
    _viCommands { *this },
    _inputHandler { _viCommands, ViMode::Insert }
{
    _savedColorPalettes.reserve(MaxColorPaletteSaveStackSize);

    // TODO(should be this instead?): hardReset();
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::LeftRightMargin, false);

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);
}

void Terminal::onViewportChanged()
{
    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = _viewport.clampCellLocation(_viCommands.cursorPosition);

    _eventListener.onScrollOffsetChanged(_viewport.scrollOffset());
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::setRefreshRate(RefreshRate refreshRate)
{
    _settings.refreshRate = refreshRate;
    _refreshInterval = RefreshInterval { refreshRate };
}

void Terminal::setLastMarkRangeOffset(LineOffset value) noexcept
{
    _settings.copyLastMarkRangeOffset = value;
}

std::optional<vtpty::Pty::ReadResult> Terminal::readFromPty()
{
    auto const timeout =
#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
        (_renderBuffer.state == RenderBufferState::WaitingForRefresh && !_screenDirty)
            ? std::optional { _refreshInterval.value }
            : std::chrono::milliseconds(0);
#else
        std::optional<std::chrono::milliseconds> { std::nullopt };
#endif

    // Request a new Buffer Object if the current one cannot sufficiently
    // store a single text line.
    if (_currentPtyBuffer->bytesAvailable() < unbox<size_t>(_settings.pageSize.columns))
    {
        if (vtpty::ptyInLog)
            vtpty::ptyInLog()("Only {} bytes left in TBO. Allocating new buffer from pool.",
                              _currentPtyBuffer->bytesAvailable());
        _currentPtyBuffer = _ptyBufferPool.allocateBufferObject();
    }

    return _pty->read(*_currentPtyBuffer, timeout, _ptyReadBufferSize);
}

void Terminal::setExecutionMode(ExecutionMode mode)
{
    auto _ = std::unique_lock(_breakMutex);
    _executionMode = mode;
    _breakCondition.notify_one();
    _pty->wakeupReader();
}

bool Terminal::processInputOnce()
{
    // clang-format off
    switch (_executionMode.load())
    {
        case ExecutionMode::BreakAtEmptyQueue:
            _executionMode = ExecutionMode::Waiting;
            [[fallthrough]];
        case ExecutionMode::Normal:
            if (!_traceHandler.pendingSequences().empty())
            {
                auto const _ = std::lock_guard { *this };
                _traceHandler.flushAllPending();
                return true;
            }
            break;
        case ExecutionMode::Waiting:
        {
            auto lock = std::unique_lock(_breakMutex);
            _breakCondition.wait(lock, [this]() { return _executionMode != ExecutionMode::Waiting; });
            return true;
        }
        case ExecutionMode::SingleStep:
            if (!_traceHandler.pendingSequences().empty())
            {
                auto const _ = std::lock_guard { *this };
                _executionMode = ExecutionMode::Waiting;
                _traceHandler.flushOne();
                return true;
            }
            break;
    }
    // clang-format on

    auto const readResult = readFromPty();

    if (!readResult)
    {
        terminalLog()("PTY read failed. {}", strerror(errno));
        if (errno == EINTR || errno == EAGAIN)
            return true;

        _pty->close();
        return false;
    }
    string_view const buf = readResult->data;
    _usingStdoutFastPipe = readResult->fromStdoutFastPipe;

    if (buf.empty())
    {
        terminalLog()("PTY read returned with zero bytes. Closing PTY.");
        _pty->close();
        return false;
    }

    {
        auto const _ = std::lock_guard { *this };
        _parser.parseFragment(buf);
    }

    if (!_modes.enabled(DECMode::BatchedRendering))
        screenUpdated();

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
    ensureFreshRenderBuffer();
#endif

    return true;
}

// {{{ RenderBuffer synchronization
void Terminal::breakLoopAndRefreshRenderBuffer()
{
    _changes++;
    _renderBuffer.state = RenderBufferState::RefreshBuffersAndTrySwap;
    _eventListener.renderBufferUpdated();

    // if (this_thread::get_id() == _mainLoopThreadID)
    //     return;

    _pty->wakeupReader();
}

bool Terminal::refreshRenderBuffer(bool locked)
{
    _renderBuffer.state = RenderBufferState::RefreshBuffersAndTrySwap;
    ensureFreshRenderBuffer(locked);
    return _renderBuffer.state == RenderBufferState::WaitingForRefresh;
}

bool Terminal::ensureFreshRenderBuffer(bool locked)
{
    if (!_renderBufferUpdateEnabled)
    {
        // _renderBuffer.state = RenderBufferState::WaitingForRefresh;
        return false;
    }

    auto const elapsed = _currentTime - _renderBuffer.lastUpdate;
    auto const avoidRefresh = elapsed < _refreshInterval.value;

    switch (_renderBuffer.state.load())
    {
        case RenderBufferState::WaitingForRefresh:
            if (avoidRefresh)
                break;
            _renderBuffer.state = RenderBufferState::RefreshBuffersAndTrySwap;
            [[fallthrough]];
        case RenderBufferState::RefreshBuffersAndTrySwap: {
            auto& backBuffer = _renderBuffer.backBuffer();
            auto const lastCursorPos = backBuffer.cursor;
            if (!locked)
                fillRenderBuffer(_renderBuffer.backBuffer(), true);
            else
                fillRenderBufferInternal(_renderBuffer.backBuffer(), true);
            auto const cursorChanged =
                lastCursorPos.has_value() != backBuffer.cursor.has_value()
                || (backBuffer.cursor.has_value() && backBuffer.cursor->position != lastCursorPos->position);
            if (cursorChanged)
                _eventListener.cursorPositionChanged();
            _renderBuffer.state = RenderBufferState::TrySwapBuffers;
            [[fallthrough]];
        }
        case RenderBufferState::TrySwapBuffers: {
            [[maybe_unused]] auto const success = _renderBuffer.swapBuffers(_currentTime);

#if defined(CONTOUR_PERF_STATS)
            logRenderBufferSwap(success, _lastFrameID);
#endif

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
            // Passively invoked by the terminal thread -> do inform render thread about updates.
            if (success)
                _eventListener.renderBufferUpdated();
#endif
        }
        break;
    }
    return true;
}

PageSize Terminal::TheSelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::TheSelectionHelper::wrappedLine(LineOffset line) const noexcept
{
    return terminal->isLineWrapped(line);
}

bool Terminal::TheSelectionHelper::cellEmpty(CellLocation pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().isCellEmpty(pos);
}

int Terminal::TheSelectionHelper::cellWidth(CellLocation pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().cellWidthAt(pos);
}

/**
 * Sets the hyperlink into hovering state if mouse is currently hovering it
 * and unsets the state when the object is being destroyed.
 */
struct ScopedHyperlinkHover
{
    std::shared_ptr<HyperlinkInfo const> href;

    ScopedHyperlinkHover(Terminal const& terminal, ScreenBase const& /*screen*/):
        href { terminal.tryGetHoveringHyperlink() }
    {
        if (href)
            href->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
    }

    ~ScopedHyperlinkHover()
    {
        if (href)
            href->state = HyperlinkState::Inactive;
    }
};

void Terminal::updateInputMethodPreeditString(std::string preeditString)
{
    if (_inputMethodData.preeditString == preeditString)
        return;

    _inputMethodData.preeditString = std::move(preeditString);
    screenUpdated();
}

void Terminal::fillRenderBuffer(RenderBuffer& output, bool includeSelection)
{
    auto const _ = std::lock_guard { *this };
    fillRenderBufferInternal(output, includeSelection);
}

void Terminal::fillRenderBufferInternal(RenderBuffer& output, bool includeSelection)
{
    verifyState();

    output.clear();

    _changes.store(0);
    _screenDirty = false;
    ++_lastFrameID;

#if defined(CONTOUR_PERF_STATS)
    if (terminalLog)
        terminalLog()("{}: Refreshing render buffer.\n", _lastFrameID.load());
#endif

    auto baseLine = LineOffset(0);

    if (_settings.statusDisplayPosition == StatusDisplayPosition::Top)
        baseLine += fillRenderBufferStatusLine(output, includeSelection, baseLine).as<LineOffset>();

    auto const hoveringHyperlinkGuard = ScopedHyperlinkHover { *this, *_currentScreen };
    auto const mainDisplayReverseVideo = isModeEnabled(vtbackend::DECMode::ReverseVideo);
    auto const highlightSearchMatches =
        _search.pattern.empty() ? HighlightSearchMatches::No : HighlightSearchMatches::Yes;

    auto const theCursorPosition = [&]() -> std::optional<CellLocation> {
        if (inputHandler().mode() == ViMode::Insert)
        {
            if (isModeEnabled(DECMode::VisibleCursor))
                return optional<CellLocation> { currentScreen().cursor().position };
            return std::nullopt;
        }
        return _viCommands.cursorPosition;
    }();

    if (isPrimaryScreen())
        _lastRenderPassHints =
            _primaryScreen.render(RenderBufferBuilder<PrimaryScreenCell> { *this,
                                                                           output,
                                                                           baseLine,
                                                                           mainDisplayReverseVideo,
                                                                           HighlightSearchMatches::Yes,
                                                                           _inputMethodData,
                                                                           theCursorPosition,
                                                                           includeSelection },
                                  _viewport.scrollOffset(),
                                  highlightSearchMatches);
    else
        _lastRenderPassHints =
            _alternateScreen.render(RenderBufferBuilder<AlternateScreenCell> { *this,
                                                                               output,
                                                                               baseLine,
                                                                               mainDisplayReverseVideo,
                                                                               HighlightSearchMatches::Yes,
                                                                               _inputMethodData,
                                                                               theCursorPosition,
                                                                               includeSelection },
                                    _viewport.scrollOffset(),
                                    highlightSearchMatches);

    if (_settings.statusDisplayPosition == StatusDisplayPosition::Bottom)
    {
        baseLine += pageSize().lines.as<LineOffset>();
        fillRenderBufferStatusLine(output, includeSelection, baseLine);
    }
}

LineCount Terminal::fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, LineOffset base)
{
    auto const mainDisplayReverseVideo = isModeEnabled(vtbackend::DECMode::ReverseVideo);
    switch (_statusDisplayType)
    {
        case StatusDisplayType::None:
            //.
            return LineCount(0);
        case StatusDisplayType::Indicator:
            updateIndicatorStatusLine();
            _indicatorStatusScreen.render(RenderBufferBuilder<StatusDisplayCell> { *this,
                                                                                   output,
                                                                                   base,
                                                                                   !mainDisplayReverseVideo,
                                                                                   HighlightSearchMatches::No,
                                                                                   InputMethodData {},
                                                                                   nullopt,
                                                                                   includeSelection },
                                          ScrollOffset(0));
            return _indicatorStatusScreen.pageSize().lines;
        case StatusDisplayType::HostWritable:
            _hostWritableStatusLineScreen.render(
                RenderBufferBuilder<StatusDisplayCell> { *this,
                                                         output,
                                                         base,
                                                         !mainDisplayReverseVideo,
                                                         HighlightSearchMatches::No,
                                                         InputMethodData {},
                                                         nullopt,
                                                         includeSelection },
                ScrollOffset(0));
            return _hostWritableStatusLineScreen.pageSize().lines;
    }
    crispy::unreachable();
}
// }}}

void Terminal::updateIndicatorStatusLine()
{
    Require(_activeStatusDisplay != ActiveStatusDisplay::IndicatorStatusLine);

    auto const colors = [&]() {
        if (!_focused)
        {
            return colorPalette().indicatorStatusLineInactive;
        }
        else
        {
            switch (_inputHandler.mode())
            {
                case ViMode::Insert: return colorPalette().indicatorStatusLineInsertMode;
                case ViMode::Normal: return colorPalette().indicatorStatusLineNormalMode;
                case ViMode::Visual:
                case ViMode::VisualLine:
                case ViMode::VisualBlock: return colorPalette().indicatorStatusLineVisualMode;
            }
        }
        crispy::unreachable();
    }();

    auto const backupForeground = _colorPalette.defaultForeground;
    auto const backupBackground = _colorPalette.defaultBackground;
    _colorPalette.defaultForeground = colors.foreground;
    _colorPalette.defaultBackground = colors.background;

    auto const _ = crispy::finally { [&]() {
        // Cleaning up.
        _colorPalette.defaultForeground = backupForeground;
        _colorPalette.defaultBackground = backupBackground;
        verifyState();
    } };

    // Prepare old status line's cursor position and some other flags.
    _indicatorStatusScreen.moveCursorTo({}, {});
    _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
    _indicatorStatusScreen.cursor().graphicsRendition.backgroundColor = colors.background;
    _indicatorStatusScreen.clearLine();

    using Styling = StatusLineStyling;
    auto const& definitions = _indicatorStatusLineDefinition;

    if (!definitions.left.empty())
    {
        auto const leftVT = serializeToVT(*this, definitions.left, Styling::Enabled);
        writeToScreenInternal(_indicatorStatusScreen, leftVT);
    }

    if (!definitions.middle.empty() || !definitions.right.empty())
    {
        // Don't show the middle segment if text is too long.
        auto const middleLength = serializeToVT(*this, definitions.middle, Styling::Disabled).size();
        auto const center = pageSize().columns / ColumnCount(2) - ColumnCount(1);
        // size of middleVT segment is less that number of elements in the string due to formatting
        // and on average we can assume that the middle segment is half the size of the string
        _indicatorStatusScreen.moveCursorToColumn(
            ColumnOffset::cast_from(center - ColumnOffset::cast_from(middleLength / 4)));
        auto const middleVT = serializeToVT(*this, definitions.middle, Styling::Enabled);
        if (unbox<size_t>(center) > static_cast<size_t>(middleLength / 2))
            writeToScreenInternal(_indicatorStatusScreen, middleVT);

        // Don't show the right part if the left and middle segments are too long.
        // That is, if the current cursor position is past the beginning of the right segment.
        // Also don't show if the right text is too long.
        auto const rightLength = serializeToVT(*this, definitions.right, Styling::Disabled).size();
        if (ColumnCount::cast_from(rightLength) < _indicatorStatusScreen.pageSize().columns)
        {
            _indicatorStatusScreen.moveCursorToColumn(
                boxed_cast<ColumnOffset>(_indicatorStatusScreen.pageSize().columns)
                - ColumnOffset::cast_from(rightLength));

            auto const rightVT = serializeToVT(*this, definitions.right, Styling::Enabled);
            writeToScreenInternal(_indicatorStatusScreen, rightVT);
        }
    }
}

Handled Terminal::sendKeyEvent(Key key, Modifiers modifiers, KeyboardEventType eventType, Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    if (!allowInput())
        return Handled { true };

    if (_inputHandler.sendKeyPressEvent(key, modifiers, eventType))
        return Handled { true };

    bool const success = _inputGenerator.generate(key, modifiers, eventType);
    if (success)
    {
        flushInput();
        _viewport.scrollToBottom();
    }
    return Handled { success };
}

Handled Terminal::sendCharEvent(
    char32_t ch, uint32_t physicalKey, Modifiers modifiers, KeyboardEventType eventType, Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    // Early exit if KAM is enabled.
    if (!allowInput())
        return Handled { true };

    if (_inputHandler.sendCharPressEvent(ch, modifiers, eventType))
        return Handled { true };

    auto const success = _inputGenerator.generate(ch, physicalKey, modifiers, eventType);
    if (success)
    {
        flushInput();
        _viewport.scrollToBottom();
    }
    return Handled { success };
}

Handled Terminal::sendMousePressEvent(Modifiers modifiers,
                                      MouseButton button,
                                      PixelCoordinate pixelPosition,
                                      bool uiHandledHint)
{
    if (button == MouseButton::Left)
    {
        _leftMouseButtonPressed = true;
        _lastMousePixelPositionOnLeftClick = pixelPosition;
        if (!allowPassMouseEventToApp(modifiers))
            uiHandledHint = handleMouseSelection(modifiers) || uiHandledHint;
    }

    verifyState();

    auto const eventHandledByApp =
        allowPassMouseEventToApp(modifiers)
        && _inputGenerator.generateMousePress(
            modifiers, button, _currentMousePosition, pixelPosition, uiHandledHint);

    // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
    // under the current position
    flushInput();
    return Handled { eventHandledByApp && !isModeEnabled(DECMode::MousePassiveTracking) };
}

void Terminal::triggerWordWiseSelection(CellLocation startPos, TheSelectionHelper const& selectionHelper)
{
    setSelector(std::make_unique<WordWiseSelection>(selectionHelper, startPos, selectionUpdatedHelper()));

    if (_selection->extend(startPos))
    {
        updateSelectionMatches();
        onSelectionUpdated();
    }
}

void Terminal::updateSelectionMatches()
{
    if (!_settings.visualizeSelectedWord)
        return;

    auto const text = extractSelectionText();
    auto const text32 = unicode::convert_to<char32_t>(string_view(text.data(), text.size()));
    setNewSearchTerm(text32, true);
}

void Terminal::setStatusLineDefinition(StatusLineDefinition&& definition)
{
    _indicatorStatusLineDefinition = std::move(definition);
    updateIndicatorStatusLine();
}

void Terminal::resetStatusLineDefinition()
{
    _indicatorStatusLineDefinition = parseStatusLineDefinition(_settings.indicatorStatusLine.left,
                                                               _settings.indicatorStatusLine.middle,
                                                               _settings.indicatorStatusLine.right);
    updateIndicatorStatusLine();
}

bool Terminal::handleMouseSelection(Modifiers modifiers)
{
    verifyState();

    double const diffMs = chrono::duration<double, std::milli>(_currentTime - _lastClick).count();
    _lastClick = _currentTime;
    _speedClicks = (diffMs >= 0.0 && diffMs <= 1000.0 ? _speedClicks : 0) % 4 + 1;

    auto const startPos = CellLocation {
        .line = _currentMousePosition.line - boxed_cast<LineOffset>(_viewport.scrollOffset()),
        .column = _currentMousePosition.column,
    };

    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = startPos;

    switch (_speedClicks)
    {
        case 1:
            if (_search.initiatedByDoubleClick)
                clearSearch();
            clearSelection();
            if (modifiers == _settings.mouseBlockSelectionModifiers)
            {
                setSelector(std::make_unique<RectangularSelection>(
                    _selectionHelper, startPos, selectionUpdatedHelper()));
            }
            else
            {
                setSelector(
                    std::make_unique<LinearSelection>(_selectionHelper, startPos, selectionUpdatedHelper()));
            }
            break;
        case 2: triggerWordWiseSelection(startPos, _selectionHelper); break;
        case 3: triggerWordWiseSelection(startPos, _extendedSelectionHelper); break;
        case 4:
            setSelector(
                std::make_unique<FullLineSelection>(_selectionHelper, startPos, selectionUpdatedHelper()));
            if (_selection->extend(startPos))
                onSelectionUpdated();
            break;
        default: clearSelection(); break;
    }

    breakLoopAndRefreshRenderBuffer();
    return true;
}

void Terminal::triggerWordWiseSelectionWithCustomDelimiters(string const& delimiters)
{
    verifyState();
    auto const startPos = CellLocation {
        .line = _currentMousePosition.line - boxed_cast<LineOffset>(_viewport.scrollOffset()),
        .column = _currentMousePosition.column,
    };
    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = startPos;
    _customSelectionHelper.wordDelimited = [wordDelimiters = unicode::from_utf8(delimiters),
                                            this](CellLocation const& pos) {
        return this->wordDelimited(pos, wordDelimiters);
    };

    triggerWordWiseSelection(startPos, _customSelectionHelper);
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::setSelector(std::unique_ptr<Selection> selector)
{
    Require(selector.get() != nullptr);
    inputLog()("Creating cell selector: {}", *selector);
    _selection = std::move(selector);
}

void Terminal::clearSelection()
{
    if (_inputHandler.isVisualMode())
    {
        if (!_leftMouseButtonPressed)
            // Don't clear if in visual mode and mouse wasn't used.
            return;
        _inputHandler.setMode(ViMode::Normal);
    }

    if (!_selection)
        return;

    inputLog()("Clearing selection.");
    _selection.reset();

    onSelectionUpdated();

    breakLoopAndRefreshRenderBuffer();
}

bool Terminal::shouldExtendSelectionByMouse(CellLocation newPosition,
                                            PixelCoordinate pixelPosition) const noexcept
{
    if (!selectionAvailable() || selector()->state() == Selection::State::Complete)
        return false;

    auto selectionCorner = selector()->to();
    auto const cellPixelWidth = unbox<int>(cellPixelSize().width);
    if (selector()->state() == Selection::State::Waiting)
    {
        if (!(newPosition.line != selectionCorner.line
              || abs(_lastMousePixelPositionOnLeftClick.x.value - pixelPosition.x.value)
                     / (cellPixelWidth / 2)))
            return false;
    }
    else if (newPosition.line == selectionCorner.line)
    {
        auto const mod = pixelPosition.x.value % cellPixelWidth;
        if (newPosition.column > selectionCorner.column) // selection to the right
        {
            if (mod < cellPixelWidth / 2)
                return false;
        }
        else if (newPosition.column < selectionCorner.column) // selection to the left
        {
            if (mod > cellPixelWidth / 2)
                return false;
        }
    }

    return true;
}

void Terminal::sendMouseMoveEvent(Modifiers modifiers,
                                  CellLocation newPosition,
                                  PixelCoordinate pixelPosition,
                                  bool uiHandledHint)
{
    // Updates the internal state to remember the current mouse' position.
    // On top of that, a few more things are happening:
    // - updates cursor hovering state (e.g. necessary for properly highlighting hyperlinks)
    // - the internal speed-clicks counter (for tracking rapid multi click) is reset
    // - grid text selection is extended
    verifyState();

    // avoid applying event for sctatus line or inidcator status line
    if (!(isPrimaryScreen() || isAlternateScreen()))
        return;

    if (newPosition != _currentMousePosition)
    {
        // Speed-clicks are only counted when not moving the mouse in between, so reset on mouse move here.
        _speedClicks = 0;

        _currentMousePosition = newPosition;
        updateHoveringHyperlinkState();
    }

    auto const shouldExtendSelection = shouldExtendSelectionByMouse(newPosition, pixelPosition);
    auto relativePos = _viewport.translateScreenToGridCoordinate(newPosition);
    if (shouldExtendSelection && _leftMouseButtonPressed)
    {
        _viCommands.cursorPosition = relativePos;
        _viewport.makeVisible(_viCommands.cursorPosition.line);
    }

    // Do not handle mouse-move events in sub-cell dimensions.
    if (allowPassMouseEventToApp(modifiers))
    {
        if (_inputGenerator.generateMouseMove(
                modifiers, relativePos, pixelPosition, uiHandledHint || !selectionAvailable()))
            flushInput();
        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return;
    }

    if (_leftMouseButtonPressed)
    {
        if (!selectionAvailable())
        {
            setSelector(
                std::make_unique<LinearSelection>(_selectionHelper, relativePos, selectionUpdatedHelper()));
        }
        else if (selector()->state() != Selection::State::Complete)
        {
            if (currentScreen().isCellEmpty(relativePos)
                && !currentScreen().compareCellTextAt(relativePos, 0x20))
                relativePos.column = ColumnOffset { 0 } + unbox(_settings.pageSize.columns - 1);
            _viCommands.cursorPosition = relativePos;
            if (_inputHandler.mode() != ViMode::Insert)
                _inputHandler.setMode(selector()->viMode());
            if (selector()->extend(relativePos))
            {
                updateSelectionMatches();
                breakLoopAndRefreshRenderBuffer();
            }
        }
    }
}

Handled Terminal::sendMouseReleaseEvent(Modifiers modifiers,
                                        MouseButton button,
                                        PixelCoordinate pixelPosition,
                                        bool uiHandledHint)
{
    verifyState();

    if (button == MouseButton::Left)
    {
        _leftMouseButtonPressed = false;
        if (selectionAvailable())
        {
            switch (selector()->state())
            {
                case Selection::State::InProgress:
                    if (_inputHandler.mode() == ViMode::Insert)
                        selector()->complete();
                    _eventListener.onSelectionCompleted();
                    break;
                case Selection::State::Waiting: _selection.reset(); break;
                case Selection::State::Complete: break;
            }
        }
    }

    if (allowPassMouseEventToApp(modifiers)
        && _inputGenerator.generateMouseRelease(
            modifiers, button, _currentMousePosition, pixelPosition, uiHandledHint))
    {
        flushInput();

        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return Handled { true };
    }

    return Handled { true };
}

bool Terminal::sendFocusInEvent()
{
    _focused = true;
    breakLoopAndRefreshRenderBuffer();

    if (_inputGenerator.generateFocusInEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::sendFocusOutEvent()
{
    _focused = false;
    breakLoopAndRefreshRenderBuffer();

    if (_inputGenerator.generateFocusOutEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

void Terminal::sendPaste(string_view text)
{
    if (!allowInput())
        return;

    if (_inputHandler.isEditingSearch())
    {
        _search.pattern += unicode::convert_to<char32_t>(text);
        screenUpdated();
        return;
    }

    _inputGenerator.generatePaste(text);
    flushInput();
}

void Terminal::sendRawInput(string_view text)
{
    if (!allowInput())
        return;

    if (_inputHandler.isEditingSearch())
    {
        inputLog()("Sending raw input to search input: {}", crispy::escape(text));
        _search.pattern += unicode::convert_to<char32_t>(text);
        screenUpdated();
        return;
    }

    inputLog()("Sending raw input to stdin: {}", crispy::escape(text));
    _inputGenerator.generateRaw(text);
    flushInput();
}

bool Terminal::hasInput() const noexcept
{
    return !_inputGenerator.peek().empty();
}

void Terminal::flushInput()
{
    if (_inputGenerator.peek().empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    auto const input = _inputGenerator.peek();
    auto const rv = _pty->write(input);
    if (rv > 0)
        _inputGenerator.consume(rv);
}

void Terminal::writeToScreen(string_view vtStream)
{
    {
        auto const l = std::lock_guard { *this };
        while (!vtStream.empty())
        {
            if (_currentPtyBuffer->bytesAvailable() < 64
                && _currentPtyBuffer->bytesAvailable() < vtStream.size())
                _currentPtyBuffer = _ptyBufferPool.allocateBufferObject();
            auto const chunk =
                vtStream.substr(0, std::min(vtStream.size(), _currentPtyBuffer->bytesAvailable()));
            vtStream.remove_prefix(chunk.size());
            _parser.parseFragment(_currentPtyBuffer->writeAtEnd(chunk));
        }
    }

    if (!_modes.enabled(DECMode::BatchedRendering))
    {
        screenUpdated();
    }
}

string_view Terminal::lockedWriteToPtyBuffer(string_view data)
{
    if (_currentPtyBuffer->bytesAvailable() < 64 && _currentPtyBuffer->bytesAvailable() < data.size())
        _currentPtyBuffer = _ptyBufferPool.allocateBufferObject();

    auto const chunk = data.substr(0, std::min(data.size(), _currentPtyBuffer->bytesAvailable()));
    auto const _ = std::scoped_lock { *_currentPtyBuffer };
    auto const ref = _currentPtyBuffer->writeAtEnd(chunk);
    return string_view(ref.data(), ref.size());
}

size_t Terminal::maxBulkTextSequenceWidth() const noexcept
{
    if (!isPrimaryScreen())
        return 0;

    if (!_primaryScreen.currentLine().isTrivialBuffer())
        return 0;

    assert(_mainScreenMargin.horizontal.to >= _currentScreen->cursor().position.column);

    return unbox<size_t>(_mainScreenMargin.horizontal.to - _currentScreen->cursor().position.column);
}

// {{{ SimpleSequenceHandler
// This simple sequence handler is used to write to the screen
// without any optimizations (and no parser hooking).
// We use this for rendering the status line.
struct SimpleSequenceHandler
{
    Screen<StatusDisplayCell>& targetScreen;

    void executeControlCode(char controlCode) { targetScreen.executeControlCode(controlCode); }

    void processSequence(Sequence const& seq)
    {
        // NB: We might want to check for some VT sequences that should not be processed here.
        // We might make use of Terminal::activeSequences() here somehow.
        targetScreen.processSequence(seq);
    }

    void writeText(char32_t codepoint) { targetScreen.writeText(codepoint); }

    void writeText(std::string_view chars, size_t /*cellCount*/)
    {
        // implementation of targetScreen.writeText(chars, cellCount)
        // is buggy and does not work correctly
        // so we do not use optimization for
        // ParserEvents::print(chars,cellCount)
        // but write char by char
        // TODO fix targetScreen.writeText(chars, cellCount)
        for (auto c: chars)
            targetScreen.writeText(c);
    }

    void writeTextEnd() { targetScreen.writeTextEnd(); }

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept
    {
        // Returning 0 here, because we do not make use of the performance optimized bulk write above.
        return 0;
    }
};
// }}}

void Terminal::writeToScreenInternal(Screen<StatusDisplayCell>& screen, std::string_view vtStream)
{
    auto sequenceHandler = SimpleSequenceHandler { screen };
    auto sequenceBuilder = SequenceBuilder { sequenceHandler, NoOpInstructionCounter() };
    // auto sequenceBuilder = SequenceBuilder { *this, NoOpInstructionCounter() };
    auto parser = vtparser::Parser { sequenceBuilder };

    parser.parseFragment(vtStream);
}

void Terminal::writeToScreenInternal(std::string_view vtStream)
{
    while (!vtStream.empty())
    {
        auto const chunk = lockedWriteToPtyBuffer(vtStream);
        vtStream.remove_prefix(chunk.size());
        _parser.parseFragment(chunk);
    }
}

void Terminal::updateCursorVisibilityState() const noexcept
{
    if (_settings.cursorDisplay == CursorDisplay::Steady)
        return;

    auto const passed = chrono::duration_cast<chrono::milliseconds>(_currentTime - _lastCursorBlink);
    if (passed < _settings.cursorBlinkInterval)
        return;

    _lastCursorBlink = _currentTime;
    _cursorBlinkState = (_cursorBlinkState + 1) % 2;
}

void Terminal::updateHoveringHyperlinkState()
{
    auto const newState =
        _currentScreen->contains(_currentMousePosition)
            ? _currentScreen->hyperlinkIdAt(_viewport.translateScreenToGridCoordinate(_currentMousePosition))
            : HyperlinkId {};

    auto const oldState = _hoveringHyperlinkId.exchange(newState);

    if (newState != oldState)
        renderBufferUpdated();
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    auto nextBlink = chrono::milliseconds::max();
    if ((isModeEnabled(DECMode::VisibleCursor) && _settings.cursorDisplay == CursorDisplay::Blink)
        || isBlinkOnScreen())
    {
        auto const passedCursor =
            chrono::duration_cast<chrono::milliseconds>(_currentTime - _lastCursorBlink);
        auto const passedSlowBlink = chrono::duration_cast<chrono::milliseconds>(_currentTime - _lastBlink);
        auto const passedRapidBlink =
            chrono::duration_cast<chrono::milliseconds>(_currentTime - _lastRapidBlink);
        if (passedCursor <= _settings.cursorBlinkInterval)
            nextBlink = std::min(nextBlink, _settings.cursorBlinkInterval - passedCursor);
        if (passedSlowBlink <= _slowBlinker.interval)
            nextBlink = std::min(nextBlink, _slowBlinker.interval - passedSlowBlink);
        if (passedRapidBlink <= _rapidBlinker.interval)
            nextBlink = std::min(nextBlink, _rapidBlinker.interval - passedRapidBlink);
    }

    if (_statusDisplayType == StatusDisplayType::Indicator)
    {
        auto const currentSecond =
            chrono::time_point_cast<chrono::seconds>(chrono::system_clock::now()).time_since_epoch().count()
            % 60;
        auto const millisUntilNextMinute =
            chrono::duration_cast<chrono::milliseconds>(chrono::seconds(60 - currentSecond));
        nextBlink = std::min(nextBlink, millisUntilNextMinute);
    }

    if (nextBlink == chrono::milliseconds::max())
        return nullopt;

    return nextBlink;
}

void Terminal::tick(chrono::steady_clock::time_point now) noexcept
{
    auto const changes = _changes.exchange(0);
    (void) changes;
    // TODO: update render buffer if  (changes != 0)

    _currentTime = now;
    updateCursorVisibilityState();
    if (isBlinkOnScreen())
    {
        tie(_rapidBlinker.state, _lastRapidBlink) = nextBlinkState(_rapidBlinker, _lastRapidBlink);
        tie(_slowBlinker.state, _lastBlink) = nextBlinkState(_slowBlinker, _lastBlink);
    }
}

void Terminal::resizeScreen(PageSize totalPageSize, optional<ImageSize> pixels)
{
    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.
    auto const mainDisplayPageSize = totalPageSize - statusLineHeight();

    auto const oldMainDisplayPageSize = _settings.pageSize;

    _factorySettings.pageSize = totalPageSize;
    _settings.pageSize = totalPageSize;
    _currentMousePosition = clampToScreen(_currentMousePosition);
    if (pixels)
        setCellPixelSize(pixels.value() / mainDisplayPageSize);

    // Reset margin to their default.
    _primaryScreen.margin() = Margin {
        .vertical = Margin::Vertical { .from = {}, .to = mainDisplayPageSize.lines.as<LineOffset>() - 1 },
        .horizontal =
            Margin::Horizontal { .from = {}, .to = mainDisplayPageSize.columns.as<ColumnOffset>() - 1 }
    };
    _alternateScreen.margin() = _primaryScreen.margin();

    applyPageSizeToCurrentBuffer();

    _pty->resizeScreen(mainDisplayPageSize, pixels);

    // Adjust Normal-mode's cursor in order to avoid drift when growing/shrinking in main page line count.
    if (mainDisplayPageSize.lines > oldMainDisplayPageSize.lines)
        _viCommands.cursorPosition.line +=
            boxed_cast<LineOffset>(mainDisplayPageSize.lines - oldMainDisplayPageSize.lines);
    else if (oldMainDisplayPageSize.lines > mainDisplayPageSize.lines)
        _viCommands.cursorPosition.line -=
            boxed_cast<LineOffset>(oldMainDisplayPageSize.lines - mainDisplayPageSize.lines);

    _viCommands.cursorPosition = clampToScreen(_viCommands.cursorPosition);

    verifyState();
}

void Terminal::verifyState()
{
#if !defined(NDEBUG)
    auto const thePageSize = _settings.pageSize;
    Require(*_currentMousePosition.column < *thePageSize.columns);
    Require(*_currentMousePosition.line < *thePageSize.lines);

    Require(_hostWritableStatusLineScreen.pageSize() == _indicatorStatusScreen.pageSize());
    Require(_hostWritableStatusLineScreen.pageSize().lines == LineCount(1));
    Require(_hostWritableStatusLineScreen.pageSize().columns == _settings.pageSize.columns);

    // TODO: the current main display's page size PLUS visible status line count must match total page size.

    Require(_hostWritableStatusLineScreen.grid().pageSize().columns == _settings.pageSize.columns);
    Require(_indicatorStatusScreen.grid().pageSize().columns == _settings.pageSize.columns);

    Require(_tabs.empty() || _tabs.back() < unbox<ColumnOffset>(_settings.pageSize.columns));

    _currentScreen->verifyState();

    Require(0 <= *_mainScreenMargin.horizontal.from
            && *_mainScreenMargin.horizontal.to < *pageSize().columns);
    Require(0 <= *_mainScreenMargin.vertical.from && *_mainScreenMargin.vertical.to < *pageSize().lines);
#endif
}

void Terminal::setCursorDisplay(CursorDisplay display)
{
    _settings.cursorDisplay = display;
}

void Terminal::setCursorShape(CursorShape shape)
{
    _settings.cursorShape = shape;
}

void Terminal::setWordDelimiters(string const& wordDelimiters)
{
    _settings.wordDelimiters = unicode::from_utf8(wordDelimiters);

    _selectionHelper.wordDelimited = [wordDelimiters = _settings.wordDelimiters,
                                      this](CellLocation const& pos) {
        return this->wordDelimited(pos, wordDelimiters);
    };
}

void Terminal::setExtendedWordDelimiters(string const& wordDelimiters)
{
    _settings.extendedWordDelimiters = unicode::from_utf8(wordDelimiters);
    _extendedSelectionHelper.wordDelimited = [extendedDelimieters = _settings.extendedWordDelimiters,
                                              this](CellLocation const& pos) {
        return this->wordDelimited(pos, extendedDelimieters);
    };
}

namespace
{
    template <CellConcept Cell>
    struct SelectionRenderer
    {
        gsl::not_null<Terminal const*> term;
        ColumnOffset rightPage {};
        ColumnOffset lastColumn {};
        string text {};
        string currentLine {};

        SelectionRenderer(Terminal const& term, ColumnOffset rightPage): term(&term), rightPage(rightPage) {}

        void operator()(CellLocation pos, Cell const& cell)
        {
            auto const isNewLine = pos.column < lastColumn || (pos.column == lastColumn && !text.empty());
            if (isNewLine && (!term->isLineWrapped(pos.line)))
            {
                // TODO: handle logical line in word-selection (don't include LF in wrapped lines)
                trimSpaceRight(currentLine);
                text += currentLine;
                text += '\n';
                currentLine.clear();
            }
            if (cell.empty())
                currentLine += ' ';
            else
                currentLine += cell.toUtf8();
            lastColumn = pos.column;
        }

        std::string finish()
        {
            trimSpaceRight(currentLine);
            text += currentLine;
            if (dynamic_cast<FullLineSelection const*>(term->selector()))
                text += '\n';
            return std::move(text);
        }
    };
} // namespace

string Terminal::extractSelectionText() const
{
    if (!_selection || _selection->state() == Selection::State::Waiting)
        return "";

    if (isPrimaryScreen())
    {
        auto se = SelectionRenderer<PrimaryScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        vtbackend::renderSelection(*_selection, [&](CellLocation pos) { se(pos, _primaryScreen.at(pos)); });
        return se.finish();
    }
    else
    {
        auto se = SelectionRenderer<AlternateScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        vtbackend::renderSelection(*_selection, [&](CellLocation pos) { se(pos, _alternateScreen.at(pos)); });
        return se.finish();
    }
}

string Terminal::extractLastMarkRange() const
{
    // -1 because we always want to start extracting one line above the cursor by default.
    auto const bottomLine =
        _currentScreen->cursor().position.line + LineOffset(-1) + _settings.copyLastMarkRangeOffset;

    auto const marker1 = optional { bottomLine };

    auto const marker0 = _primaryScreen.findMarkerUpwards(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 + 1;
    auto const lastLine = *marker1;

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        text += _primaryScreen.grid().lineAt(lineNum).toUtf8Trimmed();
        text += '\n';
    }

    return text;
}

// {{{ screen events
void Terminal::requestCaptureBuffer(LineCount lines, bool logical)
{
    _eventListener.requestCaptureBuffer(lines, logical);
}

void Terminal::requestShowHostWritableStatusLine()
{
    _eventListener.requestShowHostWritableStatusLine();
}

void Terminal::bell()
{
    _eventListener.bell();
}

void Terminal::bufferChanged(ScreenType type)
{
    clearSelection();
    _viewport.forceScrollToBottom();
    _eventListener.bufferChanged(type);
}

void Terminal::scrollbackBufferCleared()
{
    clearSelection();
    _viewport.scrollToBottom();
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::screenUpdated()
{
    if (!_renderBufferUpdateEnabled)
        return;

    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
    {
        _renderBuffer.swapBuffers(_renderBuffer.lastUpdate);
        return;
    }

    _screenDirty = true;
    _eventListener.screenUpdated();
}

void Terminal::renderBufferUpdated()
{
    if (!_renderBufferUpdateEnabled)
        return;

    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
    {
        _renderBuffer.swapBuffers(_renderBuffer.lastUpdate);
        return;
    }

    _screenDirty = true;
    _eventListener.renderBufferUpdated();
}

FontDef Terminal::getFontDef()
{
    return _eventListener.getFontDef();
}

void Terminal::setFontDef(FontDef const& fontDef)
{
    _eventListener.setFontDef(fontDef);
}

void Terminal::copyToClipboard(string_view data)
{
    _eventListener.copyToClipboard(data);
}

void Terminal::openDocument(string_view data)
{
    _eventListener.openDocument(data);
}

void Terminal::inspect()
{
    _eventListener.inspect();
}

void Terminal::notify(string_view title, string_view body)
{
    _eventListener.notify(title, body);
}

void Terminal::reply(string_view text)
{
    // this is invoked from within the terminal thread.
    // most likely that's not the main thread, which will however write
    // the actual input events.
    // TODO: introduce new mutex to guard terminal writes.
    _inputGenerator.generateRaw(text);

    auto const* syncReply = getenv("CONTOUR_SYNC_PTY_OUTPUT");

    if (syncReply && *syncReply != '0')
        flushInput();
}

void Terminal::requestWindowResize(PageSize size)
{
    _eventListener.requestWindowResize(size.lines, size.columns);
}

void Terminal::requestWindowResize(ImageSize size)
{
    _eventListener.requestWindowResize(size.width, size.height);
}

void Terminal::setApplicationkeypadMode(bool enabled)
{
    _inputGenerator.setApplicationKeypadMode(enabled);
}

void Terminal::setBracketedPaste(bool enabled)
{
    _inputGenerator.setBracketedPaste(enabled);
}

void Terminal::setCursorStyle(CursorDisplay display, CursorShape shape)
{
    _settings.cursorDisplay = display;
    _settings.cursorShape = shape;

    _cursorDisplay = display;
    _cursorShape = shape;
}

void Terminal::setCursorVisibility(bool /*visible*/)
{
    // don't do anything for now
}

void Terminal::setGenerateFocusEvents(bool enabled)
{
    _inputGenerator.setGenerateFocusEvents(enabled);
}

void Terminal::setMouseProtocol(MouseProtocol protocol, bool enabled)
{
    _inputGenerator.setMouseProtocol(protocol, enabled);
}

void Terminal::setMouseTransport(MouseTransport transport)
{
    _inputGenerator.setMouseTransport(transport);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode mode)
{
    _inputGenerator.setMouseWheelMode(mode);
}

void Terminal::setWindowTitle(string_view title)
{
    _windowTitle = title;
    _eventListener.setWindowTitle(title);
}

std::string const& Terminal::windowTitle() const noexcept
{
    return _windowTitle;
}

void Terminal::requestTabName()
{
    inputHandler().setTabName([&](std::string name) { _tabName = std::move(name); });
}

std::optional<std::string> Terminal::tabName() const noexcept
{
    return _tabName;
}

void Terminal::saveWindowTitle()
{
    _savedWindowTitles.push(_windowTitle);
}

void Terminal::restoreWindowTitle()
{
    if (!_savedWindowTitles.empty())
    {
        _windowTitle = _savedWindowTitles.top();
        _savedWindowTitles.pop();
        setWindowTitle(_windowTitle);
    }
}

void Terminal::setTerminalProfile(string const& configProfileName)
{
    _eventListener.setTerminalProfile(configProfileName);
}

void Terminal::useApplicationCursorKeys(bool enable)
{
    auto const keyMode = enable ? KeyMode::Application : KeyMode::Normal;
    _inputGenerator.setCursorKeysMode(keyMode);
}

void Terminal::setMode(AnsiMode mode, bool enable)
{
    if (!isValidAnsiMode(static_cast<unsigned int>(mode)))
        return;

    if (mode == AnsiMode::KeyboardAction)
    {
        if (enable)
            pushStatusDisplay(StatusDisplayType::Indicator);
        else
            popStatusDisplay();
    }

    _modes.set(mode, enable);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Terminal::setMode(DECMode mode, bool enable)
{
    if (!isValidDECMode(static_cast<unsigned int>(mode)))
    {
        errorLog()("Attempt to {} invalid DEC mode: {}", mode, enable ? "set" : "reset");
        return;
    }

    if (_modes.frozen(mode))
    {
        if (_modes.enabled(mode) != enable)
        {
            terminalLog()(
                "Attempt to change permanently-{} mode {}.", _modes.enabled(mode) ? "set" : "reset", mode);
        }
        return;
    }

    switch (mode)
    {
        case DECMode::AutoWrap: _currentScreen->cursor().autoWrap = enable; break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!enable)
            {
                _mainScreenMargin.horizontal =
                    Margin::Horizontal { .from = ColumnOffset(0),
                                         .to = boxed_cast<ColumnOffset>(_settings.pageSize.columns - 1) };
                _supportedVTSequences.enableSequence(SCOSC);
                _supportedVTSequences.disableSequence(DECSLRM);
            }
            else
            {
                _supportedVTSequences.enableSequence(DECSLRM);
                _supportedVTSequences.disableSequence(SCOSC);
            }
            break;
        case DECMode::Origin: _currentScreen->cursor().originMode = enable; break;
        case DECMode::Columns132: {
            if (!isModeEnabled(DECMode::AllowColumns80to132))
            {
                terminalLog()("Ignoring DECCOLM because DECCOLM is not allowed by mode setting.");
                break;
            }

            // sets the number of columns on the page to 80 or 132 and selects the
            // corresponding 80- or 132-column font
            auto const columns = ColumnCount(enable ? 132 : 80);

            terminalLog()("DECCOLM: Setting columns to {}", columns);

            // Sets the left, right, top and bottom scrolling margins to their default positions.
            setTopBottomMargin(std::nullopt, std::nullopt); // DECSTBM
            setLeftRightMargin(std::nullopt, std::nullopt); // DECRLM

            // resets vertical split screen mode (DECLRMM) to unavailable
            setMode(DECMode::LeftRightMargin, false); // DECSLRM

            // DECCOLM clears data from the status line if the status line is set to host-writable.
            if (_statusDisplayType == StatusDisplayType::HostWritable)
                _hostWritableStatusLineScreen.clearScreen();

            // Erases all data in page memory
            clearScreen();

            requestWindowResize(PageSize { totalPageSize().lines, columns });
        }
        break;
        case DECMode::BatchedRendering:
            if (_modes.enabled(DECMode::BatchedRendering) != enable)
                synchronizedOutput(enable);
            break;
        case DECMode::TextReflow:
            if (_settings.primaryScreen.allowReflowOnResize && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = enable ? LineOffset(0) : currentScreen().cursor().position.line;
                for (auto line = startLine; line < boxed_cast<LineOffset>(_settings.pageSize.lines); ++line)
                    _primaryScreen.grid().lineAt(line).setWrappable(enable);
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(enable);
            break;
        case DECMode::UseAlternateScreen:
            if (enable)
                setScreen(ScreenType::Alternate);
            else
                setScreen(ScreenType::Primary);
            break;
        case DECMode::UseApplicationCursorKeys:
            useApplicationCursorKeys(enable);
            if (isAlternateScreen())
            {
                if (enable)
                    setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case DECMode::BracketedPaste: setBracketedPaste(enable); break;
        case DECMode::MouseSGR:
            if (enable)
                setMouseTransport(MouseTransport::SGR);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseExtended: setMouseTransport(MouseTransport::Extended); break;
        case DECMode::MouseURXVT: setMouseTransport(MouseTransport::URXVT); break;
        case DECMode::MousePassiveTracking:
            _inputGenerator.setPassiveMouseTracking(enable);
            setMode(DECMode::MouseSGR, enable);                    // SGR is required.
            setMode(DECMode::MouseProtocolButtonTracking, enable); // ButtonTracking is default
            break;
        case DECMode::MouseSGRPixels:
            if (enable)
                setMouseTransport(MouseTransport::SGRPixels);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseAlternateScroll:
            if (enable)
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
        case DECMode::FocusTracking: setGenerateFocusEvents(enable); break;
        case DECMode::UsePrivateColorRegisters: _usePrivateColorRegisters = enable; break;
        case DECMode::VisibleCursor: setCursorVisibility(enable); break;
        case DECMode::MouseProtocolX10: setMouseProtocol(MouseProtocol::X10, enable); break;
        case DECMode::MouseProtocolNormalTracking:
            setMouseProtocol(MouseProtocol::NormalTracking, enable);
            break;
        case DECMode::MouseProtocolHighlightTracking:
            setMouseProtocol(MouseProtocol::HighlightTracking, enable);
            break;
        case DECMode::MouseProtocolButtonTracking:
            setMouseProtocol(MouseProtocol::ButtonTracking, enable);
            break;
        case DECMode::MouseProtocolAnyEventTracking:
            setMouseProtocol(MouseProtocol::AnyEventTracking, enable);
            break;
        case DECMode::SaveCursor:
            if (enable)
                _currentScreen->saveCursor();
            else
                _currentScreen->restoreCursor();
            break;
        case DECMode::ExtendedAltScreen:
            if (enable)
            {
                setMode(DECMode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(DECMode::UseAlternateScreen, false);
                // NB: The cursor position doesn't need to be restored,
                // because it's local to the screen buffer.
            }
            break;
        default: break;
    }

    _modes.set(mode, enable);
}

void Terminal::setTopBottomMargin(optional<LineOffset> top, optional<LineOffset> bottom)
{
    auto const defaultTop = LineOffset(0);
    auto const defaultBottom = boxed_cast<LineOffset>(pageSize().lines) - 1;
    auto const sanitizedTop = std::max(defaultTop, top.value_or(defaultTop));
    auto const sanitizedBottom = std::min(defaultBottom, bottom.value_or(defaultBottom));

    if (sanitizedTop < sanitizedBottom)
    {
        _mainScreenMargin.vertical.from = sanitizedTop;
        _mainScreenMargin.vertical.to = sanitizedBottom;
    }
}

void Terminal::setLeftRightMargin(optional<ColumnOffset> left, optional<ColumnOffset> right)
{
    auto const defaultLeft = ColumnOffset(0);
    auto const defaultRight = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    auto const sanitizedRight = std::min(right.value_or(defaultRight), defaultRight);
    auto const sanitizedLeft = std::max(left.value_or(defaultLeft), defaultLeft);
    if (sanitizedLeft < sanitizedRight)
    {
        _mainScreenMargin.horizontal.from = sanitizedLeft;
        _mainScreenMargin.horizontal.to = sanitizedRight;
    }
}

void Terminal::clearScreen()
{
    if (isPrimaryScreen())
        _primaryScreen.clearScreen();
    else
        _alternateScreen.clearScreen();
}

void Terminal::moveCursorTo(LineOffset line, ColumnOffset column)
{
    _currentScreen->moveCursorTo(line, column);
}

void Terminal::softReset()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setGraphicsRendition(GraphicsRendition::Reset);    // SGR
    _currentScreen->resetSavedCursorState();           // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true);             // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);                   // DECOM
    setMode(AnsiMode::KeyboardAction, false);          // KAM
    setMode(DECMode::AutoWrap, false);                 // DECAWM
    setMode(AnsiMode::Insert, false);                  // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, boxed_cast<LineOffset>(_settings.pageSize.lines) - LineOffset(1));       // DECSTBM
    setLeftRightMargin({}, boxed_cast<ColumnOffset>(_settings.pageSize.columns) - ColumnOffset(1)); // DECRLM

    _currentScreen->cursor().hyperlink = {};

    resetColorPalette();

    setActiveStatusDisplay(ActiveStatusDisplay::Main);
    setStatusDisplay(StatusDisplayType::None);

    // TODO: DECNKM (Numeric keypad)
    // TODO: DECSCA (Select character attribute)
    // TODO: DECNRCM (National replacement character set)
    // TODO: GL, GR (G0, G1, G2, G3)
    // TODO: DECAUPSS (Assign user preference supplemental set)
    // TODO: DECSASD (Select active status display)
    // TODO: DECKPM (Keyboard position mode)
    // TODO: DECPCTERM (PCTerm mode)
}

void Terminal::setGraphicsRendition(GraphicsRendition rendition)
{
    if (rendition == GraphicsRendition::Reset)
        _currentScreen->cursor().graphicsRendition = {};
    else
        _currentScreen->cursor().graphicsRendition.flags =
            CellUtil::makeCellFlags(rendition, _currentScreen->cursor().graphicsRendition.flags);
}

void Terminal::setForegroundColor(Color color)
{
    _currentScreen->cursor().graphicsRendition.foregroundColor = color;
}

void Terminal::setBackgroundColor(Color color)
{
    _currentScreen->cursor().graphicsRendition.backgroundColor = color;
}

void Terminal::setUnderlineColor(Color color)
{
    _currentScreen->cursor().graphicsRendition.underlineColor = color;
}

void Terminal::hardReset()
{
    // TODO: make use of _factorySettings
    setScreen(ScreenType::Primary);

    // Ensure that the alternate screen buffer is having the correct size, as well.
    applyPageSizeToMainDisplay(ScreenType::Alternate);

    _modes = Modes {};
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::VisibleCursor, true);

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);

    _primaryScreen.hardReset();
    _alternateScreen.hardReset();
    _hostWritableStatusLineScreen.hardReset();
    _indicatorStatusScreen.hardReset();

    _imagePool.clear();
    _tabs.clear();

    resetColorPalette();

    _hostWritableStatusLineScreen.margin() = Margin {
        .vertical =
            Margin::Vertical { .from = {},
                               .to = boxed_cast<LineOffset>(_hostWritableStatusLineScreen.pageSize().lines)
                                     - 1 },
        .horizontal =
            Margin::Horizontal {
                .from = {},
                .to = boxed_cast<ColumnOffset>(_hostWritableStatusLineScreen.pageSize().columns) - 1 },
    };
    _hostWritableStatusLineScreen.verifyState();

    setActiveStatusDisplay(ActiveStatusDisplay::Main);
    _hostWritableStatusLineScreen.clearScreen();
    _hostWritableStatusLineScreen.updateCursorIterator();

    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    _primaryScreen.margin() = Margin {
        .vertical =
            Margin::Vertical { .from = {}, .to = boxed_cast<LineOffset>(mainDisplayPageSize.lines) - 1 },
        .horizontal = Margin::Horizontal { .from = {},
                                           .to = boxed_cast<ColumnOffset>(mainDisplayPageSize.columns) - 1 },
    };
    _primaryScreen.verifyState();

    _alternateScreen.margin() = Margin {
        .vertical =
            Margin::Vertical { .from = {}, .to = boxed_cast<LineOffset>(mainDisplayPageSize.lines) - 1 },
        .horizontal = Margin::Horizontal { .from = {},
                                           .to = boxed_cast<ColumnOffset>(mainDisplayPageSize.columns) - 1 },
    };
    alternateScreen().margin() = _primaryScreen.margin();
    // NB: We do *NOT* verify alternate screen, because the page size would probably fail as it is
    // designed to be adjusted when the given screen is activated.

    setStatusDisplay(_factorySettings.statusDisplayType);

    _inputGenerator.reset();
}

void Terminal::forceRedraw(std::function<void()> const& artificialSleep)
{
    auto const totalPageSize = _settings.pageSize;
    auto const pageSizeInPixels = cellPixelSize() * totalPageSize;
    auto const tmpPageSize = PageSize { totalPageSize.lines, totalPageSize.columns + ColumnCount(1) };

    resizeScreen(tmpPageSize, pageSizeInPixels);
    if (artificialSleep)
        artificialSleep();
    resizeScreen(totalPageSize, pageSizeInPixels);
}

void Terminal::setScreen(ScreenType type)
{
    if (type == _currentScreenType)
        return;

    switch (type)
    {
        case ScreenType::Primary:
            _currentScreen = &_primaryScreen;
            setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
            break;
        case ScreenType::Alternate:
            _currentScreen = &_alternateScreen;
            if (isModeEnabled(DECMode::MouseAlternateScroll))
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
    }

    _currentScreenType = type;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    bufferChanged(type);
}

void Terminal::applyPageSizeToCurrentBuffer()
{
    applyPageSizeToMainDisplay(screenType());
}

void Terminal::applyPageSizeToMainDisplay(ScreenType screenType)
{
    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    // clang-format off
    switch (screenType)
    {
        case ScreenType::Primary:
            _primaryScreen.applyPageSizeToMainDisplay(mainDisplayPageSize);
            break;
        case ScreenType::Alternate:
            _alternateScreen.applyPageSizeToMainDisplay(mainDisplayPageSize);
            break;
    }

    (void) _hostWritableStatusLineScreen.grid().resize(PageSize { LineCount(1), _settings.pageSize.columns }, CellLocation {}, false);
    (void) _indicatorStatusScreen.grid().resize(PageSize { LineCount(1), _settings.pageSize.columns }, CellLocation {}, false);
    // clang-format on

    // adjust margins for statuslines as well
    auto const statuslineMargin =
        Margin { .vertical = Margin::Vertical { .from = {}, .to = statusLineHeight().as<LineOffset>() - 1 },
                 .horizontal = Margin::Horizontal {
                     .from = {}, .to = _settings.pageSize.columns.as<ColumnOffset>() - 1 } };
    _indicatorScreenMargin = statuslineMargin;
    _hostWritableScreenMargin = statuslineMargin;

    // truncating tabs
    while (!_tabs.empty() && _tabs.back() >= unbox<ColumnOffset>(_settings.pageSize.columns))
        _tabs.pop_back();

    // verifyState();
}

void Terminal::discardImage(Image const& image)
{
    _eventListener.discardImage(image);
}

void Terminal::markCellDirty(CellLocation position) noexcept
{
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!_selection)
        return;

    crispy::ignore_unused(position);
    // if (_selection->contains(position))
    //     clearSelection();
}

void Terminal::markRegionDirty(Rect area) noexcept
{
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!_selection)
        return;

    crispy::ignore_unused(area);
    // if (_selection->intersects(area))
    //     clearSelection();
}

void Terminal::synchronizedOutput(bool enabled)
{
    _renderBufferUpdateEnabled = !enabled;
    if (enabled)
        return;

    tick(chrono::steady_clock::now());

    auto const diff = _currentTime - _renderBuffer.lastUpdate;
    if (diff < _refreshInterval.value)
        return;

    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
        return;

    refreshRenderBuffer(true);
    _eventListener.screenUpdated();
}

void Terminal::onBufferScrolled(LineCount n) noexcept
{
    // Adjust Normal-mode's cursor accordingly to make it fixed at the scroll-offset as if nothing has
    // happened.
    _viCommands.cursorPosition.line -= n;

    // Adjust viewport accordingly to make it fixed at the scroll-offset as if nothing has happened.
    if (viewport().scrolled() || _inputHandler.mode() == ViMode::Normal)
        viewport().scrollUp(n);

    if (!_selection)
        return;

    auto const top = -boxed_cast<LineOffset>(_primaryScreen.historyLineCount());
    if (_selection->from().line > top && _selection->to().line > top)
        _selection->applyScroll(boxed_cast<LineOffset>(n), _primaryScreen.historyLineCount());
    else
        clearSelection();
}
// }}}

void Terminal::setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount)
{
    _primaryScreen.grid().setMaxHistoryLineCount(maxHistoryLineCount);
}

LineCount Terminal::maxHistoryLineCount() const noexcept
{
    return _primaryScreen.grid().maxHistoryLineCount();
}

void Terminal::setTerminalId(VTType id) noexcept
{
    _terminalId = id;
    _supportedVTSequences.reset(id);
}

void Terminal::setStatusDisplay(StatusDisplayType statusDisplayType)
{
    assert(_currentScreen.get() != &_indicatorStatusScreen);

    if (_statusDisplayType == statusDisplayType)
        return;

    markScreenDirty();

    auto const statusLineVisibleBefore = _statusDisplayType != StatusDisplayType::None;
    auto const statusLineVisibleAfter = statusDisplayType != StatusDisplayType::None;
    _statusDisplayType = statusDisplayType;

    if (statusLineVisibleBefore != statusLineVisibleAfter)
        resizeScreen(_settings.pageSize, nullopt);
}

void Terminal::setActiveStatusDisplay(ActiveStatusDisplay activeDisplay)
{
    if (_activeStatusDisplay == activeDisplay)
        return;

    _activeStatusDisplay = activeDisplay;

    // clang-format off
    switch (activeDisplay)
    {
        case ActiveStatusDisplay::Main:
            switch (_currentScreenType)
            {
                case ScreenType::Primary:
                    _currentScreen = &_primaryScreen;
                    break;
                case ScreenType::Alternate:
                    _currentScreen = &_alternateScreen;
                    break;
            }
            break;
        case ActiveStatusDisplay::StatusLine:
            _currentScreen = &_hostWritableStatusLineScreen;
            break;
        case ActiveStatusDisplay::IndicatorStatusLine:
            _currentScreen = &_indicatorStatusScreen;
            break;
    }
    // clang-format on
}

void Terminal::pushStatusDisplay(StatusDisplayType type)
{
    // Only remember the outermost saved status display type.
    if (!_savedStatusDisplayType)
        _savedStatusDisplayType = _statusDisplayType;

    setStatusDisplay(type);
}

void Terminal::popStatusDisplay()
{
    if (!_savedStatusDisplayType)
        return;

    setStatusDisplay(_savedStatusDisplayType.value());
    _savedStatusDisplayType.reset();
}

void Terminal::setAllowInput(bool enabled)
{
    setMode(AnsiMode::KeyboardAction, !enabled);
}

bool Terminal::setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick)
{
    _search.initiatedByDoubleClick = initiatedByDoubleClick;

    if (_search.pattern == text)
        return false;

    _search.pattern = std::move(text);
    return true;
}

void Terminal::setPrompt(std::string prompt)
{
    _prompt.prompt = std::move(prompt);
}

void Terminal::setPromptText(std::string text)
{
    _prompt.text = std::move(text);
}

optional<CellLocation> Terminal::searchReverse(u32string text, CellLocation searchPosition)
{
    if (!setNewSearchTerm(std::move(text), false))
        return searchPosition;

    return searchReverse(searchPosition);
}

optional<CellLocation> Terminal::search(CellLocation searchPosition)
{
    auto const searchText = u32string_view(_search.pattern);
    auto const matchLocation = currentScreen().search(searchText, searchPosition);

    if (matchLocation)
        viewport().makeVisibleWithinSafeArea(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

std::optional<CellLocation> Terminal::searchNextMatch(CellLocation cursorPosition)
{
    auto startPosition = cursorPosition;
    if (startPosition.column < boxed_cast<ColumnOffset>(pageSize().columns))
        startPosition.column++;
    else if (cursorPosition.line < boxed_cast<LineOffset>(pageSize().lines) - 1)
    {
        startPosition.line++;
        startPosition.column = ColumnOffset(0);
    }

    return search(startPosition);
}

std::optional<CellLocation> Terminal::searchPrevMatch(CellLocation cursorPosition)
{
    auto startPosition = cursorPosition;
    if (startPosition.column != ColumnOffset(0))
        startPosition.column--;
    else if (cursorPosition.line > -boxed_cast<LineOffset>(currentScreen().historyLineCount()))
    {
        startPosition.line--;
        startPosition.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    }

    return searchReverse(startPosition);
}

void Terminal::clearSearch()
{
    _search.pattern.clear();
    _search.initiatedByDoubleClick = false;
}

bool Terminal::wordDelimited(CellLocation position) const noexcept
{
    return wordDelimited(position, u32string_view(_settings.wordDelimiters));
}

bool Terminal::wordDelimited(CellLocation position, std::u32string_view wordDelimiters) const noexcept
{
    // Word selection may be off by one
    position.column = std::min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    if (isPrimaryScreen())
        return _primaryScreen.grid().cellEmptyOrContainsOneOf(position, wordDelimiters);
    else
        return _alternateScreen.grid().cellEmptyOrContainsOneOf(position, wordDelimiters);
}

std::tuple<std::u32string, CellLocationRange> Terminal::extractWordUnderCursor(
    CellLocation position) const noexcept
{
    if (isPrimaryScreen())
    {
        auto const range =
            _primaryScreen.grid().wordRangeUnderCursor(position, u32string_view(_settings.wordDelimiters));
        return { _primaryScreen.grid().extractText(range), range };
    }
    else
    {
        auto const range =
            _alternateScreen.grid().wordRangeUnderCursor(position, u32string_view(_settings.wordDelimiters));
        return { _alternateScreen.grid().extractText(range), range };
    }
}

optional<CellLocation> Terminal::searchReverse(CellLocation searchPosition)
{
    auto const searchText = u32string_view(_search.pattern);
    auto const matchLocation = currentScreen().searchReverse(searchText, searchPosition);

    if (matchLocation)
        viewport().makeVisibleWithinSafeArea(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

bool Terminal::isHighlighted(CellLocation cell) const noexcept // NOLINT(bugprone-exception-escape)
{
    return _highlightRange.has_value()
           && std::visit(
               [cell](auto&& highlightRange) {
                   using T = std::decay_t<decltype(highlightRange)>;
                   if constexpr (std::is_same_v<T, LinearHighlight>)
                   {
                       return crispy::ascending(highlightRange.from, cell, highlightRange.to)
                              || crispy::ascending(highlightRange.to, cell, highlightRange.from);
                   }
                   else
                   {
                       return crispy::ascending(highlightRange.from.line, cell.line, highlightRange.to.line)
                              && crispy::ascending(
                                  highlightRange.from.column, cell.column, highlightRange.to.column);
                   }
               },
               _highlightRange.value());
}

void Terminal::onSelectionUpdated()
{
    if (!isModeEnabled(DECMode::ReportGridCellSelection))
        return;

    if (!_selection)
    {
        reply("\033[>M");
    }
    else
    {
        auto const& selection = *_selection;

        auto const to = selection.to();
        if (to.line < LineOffset(0))
            return;

        auto const from = raiseToMinimum(selection.from(), LineOffset(0));
        reply("\033[>{};{};{};{};{}M",
              makeSelectionTypeId(selection),
              from.line + 1,
              from.column + 1,
              to.line + 1,
              to.column + 1);
    }
}

void Terminal::resetHighlight()
{
    _highlightRange = std::nullopt;
    _eventListener.screenUpdated();
}

void Terminal::setHighlightRange(HighlightRange highlightRange)
{
    if (std::holds_alternative<RectangularHighlight>(highlightRange))
    {
        auto range = std::get<RectangularHighlight>(highlightRange);
        auto points = orderedPoints(range.from, range.to);
        range = RectangularHighlight { .from = points.first, .to = points.second };
    }
    _highlightRange = highlightRange;
    _eventListener.updateHighlights();
}

constexpr auto MagicStackTopId = size_t { 0 };

void Terminal::setColorPalette(ColorPalette const& palette) noexcept
{
    _colorPalette = palette;
}

void Terminal::resetColorPalette(ColorPalette const& colors)
{
    _colorPalette = colors;
    _defaultColorPalette = colors;
    _settings.colorPalette = colors;
    _factorySettings.colorPalette = colors;

    if (isModeEnabled(DECMode::ReportColorPaletteUpdated))
        _currentScreen->reportColorPaletteUpdate();
}

void Terminal::pushColorPalette(size_t slot)
{
    if (slot > MaxColorPaletteSaveStackSize)
        return;

    auto const index = [&](size_t slot) {
        if (slot == MagicStackTopId)
            return _savedColorPalettes.empty() ? 0 : _savedColorPalettes.size() - 1;
        else
            return slot - 1;
    }(slot);

    if (index >= _savedColorPalettes.size())
        _savedColorPalettes.resize(index + 1);

    // That's a totally weird idea.
    // Looking at the xterm's source code, and simply mimmicking their semantics without questioning,
    // simply to stay compatible (sadface).
    if (slot != MagicStackTopId && _lastSavedColorPalette < _savedColorPalettes.size())
        _lastSavedColorPalette = _savedColorPalettes.size();

    _savedColorPalettes[index] = _colorPalette;
}

void Terminal::reportColorPaletteStack()
{
    // XTREPORTCOLORS
    reply(std::format("\033[{};{}#Q", _savedColorPalettes.size(), _lastSavedColorPalette));
}

void Terminal::popColorPalette(size_t slot)
{
    if (_savedColorPalettes.empty())
        return;

    auto const index = slot == MagicStackTopId ? _savedColorPalettes.size() - 1 : slot - 1;

    setColorPalette(_savedColorPalettes[index]);
    if (slot == MagicStackTopId)
        _savedColorPalettes.pop_back();
}

// {{{ TraceHandler
TraceHandler::TraceHandler(Terminal& terminal): _terminal { &terminal }
{
}

void TraceHandler::executeControlCode(char controlCode)
{
    auto seq = Sequence {};
    seq.setCategory(FunctionCategory::C0);
    seq.setFinalChar(controlCode);
    _pendingSequences.emplace_back(std::move(seq));
}

void TraceHandler::processSequence(Sequence const& sequence)
{
    _pendingSequences.emplace_back(sequence);
}

void TraceHandler::writeText(char32_t codepoint)
{
    _pendingSequences.emplace_back(codepoint);
}

void TraceHandler::writeText(std::string_view codepoints, size_t cellCount)
{
    _pendingSequences.emplace_back(CodepointSequence { .text = codepoints, .cellCount = cellCount });
}

void TraceHandler::writeTextEnd()
{
}

void TraceHandler::flushAllPending()
{
    for (auto const& pendingSequence: _pendingSequences)
        flushOne(pendingSequence);
    _pendingSequences.clear();
}

void TraceHandler::flushOne()
{
    if (!_pendingSequences.empty())
    {
        flushOne(_pendingSequences.front());
        _pendingSequences.pop_front();
    }
}

void TraceHandler::flushOne(PendingSequence const& pendingSequence)
{
    if (auto const* seq = std::get_if<Sequence>(&pendingSequence))
    {
        if (auto const* functionDefinition = seq->functionDefinition(_terminal->activeSequences()))
            std::cout << std::format("\t{:<20} ; {:<18} ; {}\n",
                                     seq->text(),
                                     functionDefinition->documentation.mnemonic,
                                     functionDefinition->documentation.comment);
        else
            std::cout << std::format("\t{:<20}\n", seq->text());
        _terminal->activeDisplay().processSequence(*seq);
    }
    else if (auto const* codepoint = std::get_if<char32_t>(&pendingSequence))
    {
        std::cout << std::format("\t'{}'\n", unicode::convert_to<char>(*codepoint));
        _terminal->activeDisplay().writeText(*codepoint);
    }
    else if (auto const* codepoints = std::get_if<CodepointSequence>(&pendingSequence))
    {
        std::cout << std::format("\t\"{}\"   ; {} cells\n", codepoints->text, codepoints->cellCount);
        _terminal->activeDisplay().writeText(codepoints->text, codepoints->cellCount);
    }
}
// }}}

// Applies a FunctionDefinition to a given context, emitting the respective command.
std::string to_string(AnsiMode mode)
{
    switch (mode)
    {
        case AnsiMode::KeyboardAction: return "KeyboardAction";
        case AnsiMode::Insert: return "Insert";
        case AnsiMode::SendReceive: return "SendReceive";
        case AnsiMode::AutomaticNewLine: return "AutomaticNewLine";
    }

    return std::format("({})", static_cast<unsigned>(mode));
}

std::string to_string(DECMode mode)
{
    switch (mode)
    {
        case DECMode::UseApplicationCursorKeys: return "UseApplicationCursorKeys";
        case DECMode::DesignateCharsetUSASCII: return "DesignateCharsetUSASCII";
        case DECMode::Columns132: return "Columns132";
        case DECMode::SmoothScroll: return "SmoothScroll";
        case DECMode::ReverseVideo: return "ReverseVideo";
        case DECMode::MouseProtocolX10: return "MouseProtocolX10";
        case DECMode::MouseProtocolNormalTracking: return "MouseProtocolNormalTracking";
        case DECMode::MouseProtocolHighlightTracking: return "MouseProtocolHighlightTracking";
        case DECMode::MouseProtocolButtonTracking: return "MouseProtocolButtonTracking";
        case DECMode::MouseProtocolAnyEventTracking: return "MouseProtocolAnyEventTracking";
        case DECMode::SaveCursor: return "SaveCursor";
        case DECMode::ExtendedAltScreen: return "ExtendedAltScreen";
        case DECMode::Origin: return "Origin";
        case DECMode::AutoWrap: return "AutoWrap";
        case DECMode::PrinterExtend: return "PrinterExtend";
        case DECMode::LeftRightMargin: return "LeftRightMargin";
        case DECMode::ShowToolbar: return "ShowToolbar";
        case DECMode::BlinkingCursor: return "BlinkingCursor";
        case DECMode::VisibleCursor: return "VisibleCursor";
        case DECMode::ShowScrollbar: return "ShowScrollbar";
        case DECMode::AllowColumns80to132: return "AllowColumns80to132";
        case DECMode::DebugLogging: return "DebugLogging";
        case DECMode::UseAlternateScreen: return "UseAlternateScreen";
        case DECMode::BracketedPaste: return "BracketedPaste";
        case DECMode::FocusTracking: return "FocusTracking";
        case DECMode::NoSixelScrolling: return "NoSixelScrolling";
        case DECMode::UsePrivateColorRegisters: return "UsePrivateColorRegisters";
        case DECMode::MouseExtended: return "MouseExtended";
        case DECMode::MouseSGR: return "MouseSGR";
        case DECMode::MouseURXVT: return "MouseURXVT";
        case DECMode::MouseSGRPixels: return "MouseSGRPixels";
        case DECMode::MouseAlternateScroll: return "MouseAlternateScroll";
        case DECMode::MousePassiveTracking: return "MousePassiveTracking";
        case DECMode::ReportGridCellSelection: return "ReportGridCellSelection";
        case DECMode::BatchedRendering: return "BatchedRendering";
        case DECMode::Unicode: return "Unicode";
        case DECMode::TextReflow: return "TextReflow";
        case DECMode::SixelCursorNextToGraphic: return "SixelCursorNextToGraphic";
        case DECMode::ReportColorPaletteUpdated: return "ReportColorPaletteUpdated";
    }
    return std::format("({})", static_cast<unsigned>(mode));
}

} // namespace vtbackend
