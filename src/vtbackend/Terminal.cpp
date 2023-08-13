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
#include <vtbackend/ControlCode.h>
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
#include <crispy/stdfs.h>
#include <crispy/utils.h>

#include <fmt/chrono.h>

#include <sys/types.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <utility>
#include <variant>

#include <libunicode/convert.h>

using crispy::Size;
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

    string_view modeString(vi_mode mode) noexcept
    {
        switch (mode)
        {
            case vi_mode::Normal: return "NORMAL"sv;
            case vi_mode::Insert: return "INSERT"sv;
            case vi_mode::Visual: return "VISUAL"sv;
            case vi_mode::VisualLine: return "VISUAL LINE"sv;
            case vi_mode::VisualBlock: return "VISUAL BLOCK"sv;
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

    constexpr cell_location raiseToMinimum(cell_location location, line_offset minimumLine) noexcept
    {
        return cell_location { std::max(location.line, minimumLine), location.column };
    }

} // namespace
// }}}

Terminal::Terminal(events& eventListener,
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
    setMode(dec_mode::AutoWrap, true);
    setMode(dec_mode::VisibleCursor, true);
    setMode(dec_mode::Unicode, true);
    setMode(dec_mode::TextReflow, true);
    setMode(dec_mode::SixelCursorNextToGraphic, true);
#endif
}

void Terminal::setRefreshRate(RefreshRate refreshRate)
{
    _settings.refreshRate = refreshRate;
    _refreshInterval = RefreshInterval { refreshRate };
}

void Terminal::setLastMarkRangeOffset(line_offset value) noexcept
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
        if (PtyInLog)
            PtyInLog()("Only {} bytes left in TBO. Allocating new buffer from pool.",
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
    switch (_state.executionMode)
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

        TerminalLog()("PTY read failed. {}", strerror(errno));
        _pty->close();
        return false;
    }
    string_view const buf = std::get<0>(*readResult);
    _state.usingStdoutFastPipe = std::get<1>(*readResult);

    if (buf.empty())
    {
        TerminalLog()("PTY read returned with zero bytes. Closing PTY.");
        _pty->close();
        return true;
    }

    {
        auto const _ = std::lock_guard { *this };
        _state.parser.parseFragment(buf);
    }

    if (!_state.modes.enabled(dec_mode::BatchedRendering))
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

    switch (_renderBuffer.state)
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

PageSize Terminal::selection_helper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::selection_helper::wordDelimited(cell_location pos) const noexcept
{
    return terminal->wordDelimited(pos);
}

bool Terminal::selection_helper::wrappedLine(line_offset line) const noexcept
{
    return terminal->isLineWrapped(line);
}

bool Terminal::selection_helper::cellEmpty(cell_location pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<column_offset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().isCellEmpty(pos);
}

int Terminal::selection_helper::cellWidth(cell_location pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<column_offset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().cellWidthAt(pos);
}

/**
 * Sets the hyperlink into hovering state if mouse is currently hovering it
 * and unsets the state when the object is being destroyed.
 */
struct ScopedHyperlinkHover
{
    std::shared_ptr<hyperlink_info const> href;

    ScopedHyperlinkHover(Terminal const& terminal, ScreenBase const& /*screen*/):
        href { terminal.tryGetHoveringHyperlink() }
    {
        if (href)
            href->state = hyperlink_state::Hover; // TODO: Left-Ctrl pressed?
    }

    ~ScopedHyperlinkHover()
    {
        if (href)
            href->state = hyperlink_state::Inactive;
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

    auto baseLine = line_offset(0);

    if (_settings.statusDisplayPosition == status_display_position::Top)
        baseLine += fillRenderBufferStatusLine(output, includeSelection, baseLine).as<line_offset>();

    auto const hoveringHyperlinkGuard = ScopedHyperlinkHover { *this, _currentScreen };
    auto const mainDisplayReverseVideo = isModeEnabled(terminal::dec_mode::ReverseVideo);
    auto const highlightSearchMatches =
        _state.searchMode.pattern.empty() ? highlight_search_matches::No : highlight_search_matches::Yes;

    auto const theCursorPosition =
        optional<cell_location> { inputHandler().mode() == vi_mode::Insert
                                      ? (isModeEnabled(dec_mode::VisibleCursor)
                                             ? optional<cell_location> { currentScreen().cursor().position }
                                             : nullopt)
                                      : state().viCommands.cursorPosition };

    if (isPrimaryScreen())
        _lastRenderPassHints =
            _primaryScreen.render(RenderBufferBuilder<PrimaryScreenCell> { *this,
                                                                           output,
                                                                           baseLine,
                                                                           mainDisplayReverseVideo,
                                                                           highlight_search_matches::Yes,
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
                                                                               highlight_search_matches::Yes,
                                                                               _inputMethodData,
                                                                               theCursorPosition,
                                                                               includeSelection },
                                    _viewport.scrollOffset(),
                                    highlightSearchMatches);

    if (_settings.statusDisplayPosition == status_display_position::Bottom)
    {
        baseLine += pageSize().lines.as<line_offset>();
        fillRenderBufferStatusLine(output, includeSelection, baseLine);
    }
}

LineCount Terminal::fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, line_offset base)
{
    auto const mainDisplayReverseVideo = isModeEnabled(terminal::dec_mode::ReverseVideo);
    switch (_state.statusDisplayType)
    {
        case terminal::status_display_type::None:
            //.
            return LineCount(0);
        case terminal::status_display_type::Indicator:
            updateIndicatorStatusLine();
            _indicatorStatusScreen.render(
                RenderBufferBuilder<StatusDisplayCell> { *this,
                                                         output,
                                                         base,
                                                         !mainDisplayReverseVideo,
                                                         highlight_search_matches::No,
                                                         input_method_data {},
                                                         nullopt,
                                                         includeSelection },
                scroll_offset(0));
            return _indicatorStatusScreen.pageSize().lines;
        case terminal::status_display_type::HostWritable:
            _hostWritableStatusLineScreen.render(
                RenderBufferBuilder<StatusDisplayCell> { *this,
                                                         output,
                                                         base,
                                                         !mainDisplayReverseVideo,
                                                         highlight_search_matches::No,
                                                         input_method_data {},
                                                         nullopt,
                                                         includeSelection },
                scroll_offset(0));
            return _hostWritableStatusLineScreen.pageSize().lines;
    }
    crispy::unreachable();
}
// }}}

void Terminal::updateIndicatorStatusLine()
{
    Require(_state.activeStatusDisplay != active_status_display::IndicatorStatusLine);

    auto const _ = crispy::finally { [this, savedActiveStatusDisplay = _state.activeStatusDisplay]() {
        // Cleaning up.
        setActiveStatusDisplay(savedActiveStatusDisplay);
        verifyState();
    } };

    auto const colors =
        _state.focused ? colorPalette().indicatorStatusLine : colorPalette().indicatorStatusLineInactive;

    setActiveStatusDisplay(active_status_display::IndicatorStatusLine);

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
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = bright_color::Red;
        _indicatorStatusScreen.cursor().graphicsRendition.flags |= cell_flags::Bold;
        _indicatorStatusScreen.writeTextFromExternal(" (PROTECTED)");
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
        _indicatorStatusScreen.cursor().graphicsRendition.flags &= ~cell_flags::Bold;
    }

    if (_state.executionMode != ExecutionMode::Normal)
    {
        _indicatorStatusScreen.writeTextFromExternal(" | ");
        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = bright_color::Yellow;
        _indicatorStatusScreen.cursor().graphicsRendition.flags |= cell_flags::Bold;
        _indicatorStatusScreen.writeTextFromExternal("TRACING");
        if (!_traceHandler.pendingSequences().empty())
            _indicatorStatusScreen.writeTextFromExternal(
                fmt::format(" (#{}): {}",
                            _traceHandler.pendingSequences().size(),
                            _traceHandler.pendingSequences().front()));

        _indicatorStatusScreen.cursor().graphicsRendition.foregroundColor = colors.foreground;
        _indicatorStatusScreen.cursor().graphicsRendition.flags &= ~cell_flags::Bold;
    }

    // TODO: Disabled for now, but generally I want that functionality, but configurable somehow.
    auto constexpr indicatorLineShowCodepoints = false;
    if (indicatorLineShowCodepoints)
    {
        auto const cursorPosition = _state.inputHandler.mode() == vi_mode::Insert
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
        if (get_viewport().scrollOffset().value)
            rightString +=
                fmt::format("{}/{}", get_viewport().scrollOffset(), _primaryScreen.historyLineCount());
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
            column_offset::cast_from(_indicatorStatusScreen.pageSize().columns)
            - column_offset::cast_from(rightString.size()) - column_offset(1);
        _indicatorStatusScreen.updateCursorIterator();

        _indicatorStatusScreen.writeTextFromExternal(rightString);
    }
}

bool Terminal::sendKeyPressEvent(key key, modifier modifier, timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    if (allowInput() && _state.inputHandler.sendKeyPressEvent(key, modifier))
        return true;

    // Early exit if KAM is enabled.
    if (isModeEnabled(ansi_mode::KeyboardAction))
        return true;

    _viewport.scrollToBottom();
    bool const success = _state.inputGenerator.generate(key, modifier);
    flushInput();
    _viewport.scrollToBottom();
    return success;
}

bool Terminal::sendCharPressEvent(char32_t ch, modifier modifier, timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    // Early exit if KAM is enabled.
    if (isModeEnabled(ansi_mode::KeyboardAction))
        return true;

    if (_state.inputHandler.sendCharPressEvent(ch, modifier))
        return true;

    auto const success = _state.inputGenerator.generate(ch, modifier);

    flushInput();
    _viewport.scrollToBottom();
    return success;
}

bool Terminal::sendMousePressEvent(modifier modifier,
                                   mouse_button button,
                                   pixel_coordinate pixelPosition,
                                   bool uiHandledHint)
{
    if (button == mouse_button::Left)
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
    return eventHandledByApp && !isModeEnabled(dec_mode::MousePassiveTracking);
}

bool Terminal::handleMouseSelection(modifier modifier)
{
    verifyState();

    double const diff_ms = chrono::duration<double, std::milli>(_currentTime - _lastClick).count();
    _lastClick = _currentTime;
    _speedClicks = (diff_ms >= 0.0 && diff_ms <= 750.0 ? _speedClicks : 0) % 3 + 1;

    auto const startPos = cell_location {
        _currentMousePosition.line - boxed_cast<line_offset>(_viewport.scrollOffset()),
        _currentMousePosition.column,
    };

    if (_state.inputHandler.mode() != vi_mode::Insert)
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
    InputLog()("Creating cell selector: {}", *selector);
    _selection = std::move(selector);
}

void Terminal::clearSelection()
{
    if (_state.inputHandler.isVisualMode())
    {
        if (!_leftMouseButtonPressed)
            // Don't clear if in visual mode and mouse wasn't used.
            return;
        _state.inputHandler.setMode(vi_mode::Normal);
    }

    if (!_selection)
        return;

    InputLog()("Clearing selection.");
    _selection.reset();

    onSelectionUpdated();

    breakLoopAndRefreshRenderBuffer();
}

bool Terminal::shouldExtendSelectionByMouse(cell_location newPosition,
                                            pixel_coordinate pixelPosition) const noexcept
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

void Terminal::sendMouseMoveEvent(modifier modifier,
                                  cell_location newPosition,
                                  pixel_coordinate pixelPosition,
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
        if (!isModeEnabled(dec_mode::MousePassiveTracking))
            return;
    }

    if (!selectionAvailable())
        setSelector(
            std::make_unique<LinearSelection>(_selectionHelper, relativePos, selectionUpdatedHelper()));
    else if (selector()->state() != Selection::State::Complete && shouldExtendSelection)
    {
        if (currentScreen().isCellEmpty(relativePos) && !currentScreen().compareCellTextAt(relativePos, 0x20))
            relativePos.column = column_offset { 0 } + *(_settings.pageSize.columns - 1);
        _state.viCommands.cursorPosition = relativePos;
        if (_state.inputHandler.mode() != vi_mode::Insert)
            _state.inputHandler.setMode(selector()->viMode());
        if (selector()->extend(relativePos))
            breakLoopAndRefreshRenderBuffer();
    }
}

bool Terminal::sendMouseReleaseEvent(modifier modifier,
                                     mouse_button button,
                                     pixel_coordinate pixelPosition,
                                     bool uiHandledHint)
{
    verifyState();

    if (button == mouse_button::Left)
    {
        _leftMouseButtonPressed = false;
        if (selectionAvailable())
        {
            switch (selector()->state())
            {
                case Selection::State::InProgress:
                    if (_state.inputHandler.mode() == vi_mode::Insert)
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

        if (!isModeEnabled(dec_mode::MousePassiveTracking))
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
        InputLog()("Sending raw input to search input: {}", crispy::escape(text));
        _state.searchMode.pattern += unicode::convert_to<char32_t>(text);
        screenUpdated();
        return;
    }

    InputLog()("Sending raw input to stdin: {}", crispy::escape(text));
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
    auto const rv = _pty->write(input.data(), input.size());
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

    if (!_state.modes.enabled(dec_mode::BatchedRendering))
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
    if (_settings.cursorDisplay == cursor_display::Steady)
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
                              : hyperlink_id {};

    auto const oldState = _hoveringHyperlinkId.exchange(newState);

    if (newState != oldState)
        renderBufferUpdated();
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    auto nextBlink = chrono::milliseconds::max();
    if ((isModeEnabled(dec_mode::VisibleCursor) && _settings.cursorDisplay == cursor_display::Blink)
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

    if (_state.statusDisplayType == status_display_type::Indicator)
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

void Terminal::resizeScreen(PageSize totalPageSize, optional<image_size> pixels)
{
    auto const _ = std::lock_guard { *this };
    resizeScreenInternal(totalPageSize, pixels);
}

void Terminal::resizeScreenInternal(PageSize totalPageSize, std::optional<image_size> pixels)
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
    _primaryScreen.getMargin() =
        margin { margin::vertical { {}, mainDisplayPageSize.lines.as<line_offset>() - 1 },
                 margin::horizontal { {}, mainDisplayPageSize.columns.as<column_offset>() - 1 } };
    _alternateScreen.getMargin() = _primaryScreen.getMargin();

    applyPageSizeToCurrentBuffer();

    _pty->resizeScreen(mainDisplayPageSize, pixels);

    // Adjust Normal-mode's cursor in order to avoid drift when growing/shrinking in main page line count.
    if (mainDisplayPageSize.lines > oldMainDisplayPageSize.lines)
        _state.viCommands.cursorPosition.line +=
            boxed_cast<line_offset>(mainDisplayPageSize.lines - oldMainDisplayPageSize.lines);
    else if (oldMainDisplayPageSize.lines > mainDisplayPageSize.lines)
        _state.viCommands.cursorPosition.line -=
            boxed_cast<line_offset>(oldMainDisplayPageSize.lines - mainDisplayPageSize.lines);

    _state.viCommands.cursorPosition = clampToScreen(_state.viCommands.cursorPosition);

    verifyState();
}

void Terminal::resizeColumns(ColumnCount newColumnCount, bool clear)
{
    // DECCOLM / DECSCPP
    if (clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin({}, unbox<line_offset>(_settings.pageSize.lines) - line_offset(1)); // DECSTBM
        setLeftRightMargin({}, unbox<column_offset>(_settings.pageSize.columns) - column_offset(1)); // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(dec_mode::LeftRightMargin, false); // DECSLRM

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

void Terminal::setCursorDisplay(cursor_display display)
{
    _settings.cursorDisplay = display;
}

void Terminal::setCursorShape(cursor_shape shape)
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
        column_offset rightPage;
        column_offset lastColumn {};
        string text {};
        string currentLine {};

        void operator()(cell_location pos, Cell const& cell)
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
        auto se = SelectionRenderer<PrimaryScreenCell> { *this, pageSize().columns.as<column_offset>() - 1 };
        terminal::renderSelection(*_selection, [&](cell_location pos) { se(pos, _primaryScreen.at(pos)); });
        return se.finish();
    }
    else
    {
        auto se =
            SelectionRenderer<AlternateScreenCell> { *this, pageSize().columns.as<column_offset>() - 1 };
        terminal::renderSelection(*_selection, [&](cell_location pos) { se(pos, _alternateScreen.at(pos)); });
        return se.finish();
    }
}

string Terminal::extractLastMarkRange() const
{
    auto const _ = std::lock_guard { *this };

    // -1 because we always want to start extracting one line above the cursor by default.
    auto const bottomLine =
        _currentScreen.get().cursor().position.line + line_offset(-1) + _settings.copyLastMarkRangeOffset;

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

void Terminal::bufferChanged(screen_type type)
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

font_def Terminal::getFontDef()
{
    return _eventListener.getFontDef();
}

void Terminal::setFontDef(font_def const& fontDef)
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
}

void Terminal::requestWindowResize(PageSize size)
{
    _eventListener.requestWindowResize(size.lines, size.columns);
}

void Terminal::requestWindowResize(image_size size)
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

void Terminal::setCursorStyle(cursor_display display, cursor_shape shape)
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

void Terminal::setMouseProtocol(mouse_protocol protocol, bool enabled)
{
    _state.inputGenerator.setMouseProtocol(protocol, enabled);
}

void Terminal::setMouseTransport(mouse_transport transport)
{
    _state.inputGenerator.setMouseTransport(transport);
}

void Terminal::setMouseWheelMode(input_generator::mouse_wheel_mode mode)
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
    auto const keyMode = enable ? key_mode::Application : key_mode::Normal;
    _state.inputGenerator.setCursorKeysMode(keyMode);
}

void Terminal::setMode(ansi_mode mode, bool enable)
{
    if (!isValidAnsiMode(static_cast<unsigned int>(mode)))
        return;

    if (mode == ansi_mode::KeyboardAction)
    {
        if (enable)
            pushStatusDisplay(status_display_type::Indicator);
        else
            popStatusDisplay();
    }

    _state.modes.set(mode, enable);
}

void Terminal::setMode(dec_mode mode, bool enable)
{
    if (!isValidDECMode(static_cast<unsigned int>(mode)))
        return;

    switch (mode)
    {
        case dec_mode::AutoWrap: _currentScreen.get().cursor().autoWrap = enable; break;
        case dec_mode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!enable)
                currentScreen().getMargin().hori =
                    margin::horizontal { column_offset(0),
                                         boxed_cast<column_offset>(_settings.pageSize.columns - 1) };
            break;
        case dec_mode::Origin: _currentScreen.get().cursor().originMode = enable; break;
        case dec_mode::Columns132:
            if (!isModeEnabled(dec_mode::AllowColumns80to132))
                break;
            if (enable != isModeEnabled(dec_mode::Columns132))
            {
                auto const clear = enable != isModeEnabled(mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case dec_mode::BatchedRendering:
            if (_state.modes.enabled(dec_mode::BatchedRendering) != enable)
                synchronizedOutput(enable);
            break;
        case dec_mode::TextReflow:
            if (_settings.primaryScreen.allowReflowOnResize && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = enable ? line_offset(0) : currentScreen().cursor().position.line;
                for (auto line = startLine; line < boxed_cast<line_offset>(_settings.pageSize.lines); ++line)
                    _primaryScreen.grid().lineAt(line).setWrappable(enable);
            }
            break;
        case dec_mode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(enable);
            break;
        case dec_mode::UseAlternateScreen:
            if (enable)
                setScreen(screen_type::Alternate);
            else
                setScreen(screen_type::Primary);
            break;
        case dec_mode::UseApplicationCursorKeys:
            useApplicationCursorKeys(enable);
            if (isAlternateScreen())
            {
                if (enable)
                    setMouseWheelMode(input_generator::mouse_wheel_mode::ApplicationCursorKeys);
                else
                    setMouseWheelMode(input_generator::mouse_wheel_mode::NormalCursorKeys);
            }
            break;
        case dec_mode::BracketedPaste: setBracketedPaste(enable); break;
        case dec_mode::MouseSGR:
            if (enable)
                setMouseTransport(mouse_transport::SGR);
            else
                setMouseTransport(mouse_transport::Default);
            break;
        case dec_mode::MouseExtended: setMouseTransport(mouse_transport::Extended); break;
        case dec_mode::MouseURXVT: setMouseTransport(mouse_transport::URXVT); break;
        case dec_mode::MousePassiveTracking:
            _state.inputGenerator.setPassiveMouseTracking(enable);
            setMode(dec_mode::MouseSGR, enable);                    // SGR is required.
            setMode(dec_mode::MouseProtocolButtonTracking, enable); // ButtonTracking is default
            break;
        case dec_mode::MouseSGRPixels:
            if (enable)
                setMouseTransport(mouse_transport::SGRPixels);
            else
                setMouseTransport(mouse_transport::Default);
            break;
        case dec_mode::MouseAlternateScroll:
            if (enable)
                setMouseWheelMode(input_generator::mouse_wheel_mode::ApplicationCursorKeys);
            else
                setMouseWheelMode(input_generator::mouse_wheel_mode::NormalCursorKeys);
            break;
        case dec_mode::FocusTracking: setGenerateFocusEvents(enable); break;
        case dec_mode::UsePrivateColorRegisters: _state.usePrivateColorRegisters = enable; break;
        case dec_mode::VisibleCursor: setCursorVisibility(enable); break;
        case dec_mode::MouseProtocolX10: setMouseProtocol(mouse_protocol::X10, enable); break;
        case dec_mode::MouseProtocolNormalTracking:
            setMouseProtocol(mouse_protocol::NormalTracking, enable);
            break;
        case dec_mode::MouseProtocolHighlightTracking:
            setMouseProtocol(mouse_protocol::HighlightTracking, enable);
            break;
        case dec_mode::MouseProtocolButtonTracking:
            setMouseProtocol(mouse_protocol::ButtonTracking, enable);
            break;
        case dec_mode::MouseProtocolAnyEventTracking:
            setMouseProtocol(mouse_protocol::AnyEventTracking, enable);
            break;
        case dec_mode::SaveCursor:
            if (enable)
                _currentScreen.get().saveCursor();
            else
                _currentScreen.get().restoreCursor();
            break;
        case dec_mode::ExtendedAltScreen:
            if (enable)
            {
                setMode(dec_mode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(dec_mode::UseAlternateScreen, false);
                // NB: The cursor position doesn't need to be restored,
                // because it's local to the screen buffer.
            }
            break;
        default: break;
    }

    _state.modes.set(mode, enable);
}

void Terminal::setTopBottomMargin(optional<line_offset> top, optional<line_offset> bottom)
{
    auto const defaultTop = line_offset(0);
    auto const defaultBottom = boxed_cast<line_offset>(_settings.pageSize.lines) - 1;
    auto const sanitizedTop = std::max(defaultTop, top.value_or(defaultTop));
    auto const sanitizedBottom = std::min(defaultBottom, bottom.value_or(defaultBottom));

    if (top < bottom)
    {
        currentScreen().getMargin().vert.from = sanitizedTop;
        currentScreen().getMargin().vert.to = sanitizedBottom;
    }
}

void Terminal::setLeftRightMargin(optional<column_offset> left, optional<column_offset> right)
{
    if (isModeEnabled(dec_mode::LeftRightMargin))
    {
        auto const defaultLeft = column_offset(0);
        auto const defaultRight = boxed_cast<column_offset>(_settings.pageSize.columns) - 1;
        auto const sanitizedRight = std::min(right.value_or(defaultRight), defaultRight);
        auto const sanitizedLeft = std::max(left.value_or(defaultLeft), defaultLeft);
        if (left < right)
        {
            currentScreen().getMargin().hori.from = sanitizedLeft;
            currentScreen().getMargin().hori.to = sanitizedRight;
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

void Terminal::moveCursorTo(line_offset line, column_offset column)
{
    _currentScreen.get().moveCursorTo(line, column);
}

void Terminal::softReset()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(dec_mode::BatchedRendering, false);
    setMode(dec_mode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setGraphicsRendition(graphics_rendition::Reset);    // SGR
    _currentScreen.get().resetSavedCursorState();       // DECSC (Save cursor state)
    setMode(dec_mode::VisibleCursor, true);             // DECTCEM (Text cursor enable)
    setMode(dec_mode::Origin, false);                   // DECOM
    setMode(ansi_mode::KeyboardAction, false);          // KAM
    setMode(dec_mode::AutoWrap, false);                 // DECAWM
    setMode(ansi_mode::Insert, false);                  // IRM
    setMode(dec_mode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, boxed_cast<line_offset>(_settings.pageSize.lines) - line_offset(1)); // DECSTBM
    setLeftRightMargin({},
                       boxed_cast<column_offset>(_settings.pageSize.columns) - column_offset(1)); // DECRLM

    _currentScreen.get().cursor().hyperlink = {};
    _state.colorPalette = _state.defaultColorPalette;

    setActiveStatusDisplay(active_status_display::Main);
    setStatusDisplay(status_display_type::None);

    // TODO: DECNKM (Numeric keypad)
    // TODO: DECSCA (Select character attribute)
    // TODO: DECNRCM (National replacement character set)
    // TODO: GL, GR (G0, G1, G2, G3)
    // TODO: DECAUPSS (Assign user preference supplemental set)
    // TODO: DECSASD (Select active status display)
    // TODO: DECKPM (Keyboard position mode)
    // TODO: DECPCTERM (PCTerm mode)
}

void Terminal::setGraphicsRendition(graphics_rendition rendition)
{
    if (rendition == graphics_rendition::Reset)
        _currentScreen.get().cursor().graphicsRendition = {};
    else
        _currentScreen.get().cursor().graphicsRendition.flags =
            CellUtil::makeCellFlags(rendition, _currentScreen.get().cursor().graphicsRendition.flags);
}

void Terminal::setForegroundColor(color color)
{
    _currentScreen.get().cursor().graphicsRendition.foregroundColor = color;
}

void Terminal::setBackgroundColor(color color)
{
    _currentScreen.get().cursor().graphicsRendition.backgroundColor = color;
}

void Terminal::setUnderlineColor(color color)
{
    _currentScreen.get().cursor().graphicsRendition.underlineColor = color;
}

void Terminal::hardReset()
{
    // TODO: make use of _factorySettings
    setScreen(screen_type::Primary);

    // Ensure that the alternate screen buffer is having the correct size, as well.
    applyPageSizeToMainDisplay(screen_type::Alternate);

    _state.modes = Modes {};
    setMode(dec_mode::AutoWrap, true);
    setMode(dec_mode::Unicode, true);
    setMode(dec_mode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(dec_mode::SixelCursorNextToGraphic, true);
    setMode(dec_mode::VisibleCursor, true);

    _primaryScreen.hardReset();
    _alternateScreen.hardReset();
    _hostWritableStatusLineScreen.hardReset();
    _indicatorStatusScreen.hardReset();

    _state.imagePool.clear();
    _state.tabs.clear();

    _state.colorPalette = _state.defaultColorPalette;

    _hostWritableStatusLineScreen.getMargin() = margin {
        margin::vertical { {}, boxed_cast<line_offset>(_hostWritableStatusLineScreen.pageSize().lines) - 1 },
        margin::horizontal { {},
                             boxed_cast<column_offset>(_hostWritableStatusLineScreen.pageSize().columns) - 1 }
    };
    _hostWritableStatusLineScreen.verifyState();

    setActiveStatusDisplay(active_status_display::Main);
    _hostWritableStatusLineScreen.clearScreen();
    _hostWritableStatusLineScreen.updateCursorIterator();

    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    _primaryScreen.getMargin() =
        margin { margin::vertical { {}, boxed_cast<line_offset>(mainDisplayPageSize.lines) - 1 },
                 margin::horizontal { {}, boxed_cast<column_offset>(mainDisplayPageSize.columns) - 1 } };
    _primaryScreen.verifyState();

    _alternateScreen.getMargin() =
        margin { margin::vertical { {}, boxed_cast<line_offset>(mainDisplayPageSize.lines) - 1 },
                 margin::horizontal { {}, boxed_cast<column_offset>(mainDisplayPageSize.columns) - 1 } };
    alternateScreen().getMargin() = _primaryScreen.getMargin();
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

void Terminal::setScreen(screen_type type)
{
    if (type == _state.screenType)
        return;

    switch (type)
    {
        case screen_type::Primary:
            _currentScreen = _primaryScreen;
            setMouseWheelMode(input_generator::mouse_wheel_mode::Default);
            break;
        case screen_type::Alternate:
            _currentScreen = _alternateScreen;
            if (isModeEnabled(dec_mode::MouseAlternateScroll))
                setMouseWheelMode(input_generator::mouse_wheel_mode::ApplicationCursorKeys);
            else
                setMouseWheelMode(input_generator::mouse_wheel_mode::NormalCursorKeys);
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

void Terminal::applyPageSizeToMainDisplay(screen_type screenType)
{
    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    // clang-format off
    switch (screenType)
    {
        case screen_type::Primary:
            _primaryScreen.applyPageSizeToMainDisplay(mainDisplayPageSize);
            break;
        case screen_type::Alternate:
            _alternateScreen.applyPageSizeToMainDisplay(mainDisplayPageSize);
            break;
    }

    (void) _hostWritableStatusLineScreen.grid().resize(PageSize { LineCount(1), _settings.pageSize.columns }, cell_location {}, false);
    (void) _indicatorStatusScreen.grid().resize(PageSize { LineCount(1), _settings.pageSize.columns }, cell_location {}, false);
    // clang-format on

    // truncating tabs
    while (!_state.tabs.empty() && _state.tabs.back() >= unbox<column_offset>(_settings.pageSize.columns))
        _state.tabs.pop_back();

    // verifyState();
}

void Terminal::discardImage(image const& image)
{
    _eventListener.discardImage(image);
}

void Terminal::markCellDirty(cell_location position) noexcept
{
    if (_state.activeStatusDisplay != active_status_display::Main)
        return;

    if (!_selection)
        return;

    if (_selection->contains(position))
        clearSelection();
}

void Terminal::markRegionDirty(rect area) noexcept
{
    if (_state.activeStatusDisplay != active_status_display::Main)
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
    if (get_viewport().scrolled())
        get_viewport().scrollUp(n);

    if (!_selection)
        return;

    auto const top = -boxed_cast<line_offset>(_primaryScreen.historyLineCount());
    if (_selection->from().line > top && _selection->to().line > top)
        _selection->applyScroll(boxed_cast<line_offset>(n), _primaryScreen.historyLineCount());
    else
        clearSelection();
}
// }}}

void Terminal::setMaxHistoryLineCount(max_history_line_count maxHistoryLineCount)
{
    _primaryScreen.grid().setMaxHistoryLineCount(maxHistoryLineCount);
}

LineCount Terminal::maxHistoryLineCount() const noexcept
{
    return _primaryScreen.grid().maxHistoryLineCount();
}

void Terminal::setStatusDisplay(status_display_type statusDisplayType)
{
    assert(&_currentScreen.get() != &_indicatorStatusScreen);

    if (_state.statusDisplayType == statusDisplayType)
        return;

    markScreenDirty();

    auto const statusLineVisibleBefore = _state.statusDisplayType != status_display_type::None;
    auto const statusLineVisibleAfter = statusDisplayType != status_display_type::None;
    _state.statusDisplayType = statusDisplayType;

    if (statusLineVisibleBefore != statusLineVisibleAfter)
        resizeScreenInternal(_settings.pageSize, nullopt);
}

void Terminal::setActiveStatusDisplay(active_status_display activeDisplay)
{
    if (_state.activeStatusDisplay == activeDisplay)
        return;

    _state.activeStatusDisplay = activeDisplay;

    // clang-format off
    switch (activeDisplay)
    {
        case active_status_display::Main:
            switch (_state.screenType)
            {
                case screen_type::Primary:
                    _currentScreen = _primaryScreen;
                    break;
                case screen_type::Alternate:
                    _currentScreen = _alternateScreen;
                    break;
            }
            break;
        case active_status_display::StatusLine:
            _currentScreen = _hostWritableStatusLineScreen;
            break;
        case active_status_display::IndicatorStatusLine:
            _currentScreen = _indicatorStatusScreen;
            break;
    }
    // clang-format on
}

void Terminal::pushStatusDisplay(status_display_type type)
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
    setMode(ansi_mode::KeyboardAction, !enabled);
}

bool Terminal::setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick)
{
    _state.searchMode.initiatedByDoubleClick = initiatedByDoubleClick;

    if (_state.searchMode.pattern == text)
        return false;

    _state.searchMode.pattern = std::move(text);
    return true;
}

optional<cell_location> Terminal::searchReverse(u32string text, cell_location searchPosition)
{
    if (!setNewSearchTerm(std::move(text), false))
        return searchPosition;

    return searchReverse(searchPosition);
}

optional<cell_location> Terminal::search(std::u32string text,
                                         cell_location searchPosition,
                                         bool initiatedByDoubleClick)
{
    if (!setNewSearchTerm(std::move(text), initiatedByDoubleClick))
        return searchPosition;

    return search(searchPosition);
}

optional<cell_location> Terminal::search(cell_location searchPosition)
{
    auto const searchText = u32string_view(_state.searchMode.pattern);
    auto const matchLocation = currentScreen().search(searchText, searchPosition);

    if (matchLocation)
        get_viewport().makeVisibleWithinSafeArea(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

void Terminal::clearSearch()
{
    _state.searchMode.pattern.clear();
    _state.searchMode.initiatedByDoubleClick = false;
}

bool Terminal::wordDelimited(cell_location position) const noexcept
{
    // Word selection may be off by one
    position.column = std::min(position.column, boxed_cast<column_offset>(pageSize().columns - 1));

    if (isPrimaryScreen())
        return _primaryScreen.grid().cellEmptyOrContainsOneOf(position, _settings.wordDelimiters);
    else
        return _alternateScreen.grid().cellEmptyOrContainsOneOf(position, _settings.wordDelimiters);
}

std::tuple<std::u32string, cell_location_range> Terminal::extractWordUnderCursor(
    cell_location position) const noexcept
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

optional<cell_location> Terminal::searchReverse(cell_location searchPosition)
{
    auto const searchText = u32string_view(_state.searchMode.pattern);
    auto const matchLocation = currentScreen().searchReverse(searchText, searchPosition);

    if (matchLocation)
        get_viewport().makeVisibleWithinSafeArea(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

bool Terminal::isHighlighted(cell_location cell) const noexcept // NOLINT(bugprone-exception-escape)
{
    return _highlightRange.has_value()
           && std::visit(
               [cell](auto&& highlightRange) {
                   using T = std::decay_t<decltype(highlightRange)>;
                   if constexpr (std::is_same_v<T, linear_highlight>)
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
    if (!isModeEnabled(dec_mode::ReportGridCellSelection))
        return;

    if (!_selection)
    {
        reply("\033[>M");
    }
    else
    {
        auto const& selection = *_selection;

        auto const to = selection.to();
        if (to.line < line_offset(0))
            return;

        auto const from = raiseToMinimum(selection.from(), line_offset(0));
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

void Terminal::setHighlightRange(highlight_range highlightRange)
{
    if (std::holds_alternative<rectangular_highlight>(highlightRange))
    {
        auto range = std::get<rectangular_highlight>(highlightRange);
        auto points = orderedPoints(range.from, range.to);
        range = rectangular_highlight { points.first, points.second };
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
trace_handlert::trace_handlert(Terminal& terminal): _terminal { terminal }
{
}

void trace_handlert::executeControlCode(char controlCode)
{
    auto seq = sequence {};
    seq.setCategory(function_category::C0);
    seq.setFinalChar(controlCode);
    _pendingSequences.emplace_back(std::move(seq));
}

void trace_handlert::processSequence(sequence const& sequence)
{
    _pendingSequences.emplace_back(sequence);
}

void trace_handlert::writeText(char32_t codepoint)
{
    _pendingSequences.emplace_back(codepoint);
}

void trace_handlert::writeText(std::string_view codepoints, size_t cellCount)
{
    _pendingSequences.emplace_back(codepoint_sequence { codepoints, cellCount });
}

void trace_handlert::flushAllPending()
{
    for (auto const& pendingSequence: _pendingSequences)
        flushOne(pendingSequence);
    _pendingSequences.clear();
}

void trace_handlert::flushOne()
{
    if (!_pendingSequences.empty())
    {
        flushOne(_pendingSequences.front());
        _pendingSequences.pop_front();
    }
}

void trace_handlert::flushOne(pending_sequence const& pendingSequence)
{
    if (auto const* seq = std::get_if<sequence>(&pendingSequence))
    {
        if (auto const* functionDefinition = seq->functionDefinition())
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
    else if (auto const* codepoints = std::get_if<codepoint_sequence>(&pendingSequence))
    {
        fmt::print("\t\"{}\"   ; {} cells\n", codepoints->text, codepoints->cellCount);
        _terminal.activeDisplay().writeText(codepoints->text, codepoints->cellCount);
    }
}
// }}}

} // namespace terminal
