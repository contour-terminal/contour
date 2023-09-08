// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ControlCode.h>
#include <vtbackend/Functions.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/RenderBufferBuilder.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtpty/MockPty.h>

#include <crispy/assert.h>
#include <crispy/escape.h>
#include <crispy/utils.h>

#include <fmt/chrono.h>

#include <sys/types.h>

#include <chrono>
#include <cstdlib>
#include <utility>
#include <variant>

#include <libunicode/convert.h>

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

namespace terminal
{

namespace // {{{ helpers
{
    constexpr size_t MaxColorPaletteSaveStackSize = 10;

    void trimSpaceRight(string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    }

    string_view modeString(ViMode mode) noexcept
    {
        switch (mode)
        {
            case ViMode::Normal: return "NORMAL"sv;
            case ViMode::Insert: return "INSERT"sv;
            case ViMode::Visual: return "VISUAL"sv;
            case ViMode::VisualLine: return "VISUAL LINE"sv;
            case ViMode::VisualBlock: return "VISUAL BLOCK"sv;
        }
        crispy::unreachable();
    }

    std::string codepointText(std::u32string const& codepoints)
    {
        std::string text;
        for (auto const codepoint: codepoints)
        {
            if (!text.empty())
                text += ' ';
            text += fmt::format("U+{:X}", static_cast<unsigned>(codepoint));
        }
        return text;
    }

#if defined(CONTOUR_PERF_STATS)
    void logRenderBufferSwap(bool success, uint64_t frameID)
    {
        if (!RenderBufferLog)
            return;

        if (success)
            RenderBufferLog()("Render buffer {} swapped.", frameID);
        else
            RenderBufferLog()("Render buffer {} swapping failed.", frameID);
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
        return CellLocation { std::max(location.line, minimumLine), location.column };
    }

} // namespace
// }}}

Terminal::Terminal(Events& eventListener,
                   std::unique_ptr<Pty> pty,
                   Settings factorySettings,
                   chrono::steady_clock::time_point now):
    _eventListener { eventListener },
    _factorySettings { std::move(factorySettings) },
    _settings { _factorySettings },
    _state { *this },
    _currentTime { now },
    _ptyBufferPool { crispy::nextPowerOfTwo(_settings.ptyBufferObjectSize) },
    _currentPtyBuffer { _ptyBufferPool.allocateBufferObject() },
    _ptyReadBufferSize { crispy::nextPowerOfTwo(_settings.ptyReadBufferSize) },
    _pty { std::move(pty) },
    _lastCursorBlink { now },
    _primaryScreen {
        *this, _settings.pageSize, _settings.primaryScreen.allowReflowOnResize, _settings.maxHistoryLineCount
    },
    _alternateScreen { *this, _settings.pageSize, false, LineCount(0) },
    _hostWritableStatusLineScreen {
        *this, PageSize { LineCount(1), _settings.pageSize.columns }, false, LineCount(0)
    },
    _indicatorStatusScreen {
        *this, PageSize { LineCount(1), _settings.pageSize.columns }, false, LineCount(0)
    },
    _currentScreen { _primaryScreen },
    _viewport { *this,
                [this]() {
                    _eventListener.onScrollOffsetChanged(_viewport.scrollOffset());
                    breakLoopAndRefreshRenderBuffer();
                } },
    _traceHandler { *this },
    _selectionHelper { this },
    _refreshInterval { _settings.refreshRate }
{
    _state.savedColorPalettes.reserve(MaxColorPaletteSaveStackSize);
#if 0
    hardReset();
#else
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::VisibleCursor, true);
#endif
    setMode(DECMode::LeftRightMargin, false);

    for (auto const& [mode, frozen] : _settings.frozenModes)
        freezeMode(mode, frozen);
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

Pty::ReadResult Terminal::readFromPty()
{
    auto const timeout = _renderBuffer.state == RenderBufferState::WaitingForRefresh && !_screenDirty
                             ? chrono::seconds(4)
                             //: _refreshInterval : chrono::seconds(0)
                             : chrono::seconds(30);

    // Request a new Buffer Object if the current one cannot sufficiently
    // store a single text line.
    if (_currentPtyBuffer->bytesAvailable() < unbox<size_t>(_settings.pageSize.columns))
    {
        if (ptyInLog)
            ptyInLog()("Only {} bytes left in TBO. Allocating new buffer from pool.",
                       _currentPtyBuffer->bytesAvailable());
        _currentPtyBuffer = _ptyBufferPool.allocateBufferObject();
    }

    return _pty->read(*_currentPtyBuffer, timeout, _ptyReadBufferSize);
}

void Terminal::setExecutionMode(ExecutionMode mode)
{
    auto _ = std::unique_lock(_state.breakMutex);
    _state.executionMode = mode;
    _state.breakCondition.notify_one();
    _pty->wakeupReader();
}

bool Terminal::processInputOnce()
{
    // clang-format off
    switch (_state.executionMode.load())
    {
        case ExecutionMode::BreakAtEmptyQueue:
            _state.executionMode = ExecutionMode::Waiting;
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
            auto lock = std::unique_lock(_state.breakMutex);
            _state.breakCondition.wait(lock, [this]() { return _state.executionMode != ExecutionMode::Waiting; });
            return true;
        }
        case ExecutionMode::SingleStep:
            if (!_traceHandler.pendingSequences().empty())
            {
                auto const _ = std::lock_guard { *this };
                _state.executionMode = ExecutionMode::Waiting;
                _traceHandler.flushOne();
                return true;
            }
            break;
    }
    // clang-format on

    auto const readResult = readFromPty();

    if (!readResult)
    {
        if (errno == EINTR || errno == EAGAIN)
            return true;

        terminalLog()("PTY read failed. {}", strerror(errno));
        _pty->close();
        return false;
    }
    string_view const buf = std::get<0>(*readResult);
    _state.usingStdoutFastPipe = std::get<1>(*readResult);

    if (buf.empty())
    {
        terminalLog()("PTY read returned with zero bytes. Closing PTY.");
        _pty->close();
        return true;
    }

    {
        auto const _ = std::lock_guard { *this };
        _state.parser.parseFragment(buf);
    }

    if (!_state.modes.enabled(DECMode::BatchedRendering))
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

PageSize Terminal::SelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::SelectionHelper::wordDelimited(CellLocation pos) const noexcept
{
    return terminal->wordDelimited(pos);
}

bool Terminal::SelectionHelper::wrappedLine(LineOffset line) const noexcept
{
    return terminal->isLineWrapped(line);
}

bool Terminal::SelectionHelper::cellEmpty(CellLocation pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().isCellEmpty(pos);
}

int Terminal::SelectionHelper::cellWidth(CellLocation pos) const noexcept
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
    if (TerminalLog)
        TerminalLog()("{}: Refreshing render buffer.\n", _lastFrameID.load());
#endif

    auto baseLine = LineOffset(0);

    if (_settings.statusDisplayPosition == StatusDisplayPosition::Top)
        baseLine += fillRenderBufferStatusLine(output, includeSelection, baseLine).as<LineOffset>();

    auto const hoveringHyperlinkGuard = ScopedHyperlinkHover { *this, _currentScreen };
    auto const mainDisplayReverseVideo = isModeEnabled(terminal::DECMode::ReverseVideo);
    auto const highlightSearchMatches =
        _state.searchMode.pattern.empty() ? HighlightSearchMatches::No : HighlightSearchMatches::Yes;

    auto const theCursorPosition =
        optional<CellLocation> { inputHandler().mode() == ViMode::Insert
                                     ? (isModeEnabled(DECMode::VisibleCursor)
                                            ? optional<CellLocation> { currentScreen().cursor().position }
                                            : nullopt)
                                     : state().viCommands.cursorPosition };

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
    auto const mainDisplayReverseVideo = isModeEnabled(terminal::DECMode::ReverseVideo);
    switch (_state.statusDisplayType)
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
    Require(_state.activeStatusDisplay != ActiveStatusDisplay::IndicatorStatusLine);

    auto const _ = crispy::finally { [this, savedActiveStatusDisplay = _state.activeStatusDisplay]() {
        // Cleaning up.
        setActiveStatusDisplay(savedActiveStatusDisplay);
        verifyState();
    } };

    auto const colors =
        _state.focused ? colorPalette().indicatorStatusLine : colorPalette().indicatorStatusLineInactive;

    setActiveStatusDisplay(ActiveStatusDisplay::IndicatorStatusLine);

    // Prepare old status line's cursor position and some other flags.
    _indicatorStatusScreen.moveCursorTo({}, {});
    _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
    _indicatorStatusScreen.cursor().graphicsRendition.backgroundColor = colors.background;

    // Run status-line update.
    // We cannot use VT writing here, because we shall not interfere with the application's VT state.
    // TODO: Future improvement would be to allow full VT sequence support for the Indicator-status-line,
    // such that we can pass display-control partially over to some user/thirdparty configuration.
    _indicatorStatusScreen.clearLine();
    _indicatorStatusScreen.writeTextFromExternal(
        fmt::format(" {} │ {}", _state.terminalId, modeString(inputHandler().mode())));

    if (!_state.searchMode.pattern.empty() || _state.inputHandler.isEditingSearch())
        _indicatorStatusScreen.writeTextFromExternal(" SEARCH");

    if (!allowInput())
    {
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = BrightColor::Red;
        _indicatorStatusScreen.cursor().graphicsRendition.flags |= CellFlags::Bold;
        _indicatorStatusScreen.writeTextFromExternal(" (PROTECTED)");
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
        _indicatorStatusScreen.cursor().graphicsRendition.flags &= ~CellFlags::Bold;
    }

    if (_state.executionMode != ExecutionMode::Normal)
    {
        _indicatorStatusScreen.writeTextFromExternal(" | ");
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = BrightColor::Yellow;
        _indicatorStatusScreen.cursor().graphicsRendition.flags |= CellFlags::Bold;
        _indicatorStatusScreen.writeTextFromExternal("TRACING");
        if (!_traceHandler.pendingSequences().empty())
            _indicatorStatusScreen.writeTextFromExternal(
                fmt::format(" (#{}): {}",
                            _traceHandler.pendingSequences().size(),
                            _traceHandler.pendingSequences().front()));

        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
        _indicatorStatusScreen.cursor().graphicsRendition.flags &= ~CellFlags::Bold;
    }

    // TODO: Disabled for now, but generally I want that functionality, but configurable somehow.
    auto constexpr IndicatorLineShowCodepoints = false;
    if (IndicatorLineShowCodepoints)
    {
        auto const cursorPosition = _state.inputHandler.mode() == ViMode::Insert
                                        ? _indicatorStatusScreen.cursor().position
                                        : _state.viCommands.cursorPosition;
        auto const text =
            codepointText(isPrimaryScreen() ? _primaryScreen.useCellAt(cursorPosition).codepoints()
                                            : alternateScreen().useCellAt(cursorPosition).codepoints());
        _indicatorStatusScreen.writeTextFromExternal(fmt::format(" | {}", text));
    }

    if (_state.inputHandler.isEditingSearch())
        _indicatorStatusScreen.writeTextFromExternal(fmt::format(
            " │ Search: {}█", unicode::convert_to<char>(u32string_view(_state.searchMode.pattern))));

    auto rightString = ""s;

    if (isPrimaryScreen())
    {
        if (viewport().scrollOffset().value)
            rightString += fmt::format("{}/{}", viewport().scrollOffset(), _primaryScreen.historyLineCount());
        else
            rightString += fmt::format("{}", _primaryScreen.historyLineCount());
    }

    if (!rightString.empty())
        rightString += " │ ";

    // NB: Cannot use std::chrono::system_clock::now() here, because MSVC can't handle it.
    rightString += fmt::format("{:%H:%M} ", fmt::localtime(std::time(nullptr)));

    auto const columnsAvailable = _indicatorStatusScreen.pageSize().columns.as<int>()
                                  - _indicatorStatusScreen.cursor().position.column.as<int>();
    if (rightString.size() <= static_cast<size_t>(columnsAvailable))
    {
        _indicatorStatusScreen.cursor().position.column =
            ColumnOffset::cast_from(_indicatorStatusScreen.pageSize().columns)
            - ColumnOffset::cast_from(rightString.size()) - ColumnOffset(1);
        _indicatorStatusScreen.updateCursorIterator();

        _indicatorStatusScreen.writeTextFromExternal(rightString);
    }
}

bool Terminal::sendKeyPressEvent(Key key, Modifier modifier, Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    if (allowInput() && _state.inputHandler.sendKeyPressEvent(key, modifier))
        return true;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    _viewport.scrollToBottom();
    bool const success = _state.inputGenerator.generate(key, modifier);
    flushInput();
    _viewport.scrollToBottom();
    return success;
}

bool Terminal::sendCharPressEvent(char32_t ch, Modifier modifier, Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    if (_state.inputHandler.sendCharPressEvent(ch, modifier))
        return true;

    auto const success = _state.inputGenerator.generate(ch, modifier);

    flushInput();
    _viewport.scrollToBottom();
    return success;
}

bool Terminal::sendMousePressEvent(Modifier modifier,
                                   MouseButton button,
                                   PixelCoordinate pixelPosition,
                                   bool uiHandledHint)
{
    if (button == MouseButton::Left)
    {
        _leftMouseButtonPressed = true;
        _lastMousePixelPositionOnLeftClick = pixelPosition;
        if (!allowPassMouseEventToApp(modifier))
            uiHandledHint = handleMouseSelection(modifier) || uiHandledHint;
    }

    verifyState();

    auto const eventHandledByApp = allowPassMouseEventToApp(modifier)
                                   && _state.inputGenerator.generateMousePress(
                                       modifier, button, _currentMousePosition, pixelPosition, uiHandledHint);

    // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
    // under the current position
    flushInput();
    return eventHandledByApp && !isModeEnabled(DECMode::MousePassiveTracking);
}

bool Terminal::handleMouseSelection(Modifier modifier)
{
    verifyState();

    double const diffMs = chrono::duration<double, std::milli>(_currentTime - _lastClick).count();
    _lastClick = _currentTime;
    _speedClicks = (diffMs >= 0.0 && diffMs <= 750.0 ? _speedClicks : 0) % 3 + 1;

    auto const startPos = CellLocation {
        _currentMousePosition.line - boxed_cast<LineOffset>(_viewport.scrollOffset()),
        _currentMousePosition.column,
    };

    if (_state.inputHandler.mode() != ViMode::Insert)
        _state.viCommands.cursorPosition = startPos;

    switch (_speedClicks)
    {
        case 1:
            if (_state.searchMode.initiatedByDoubleClick)
                clearSearch();
            clearSelection();
            if (modifier == _settings.mouseBlockSelectionModifier)
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
        case 2:
            setSelector(
                std::make_unique<WordWiseSelection>(_selectionHelper, startPos, selectionUpdatedHelper()));
            if (_selection->extend(startPos))
                onSelectionUpdated();
            if (_settings.visualizeSelectedWord)
            {
                auto const text = extractSelectionText();
                auto const text32 = unicode::convert_to<char32_t>(string_view(text.data(), text.size()));
                setNewSearchTerm(text32, true);
            }
            break;
        case 3:
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

void Terminal::setSelector(std::unique_ptr<Selection> selector)
{
    Require(selector.get() != nullptr);
    inputLog()("Creating cell selector: {}", *selector);
    _selection = std::move(selector);
}

void Terminal::clearSelection()
{
    if (_state.inputHandler.isVisualMode())
    {
        if (!_leftMouseButtonPressed)
            // Don't clear if in visual mode and mouse wasn't used.
            return;
        _state.inputHandler.setMode(ViMode::Normal);
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

void Terminal::sendMouseMoveEvent(Modifier modifier,
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

    if (newPosition != _currentMousePosition)
    {
        // Speed-clicks are only counted when not moving the mouse in between, so reset on mouse move here.
        _speedClicks = 0;

        _currentMousePosition = newPosition;
        updateHoveringHyperlinkState();
    }

    if (!_leftMouseButtonPressed)
        return;

    auto const shouldExtendSelection = shouldExtendSelectionByMouse(newPosition, pixelPosition);

    auto relativePos = _viewport.translateScreenToGridCoordinate(newPosition);
    if (shouldExtendSelection)
    {
        _state.viCommands.cursorPosition = relativePos;
        _viewport.makeVisible(_state.viCommands.cursorPosition.line);
    }

    // Do not handle mouse-move events in sub-cell dimensions.
    if (allowPassMouseEventToApp(modifier))
    {
        if (_state.inputGenerator.generateMouseMove(
                modifier, relativePos, pixelPosition, uiHandledHint || !selectionAvailable()))
            flushInput();
        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return;
    }

    if (!selectionAvailable())
        setSelector(
            std::make_unique<LinearSelection>(_selectionHelper, relativePos, selectionUpdatedHelper()));
    else if (selector()->state() != Selection::State::Complete && shouldExtendSelection)
    {
        if (currentScreen().isCellEmpty(relativePos) && !currentScreen().compareCellTextAt(relativePos, 0x20))
            relativePos.column = ColumnOffset { 0 } + *(_settings.pageSize.columns - 1);
        _state.viCommands.cursorPosition = relativePos;
        if (_state.inputHandler.mode() != ViMode::Insert)
            _state.inputHandler.setMode(selector()->viMode());
        if (selector()->extend(relativePos))
            breakLoopAndRefreshRenderBuffer();
    }
}

bool Terminal::sendMouseReleaseEvent(Modifier modifier,
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
                    if (_state.inputHandler.mode() == ViMode::Insert)
                        selector()->complete();
                    _eventListener.onSelectionCompleted();
                    break;
                case Selection::State::Waiting: _selection.reset(); break;
                case Selection::State::Complete: break;
            }
        }
    }

    if (allowPassMouseEventToApp(modifier)
        && _state.inputGenerator.generateMouseRelease(
            modifier, button, _currentMousePosition, pixelPosition, uiHandledHint))
    {
        flushInput();

        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return true;
    }

    return true;
}

bool Terminal::sendFocusInEvent()
{
    _state.focused = true;
    breakLoopAndRefreshRenderBuffer();

    if (_state.inputGenerator.generateFocusInEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::sendFocusOutEvent()
{
    _state.focused = false;
    breakLoopAndRefreshRenderBuffer();

    if (_state.inputGenerator.generateFocusOutEvent())
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

    if (_state.inputHandler.isEditingSearch())
    {
        _state.searchMode.pattern += unicode::convert_to<char32_t>(text);
        screenUpdated();
        return;
    }

    _state.inputGenerator.generatePaste(text);
    flushInput();
}

void Terminal::sendRawInput(string_view text)
{
    if (!allowInput())
        return;

    if (_state.inputHandler.isEditingSearch())
    {
        inputLog()("Sending raw input to search input: {}", crispy::escape(text));
        _state.searchMode.pattern += unicode::convert_to<char32_t>(text);
        screenUpdated();
        return;
    }

    inputLog()("Sending raw input to stdin: {}", crispy::escape(text));
    _state.inputGenerator.generateRaw(text);
    flushInput();
}

bool Terminal::hasInput() const noexcept
{
    return !_state.inputGenerator.peek().empty();
}

void Terminal::flushInput()
{
    if (_state.inputGenerator.peek().empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    auto const input = _state.inputGenerator.peek();
    auto const rv = _pty->write(input);
    if (rv > 0)
        _state.inputGenerator.consume(rv);
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
            _state.parser.parseFragment(_currentPtyBuffer->writeAtEnd(chunk));
        }
    }

    if (!_state.modes.enabled(DECMode::BatchedRendering))
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

void Terminal::writeToScreenInternal(std::string_view vtStream)
{
    while (!vtStream.empty())
    {
        auto const chunk = lockedWriteToPtyBuffer(vtStream);
        vtStream.remove_prefix(chunk.size());
        _state.parser.parseFragment(chunk);
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
    auto const newState = _currentScreen.get().contains(_currentMousePosition)
                              ? _currentScreen.get().hyperlinkIdAt(
                                  _viewport.translateScreenToGridCoordinate(_currentMousePosition))
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

    if (_state.statusDisplayType == StatusDisplayType::Indicator)
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
    auto const _ = std::lock_guard { *this };
    resizeScreenInternal(totalPageSize, pixels);
}

void Terminal::resizeScreenInternal(PageSize totalPageSize, std::optional<ImageSize> pixels)
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
    _primaryScreen.margin() =
        Margin { Margin::Vertical { {}, mainDisplayPageSize.lines.as<LineOffset>() - 1 },
                 Margin::Horizontal { {}, mainDisplayPageSize.columns.as<ColumnOffset>() - 1 } };
    _alternateScreen.margin() = _primaryScreen.margin();

    applyPageSizeToCurrentBuffer();

    _pty->resizeScreen(mainDisplayPageSize, pixels);

    // Adjust Normal-mode's cursor in order to avoid drift when growing/shrinking in main page line count.
    if (mainDisplayPageSize.lines > oldMainDisplayPageSize.lines)
        _state.viCommands.cursorPosition.line +=
            boxed_cast<LineOffset>(mainDisplayPageSize.lines - oldMainDisplayPageSize.lines);
    else if (oldMainDisplayPageSize.lines > mainDisplayPageSize.lines)
        _state.viCommands.cursorPosition.line -=
            boxed_cast<LineOffset>(oldMainDisplayPageSize.lines - mainDisplayPageSize.lines);

    _state.viCommands.cursorPosition = clampToScreen(_state.viCommands.cursorPosition);

    verifyState();
}

void Terminal::resizeColumns(ColumnCount newColumnCount, bool clear)
{
    // DECCOLM / DECSCPP
    if (clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin({}, unbox<LineOffset>(_settings.pageSize.lines) - LineOffset(1));       // DECSTBM
        setLeftRightMargin({}, unbox<ColumnOffset>(_settings.pageSize.columns) - ColumnOffset(1)); // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(DECMode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = PageSize { _settings.pageSize.lines, newColumnCount };
    auto const pixels = cellPixelSize() * newSize;
    resizeScreen(newSize, pixels);

    requestWindowResize(newSize);
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

    Require(_state.tabs.empty() || _state.tabs.back() < unbox<ColumnOffset>(_settings.pageSize.columns));

    _currentScreen.get().verifyState();
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
}

namespace
{
    template <typename Cell>
    struct SelectionRenderer
    {
        Terminal const& term;
        ColumnOffset rightPage;
        ColumnOffset lastColumn {};
        string text {};
        string currentLine {};

        void operator()(CellLocation pos, Cell const& cell)
        {
            auto const isNewLine = pos.column < lastColumn || (pos.column == lastColumn && !text.empty());
            bool const touchesRightPage = term.isSelected({ pos.line, rightPage });
            if (isNewLine && (!term.isLineWrapped(pos.line) || !touchesRightPage))
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
            if (dynamic_cast<FullLineSelection const*>(term.selector()))
                text += '\n';
            return std::move(text);
        }
    };
} // namespace

string Terminal::extractSelectionText() const
{
    auto const _ = std::scoped_lock { *this };

    if (!_selection || _selection->state() == Selection::State::Waiting)
        return "";

    if (isPrimaryScreen())
    {
        auto se = SelectionRenderer<PrimaryScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        terminal::renderSelection(*_selection, [&](CellLocation pos) { se(pos, _primaryScreen.at(pos)); });
        return se.finish();
    }
    else
    {
        auto se = SelectionRenderer<AlternateScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        terminal::renderSelection(*_selection, [&](CellLocation pos) { se(pos, _alternateScreen.at(pos)); });
        return se.finish();
    }
}

string Terminal::extractLastMarkRange() const
{
    auto const _ = std::lock_guard { *this };

    // -1 because we always want to start extracting one line above the cursor by default.
    auto const bottomLine =
        _currentScreen.get().cursor().position.line + LineOffset(-1) + _settings.copyLastMarkRangeOffset;

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
        auto const lineText = _primaryScreen.grid().lineAt(lineNum).toUtf8Trimmed();
        text += _primaryScreen.grid().lineAt(lineNum).toUtf8Trimmed();
        text += '\n';
    }

    return text;
}

// {{{ ScreenEvents overrides
void Terminal::requestCaptureBuffer(LineCount lines, bool logical)
{
    return _eventListener.requestCaptureBuffer(lines, logical);
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
    _state.inputGenerator.generateRaw(text);

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
    _state.inputGenerator.setApplicationKeypadMode(enabled);
}

void Terminal::setBracketedPaste(bool enabled)
{
    _state.inputGenerator.setBracketedPaste(enabled);
}

void Terminal::setCursorStyle(CursorDisplay display, CursorShape shape)
{
    _settings.cursorDisplay = display;
    _settings.cursorShape = shape;
}

void Terminal::setCursorVisibility(bool /*visible*/)
{
    // don't do anything for now
}

void Terminal::setGenerateFocusEvents(bool enabled)
{
    _state.inputGenerator.setGenerateFocusEvents(enabled);
}

void Terminal::setMouseProtocol(MouseProtocol protocol, bool enabled)
{
    _state.inputGenerator.setMouseProtocol(protocol, enabled);
}

void Terminal::setMouseTransport(MouseTransport transport)
{
    _state.inputGenerator.setMouseTransport(transport);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode mode)
{
    _state.inputGenerator.setMouseWheelMode(mode);
}

void Terminal::setWindowTitle(string_view title)
{
    _state.windowTitle = title;
    _eventListener.setWindowTitle(title);
}

std::string const& Terminal::windowTitle() const noexcept
{
    return _state.windowTitle;
}

void Terminal::saveWindowTitle()
{
    _state.savedWindowTitles.push(_state.windowTitle);
}

void Terminal::restoreWindowTitle()
{
    if (!_state.savedWindowTitles.empty())
    {
        _state.windowTitle = _state.savedWindowTitles.top();
        _state.savedWindowTitles.pop();
        setWindowTitle(_state.windowTitle);
    }
}

void Terminal::setTerminalProfile(string const& configProfileName)
{
    _eventListener.setTerminalProfile(configProfileName);
}

void Terminal::useApplicationCursorKeys(bool enable)
{
    auto const keyMode = enable ? KeyMode::Application : KeyMode::Normal;
    _state.inputGenerator.setCursorKeysMode(keyMode);
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

    _state.modes.set(mode, enable);
}

void Terminal::setMode(DECMode mode, bool enable)
{
    if (!isValidDECMode(static_cast<unsigned int>(mode)))
        return;

    auto const currentModeValue = _state.modes.get(mode);

    if (currentModeValue == ModeValue::PermanentlyReset || currentModeValue == ModeValue::PermanentlySet)
    {
        if (isEnabled(currentModeValue) != enable)
        {
            terminalLog()("Attempt to change permanently {} mode {} to {}.",
                          currentModeValue == ModeValue::PermanentlySet ? "set" : "reset",
                          mode,
                          enable ? ModeValue::Set : ModeValue::Reset);
        }
        return;
    }

    switch (mode)
    {
        case DECMode::AutoWrap: _currentScreen.get().cursor().autoWrap = enable; break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!enable)
            {
                currentScreen().margin().horizontal =
                    Margin::Horizontal { ColumnOffset(0),
                                         boxed_cast<ColumnOffset>(_settings.pageSize.columns - 1) };
                _supportedVTSequences.enableSequence(SCOSC);
                _supportedVTSequences.disableSequence(DECSLRM);
            }
            else
            {
                _supportedVTSequences.enableSequence(DECSLRM);
                _supportedVTSequences.disableSequence(SCOSC);
            }
            break;
        case DECMode::Origin: _currentScreen.get().cursor().originMode = enable; break;
        case DECMode::Columns132:
            if (!isModeEnabled(DECMode::AllowColumns80to132))
                break;
            if (enable != isModeEnabled(DECMode::Columns132))
            {
                auto const clear = enable != isModeEnabled(mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case DECMode::BatchedRendering:
            if (_state.modes.enabled(DECMode::BatchedRendering) != enable)
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
            _state.inputGenerator.setPassiveMouseTracking(enable);
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
        case DECMode::UsePrivateColorRegisters: _state.usePrivateColorRegisters = enable; break;
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
                _currentScreen.get().saveCursor();
            else
                _currentScreen.get().restoreCursor();
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

    _state.modes.set(mode, enable);
}

void Terminal::setTopBottomMargin(optional<LineOffset> top, optional<LineOffset> bottom)
{
    auto const defaultTop = LineOffset(0);
    auto const defaultBottom = boxed_cast<LineOffset>(pageSize().lines) - 1;
    auto const sanitizedTop = std::max(defaultTop, top.value_or(defaultTop));
    auto const sanitizedBottom = std::min(defaultBottom, bottom.value_or(defaultBottom));

    if (sanitizedTop < sanitizedBottom)
    {
        currentScreen().margin().vertical.from = sanitizedTop;
        currentScreen().margin().vertical.to = sanitizedBottom;
    }
}

void Terminal::setLeftRightMargin(optional<ColumnOffset> left, optional<ColumnOffset> right)
{
    if (isModeEnabled(DECMode::LeftRightMargin))
    {
        auto const defaultLeft = ColumnOffset(0);
        auto const defaultRight = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
        auto const sanitizedRight = std::min(right.value_or(defaultRight), defaultRight);
        auto const sanitizedLeft = std::max(left.value_or(defaultLeft), defaultLeft);
        if (sanitizedLeft < sanitizedRight)
        {
            currentScreen().margin().horizontal.from = sanitizedLeft;
            currentScreen().margin().horizontal.to = sanitizedRight;
        }
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
    _currentScreen.get().moveCursorTo(line, column);
}

void Terminal::softReset()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setGraphicsRendition(GraphicsRendition::Reset);    // SGR
    _currentScreen.get().resetSavedCursorState();      // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true);             // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);                   // DECOM
    setMode(AnsiMode::KeyboardAction, false);          // KAM
    setMode(DECMode::AutoWrap, false);                 // DECAWM
    setMode(AnsiMode::Insert, false);                  // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, boxed_cast<LineOffset>(_settings.pageSize.lines) - LineOffset(1));       // DECSTBM
    setLeftRightMargin({}, boxed_cast<ColumnOffset>(_settings.pageSize.columns) - ColumnOffset(1)); // DECRLM

    _currentScreen.get().cursor().hyperlink = {};
    _state.colorPalette = _state.defaultColorPalette;

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
        _currentScreen.get().cursor().graphicsRendition = {};
    else
        _currentScreen.get().cursor().graphicsRendition.flags =
            CellUtil::makeCellFlags(rendition, _currentScreen.get().cursor().graphicsRendition.flags);
}

void Terminal::setForegroundColor(Color color)
{
    _currentScreen.get().cursor().graphicsRendition.foregroundColor = color;
}

void Terminal::setBackgroundColor(Color color)
{
    _currentScreen.get().cursor().graphicsRendition.backgroundColor = color;
}

void Terminal::setUnderlineColor(Color color)
{
    _currentScreen.get().cursor().graphicsRendition.underlineColor = color;
}

void Terminal::hardReset()
{
    // TODO: make use of _factorySettings
    setScreen(ScreenType::Primary);

    // Ensure that the alternate screen buffer is having the correct size, as well.
    applyPageSizeToMainDisplay(ScreenType::Alternate);

    _state.modes = Modes {};
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::VisibleCursor, true);

    for (auto const& [mode, frozen] : _settings.frozenModes)
        freezeMode(mode, frozen);

    _primaryScreen.hardReset();
    _alternateScreen.hardReset();
    _hostWritableStatusLineScreen.hardReset();
    _indicatorStatusScreen.hardReset();

    _state.imagePool.clear();
    _state.tabs.clear();

    _state.colorPalette = _state.defaultColorPalette;

    _hostWritableStatusLineScreen.margin() = Margin {
        Margin::Vertical { {}, boxed_cast<LineOffset>(_hostWritableStatusLineScreen.pageSize().lines) - 1 },
        Margin::Horizontal { {},
                             boxed_cast<ColumnOffset>(_hostWritableStatusLineScreen.pageSize().columns) - 1 }
    };
    _hostWritableStatusLineScreen.verifyState();

    setActiveStatusDisplay(ActiveStatusDisplay::Main);
    _hostWritableStatusLineScreen.clearScreen();
    _hostWritableStatusLineScreen.updateCursorIterator();

    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    _primaryScreen.margin() =
        Margin { Margin::Vertical { {}, boxed_cast<LineOffset>(mainDisplayPageSize.lines) - 1 },
                 Margin::Horizontal { {}, boxed_cast<ColumnOffset>(mainDisplayPageSize.columns) - 1 } };
    _primaryScreen.verifyState();

    _alternateScreen.margin() =
        Margin { Margin::Vertical { {}, boxed_cast<LineOffset>(mainDisplayPageSize.lines) - 1 },
                 Margin::Horizontal { {}, boxed_cast<ColumnOffset>(mainDisplayPageSize.columns) - 1 } };
    alternateScreen().margin() = _primaryScreen.margin();
    // NB: We do *NOT* verify alternate screen, because the page size would probably fail as it is
    // designed to be adjusted when the given screen is activated.

    setStatusDisplay(_factorySettings.statusDisplayType);

    _state.inputGenerator.reset();
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
    if (type == _state.screenType)
        return;

    switch (type)
    {
        case ScreenType::Primary:
            _currentScreen = _primaryScreen;
            setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
            break;
        case ScreenType::Alternate:
            _currentScreen = _alternateScreen;
            if (isModeEnabled(DECMode::MouseAlternateScroll))
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
    }

    _state.screenType = type;

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

    // truncating tabs
    while (!_state.tabs.empty() && _state.tabs.back() >= unbox<ColumnOffset>(_settings.pageSize.columns))
        _state.tabs.pop_back();

    // verifyState();
}

void Terminal::discardImage(Image const& image)
{
    _eventListener.discardImage(image);
}

void Terminal::markCellDirty(CellLocation position) noexcept
{
    if (_state.activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!_selection)
        return;

    if (_selection->contains(position))
        clearSelection();
}

void Terminal::markRegionDirty(Rect area) noexcept
{
    if (_state.activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!_selection)
        return;

    if (_selection->intersects(area))
        clearSelection();
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
    _state.viCommands.cursorPosition.line -= n;

    // Adjust viewport accordingly to make it fixed at the scroll-offset as if nothing has happened.
    if (viewport().scrolled())
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
    _state.terminalId = id;
    _supportedVTSequences.reset(id);
}

void Terminal::setStatusDisplay(StatusDisplayType statusDisplayType)
{
    assert(&_currentScreen.get() != &_indicatorStatusScreen);

    if (_state.statusDisplayType == statusDisplayType)
        return;

    markScreenDirty();

    auto const statusLineVisibleBefore = _state.statusDisplayType != StatusDisplayType::None;
    auto const statusLineVisibleAfter = statusDisplayType != StatusDisplayType::None;
    _state.statusDisplayType = statusDisplayType;

    if (statusLineVisibleBefore != statusLineVisibleAfter)
        resizeScreenInternal(_settings.pageSize, nullopt);
}

void Terminal::setActiveStatusDisplay(ActiveStatusDisplay activeDisplay)
{
    if (_state.activeStatusDisplay == activeDisplay)
        return;

    _state.activeStatusDisplay = activeDisplay;

    // clang-format off
    switch (activeDisplay)
    {
        case ActiveStatusDisplay::Main:
            switch (_state.screenType)
            {
                case ScreenType::Primary:
                    _currentScreen = _primaryScreen;
                    break;
                case ScreenType::Alternate:
                    _currentScreen = _alternateScreen;
                    break;
            }
            break;
        case ActiveStatusDisplay::StatusLine:
            _currentScreen = _hostWritableStatusLineScreen;
            break;
        case ActiveStatusDisplay::IndicatorStatusLine:
            _currentScreen = _indicatorStatusScreen;
            break;
    }
    // clang-format on
}

void Terminal::pushStatusDisplay(StatusDisplayType type)
{
    // Only remember the outermost saved status display type.
    if (!_state.savedStatusDisplayType)
        _state.savedStatusDisplayType = _state.statusDisplayType;

    setStatusDisplay(type);
}

void Terminal::popStatusDisplay()
{
    if (!_state.savedStatusDisplayType)
        return;

    setStatusDisplay(_state.savedStatusDisplayType.value());
    _state.savedStatusDisplayType.reset();
}

void Terminal::setAllowInput(bool enabled)
{
    setMode(AnsiMode::KeyboardAction, !enabled);
}

bool Terminal::setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick)
{
    _state.searchMode.initiatedByDoubleClick = initiatedByDoubleClick;

    if (_state.searchMode.pattern == text)
        return false;

    _state.searchMode.pattern = std::move(text);
    return true;
}

optional<CellLocation> Terminal::searchReverse(u32string text, CellLocation searchPosition)
{
    if (!setNewSearchTerm(std::move(text), false))
        return searchPosition;

    return searchReverse(searchPosition);
}

optional<CellLocation> Terminal::search(std::u32string text,
                                        CellLocation searchPosition,
                                        bool initiatedByDoubleClick)
{
    if (!setNewSearchTerm(std::move(text), initiatedByDoubleClick))
        return searchPosition;

    return search(searchPosition);
}

optional<CellLocation> Terminal::search(CellLocation searchPosition)
{
    auto const searchText = u32string_view(_state.searchMode.pattern);
    auto const matchLocation = currentScreen().search(searchText, searchPosition);

    if (matchLocation)
        viewport().makeVisibleWithinSafeArea(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

void Terminal::clearSearch()
{
    _state.searchMode.pattern.clear();
    _state.searchMode.initiatedByDoubleClick = false;
}

bool Terminal::wordDelimited(CellLocation position) const noexcept
{
    // Word selection may be off by one
    position.column = std::min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    if (isPrimaryScreen())
        return _primaryScreen.grid().cellEmptyOrContainsOneOf(position, _settings.wordDelimiters);
    else
        return _alternateScreen.grid().cellEmptyOrContainsOneOf(position, _settings.wordDelimiters);
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
    auto const searchText = u32string_view(_state.searchMode.pattern);
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
        range = RectangularHighlight { points.first, points.second };
    }
    _highlightRange = highlightRange;
    _eventListener.updateHighlights();
}

constexpr auto MagicStackTopId = size_t { 0 };

void Terminal::pushColorPalette(size_t slot)
{
    if (slot > MaxColorPaletteSaveStackSize)
        return;

    auto const index = slot == MagicStackTopId
                           ? _state.savedColorPalettes.empty() ? 0 : _state.savedColorPalettes.size() - 1
                           : slot - 1;

    if (index >= _state.savedColorPalettes.size())
        _state.savedColorPalettes.resize(index + 1);

    // That's a totally weird idea.
    // Looking at the xterm's source code, and simply mimmicking their semantics without questioning,
    // simply to stay compatible (sadface).
    if (slot != MagicStackTopId && _state.lastSavedColorPalette < _state.savedColorPalettes.size())
        _state.lastSavedColorPalette = _state.savedColorPalettes.size();

    _state.savedColorPalettes[index] = _state.colorPalette;
}

void Terminal::reportColorPaletteStack()
{
    // XTREPORTCOLORS
    reply(fmt::format("\033[{};{}#Q", _state.savedColorPalettes.size(), _state.lastSavedColorPalette));
}

void Terminal::popColorPalette(size_t slot)
{
    if (_state.savedColorPalettes.empty())
        return;

    auto const index = slot == MagicStackTopId ? _state.savedColorPalettes.size() - 1 : slot - 1;

    _state.colorPalette = _state.savedColorPalettes[index];
    if (slot == MagicStackTopId)
        _state.savedColorPalettes.pop_back();
}

// {{{ TraceHandler
TraceHandler::TraceHandler(Terminal& terminal): _terminal { terminal }
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
    _pendingSequences.emplace_back(CodepointSequence { codepoints, cellCount });
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
        if (auto const* functionDefinition = seq->functionDefinition(_terminal.activeSequences()))
            fmt::print("\t{:<20} ; {:<18} ; {}\n",
                       seq->text(),
                       functionDefinition->mnemonic,
                       functionDefinition->comment);
        else
            fmt::print("\t{:<20}\n", seq->text());
        _terminal.activeDisplay().processSequence(*seq);
    }
    else if (auto const* codepoint = std::get_if<char32_t>(&pendingSequence))
    {
        fmt::print("\t'{}'\n", unicode::convert_to<char>(*codepoint));
        _terminal.activeDisplay().writeText(*codepoint);
    }
    else if (auto const* codepoints = std::get_if<CodepointSequence>(&pendingSequence))
    {
        fmt::print("\t\"{}\"   ; {} cells\n", codepoints->text, codepoints->cellCount);
        _terminal.activeDisplay().writeText(codepoints->text, codepoints->cellCount);
    }
}
// }}}

} // namespace terminal
