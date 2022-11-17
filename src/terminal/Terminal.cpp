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
#include <terminal/ControlCode.h>
#include <terminal/InputGenerator.h>
#include <terminal/RenderBuffer.h>
#include <terminal/RenderBufferBuilder.h>
#include <terminal/Terminal.h>
#include <terminal/logging.h>

#include <vtpty/MockPty.h>

#include <crispy/escape.h>
#include <crispy/stdfs.h>
#include <crispy/utils.h>

#include <unicode/convert.h>

#include <fmt/chrono.h>

#include <sys/types.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <utility>

using crispy::Size;

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using std::move;

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
    void logRenderBufferSwap(bool _success, uint64_t _frameID)
    {
        if (!RenderBufferLog)
            return;

        if (_success)
            RenderBufferLog()("Render buffer {} swapped.", _frameID);
        else
            RenderBufferLog()("Render buffer {} swapping failed.", _frameID);
    }
#endif
} // namespace
// }}}

Terminal::Terminal(unique_ptr<Pty> _pty,
                   size_t ptyBufferObjectSize,
                   size_t _ptyReadBufferSize,
                   Terminal::Events& _eventListener,
                   MaxHistoryLineCount _maxHistoryLineCount,
                   LineOffset _copyLastMarkRangeOffset,
                   chrono::milliseconds _cursorBlinkInterval,
                   chrono::steady_clock::time_point _now,
                   string const& _wordDelimiters,
                   Modifier _mouseProtocolBypassModifier,
                   ImageSize _maxImageSize,
                   unsigned _maxImageColorRegisters,
                   bool _sixelCursorConformance,
                   ColorPalette _colorPalette,
                   double _refreshRate,
                   bool _allowReflowOnResize,
                   bool _visualizeSelectedWord,
                   chrono::milliseconds _highlightTimeout):
    changes_ { 0 },
    eventListener_ { _eventListener },
    refreshInterval_ { static_cast<long long>(1000.0 / _refreshRate) },
    renderBuffer_ {},
    pty_ { std::move(_pty) },
    startTime_ { _now },
    currentTime_ { _now },
    lastCursorBlink_ { _now },
    cursorDisplay_ { CursorDisplay::Steady }, // TODO: pass via param
    cursorShape_ { CursorShape::Block },      // TODO: pass via param
    cursorBlinkInterval_ { _cursorBlinkInterval },
    cursorBlinkState_ { 1 },
    wordDelimiters_ { unicode::from_utf8(_wordDelimiters) },
    mouseProtocolBypassModifier_ { _mouseProtocolBypassModifier },
    copyLastMarkRangeOffset_ { _copyLastMarkRangeOffset },
    // clang-format off
    state_ { *this,
             pty_->pageSize(),
             _maxHistoryLineCount,
             _maxImageSize,
             _maxImageColorRegisters,
             _sixelCursorConformance,
             std::move(_colorPalette),
             _allowReflowOnResize },
    // clang-format on
    ptyBufferPool_ { crispy::nextPowerOfTwo(ptyBufferObjectSize) },
    currentPtyBuffer_ { ptyBufferPool_.allocateBufferObject() },
    ptyReadBufferSize_ { crispy::nextPowerOfTwo(_ptyReadBufferSize) },
    primaryScreen_ { state_, state_.primaryBuffer },
    alternateScreen_ { state_, state_.alternateBuffer },
    hostWritableStatusLineScreen_ { state_, state_.hostWritableStatusBuffer },
    indicatorStatusScreen_ { state_, state_.indicatorStatusBuffer },
    currentScreen_ { primaryScreen_ },
    viewport_ { *this,
                [this]() {
                    eventListener_.onScrollOffsetChanged(viewport_.scrollOffset());
                    breakLoopAndRefreshRenderBuffer();
                } },
    visualizeSelectedWord_ { _visualizeSelectedWord },
    selectionHelper_ { this },
    highlightTimeout_(_highlightTimeout)
{
    state_.savedColorPalettes.reserve(MaxColorPaletteSaveStackSize);
#if 0
    hardReset();
#else
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::TextReflow, true);
    setMode(DECMode::SixelCursorNextToGraphic, state_.sixelCursorConformance);
#endif
}

void Terminal::setRefreshRate(double _refreshRate)
{
    refreshInterval_ = std::chrono::milliseconds(static_cast<long long>(1000.0 / _refreshRate));
}

void Terminal::setLastMarkRangeOffset(LineOffset _value) noexcept
{
    copyLastMarkRangeOffset_ = _value;
}

Pty::ReadResult Terminal::readFromPty()
{
    auto const timeout = renderBuffer_.state == RenderBufferState::WaitingForRefresh && !screenDirty_
                             ? std::chrono::seconds(4)
                             //: refreshInterval_ : std::chrono::seconds(0)
                             : std::chrono::seconds(30);

    // Request a new Buffer Object if the current one cannot sufficiently
    // store a single text line.
    if (currentPtyBuffer_->bytesAvailable() < unbox<size_t>(state_.pageSize.columns))
    {
        if (PtyInLog)
            PtyInLog()("Only {} bytes left in TBO. Allocating new buffer from pool.",
                       currentPtyBuffer_->bytesAvailable());
        currentPtyBuffer_ = ptyBufferPool_.allocateBufferObject();
    }

    return pty_->read(*currentPtyBuffer_, timeout, ptyReadBufferSize_);
}

bool Terminal::processInputOnce()
{
    auto const readResult = readFromPty();

    if (!readResult)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            TerminalLog()("PTY read failed. {}", strerror(errno));
            pty_->close();
        }
        return errno == EINTR || errno == EAGAIN;
    }
    string_view const buf = get<0>(*readResult);
    state_.usingStdoutFastPipe = get<1>(*readResult);

    if (buf.empty())
    {
        TerminalLog()("PTY read returned with zero bytes. Closing PTY.");
        pty_->close();
        return true;
    }

    {
        auto const _l = std::lock_guard { *this };
        state_.parser.parseFragment(buf);
    }

    if (!state_.modes.enabled(DECMode::BatchedRendering))
        screenUpdated();

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
    ensureFreshRenderBuffer();
#endif

    return true;
}

// {{{ RenderBuffer synchronization
void Terminal::breakLoopAndRefreshRenderBuffer()
{
    changes_++;
    renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;

    // if (this_thread::get_id() == mainLoopThreadID_)
    //     return;

    pty_->wakeupReader();
}

bool Terminal::refreshRenderBuffer(bool _locked)
{
    renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
    ensureFreshRenderBuffer(_locked);
    return renderBuffer_.state == RenderBufferState::WaitingForRefresh;
}

bool Terminal::ensureFreshRenderBuffer(bool _locked)
{
    if (!renderBufferUpdateEnabled_)
    {
        // renderBuffer_.state = RenderBufferState::WaitingForRefresh;
        return false;
    }

    auto const elapsed = currentTime_ - renderBuffer_.lastUpdate;
    auto const avoidRefresh = elapsed < refreshInterval_;

    switch (renderBuffer_.state)
    {
        case RenderBufferState::WaitingForRefresh:
            if (avoidRefresh)
                break;
            renderBuffer_.state = RenderBufferState::RefreshBuffersAndTrySwap;
            [[fallthrough]];
        case RenderBufferState::RefreshBuffersAndTrySwap:
            if (!_locked)
                refreshRenderBuffer(renderBuffer_.backBuffer());
            else
                refreshRenderBufferInternal(renderBuffer_.backBuffer());
            renderBuffer_.state = RenderBufferState::TrySwapBuffers;
            [[fallthrough]];
        case RenderBufferState::TrySwapBuffers: {
            [[maybe_unused]] auto const success = renderBuffer_.swapBuffers(currentTime_);

#if defined(CONTOUR_PERF_STATS)
            logRenderBufferSwap(success, lastFrameID_);
#endif

#if defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE)
            // Passively invoked by the terminal thread -> do inform render thread about updates.
            if (success)
                eventListener_.renderBufferUpdated();
#endif
        }
        break;
    }
    return true;
}

void Terminal::refreshRenderBuffer(RenderBuffer& _output)
{
    auto const _l = lock_guard { *this };
    refreshRenderBufferInternal(_output);
}

PageSize Terminal::SelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::SelectionHelper::wordDelimited(CellLocation _pos) const noexcept
{
    return terminal->wordDelimited(_pos);
}

bool Terminal::SelectionHelper::wrappedLine(LineOffset _line) const noexcept
{
    return terminal->isLineWrapped(_line);
}

bool Terminal::SelectionHelper::cellEmpty(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().isCellEmpty(_pos);
}

int Terminal::SelectionHelper::cellWidth(CellLocation _pos) const noexcept
{
    // Word selection may be off by one
    _pos.column = min(_pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->currentScreen().cellWidthAt(_pos);
}

/**
 * Sets the hyperlink into hovering state if mouse is currently hovering it
 * and unsets the state when the object is being destroyed.
 */
struct ScopedHyperlinkHover
{
    shared_ptr<HyperlinkInfo const> const href;

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
    if (inputMethodData_.preeditString == preeditString)
        return;

    inputMethodData_.preeditString = preeditString;
    screenUpdated();
}

void Terminal::refreshRenderBufferInternal(RenderBuffer& _output)
{
    verifyState();

    auto const lastCursorPos = std::move(_output.cursor);
    _output.clear();

    changes_.store(0);
    screenDirty_ = false;
    ++lastFrameID_;

#if defined(CONTOUR_PERF_STATS)
    if (TerminalLog)
        TerminalLog()("{}: Refreshing render buffer.\n", lastFrameID_.load());
#endif

    auto const hoveringHyperlinkGuard = ScopedHyperlinkHover { *this, currentScreen_ };
    auto const mainDisplayReverseVideo = isModeEnabled(terminal::DECMode::ReverseVideo);
    auto const highlightSearchMatches =
        state_.searchMode.pattern.empty() ? HighlightSearchMatches::No : HighlightSearchMatches::Yes;

    if (isPrimaryScreen())
        _lastRenderPassHints =
            primaryScreen_.render(RenderBufferBuilder<PrimaryScreenCell> { *this,
                                                                           _output,
                                                                           LineOffset(0),
                                                                           mainDisplayReverseVideo,
                                                                           HighlightSearchMatches::Yes,
                                                                           inputMethodData_ },
                                  viewport_.scrollOffset(),
                                  highlightSearchMatches);
    else
        _lastRenderPassHints =
            alternateScreen_.render(RenderBufferBuilder<AlternateScreenCell> { *this,
                                                                               _output,
                                                                               LineOffset(0),
                                                                               mainDisplayReverseVideo,
                                                                               HighlightSearchMatches::Yes,
                                                                               inputMethodData_ },
                                    viewport_.scrollOffset(),
                                    highlightSearchMatches);

    switch (state_.statusDisplayType)
    {
        case StatusDisplayType::None:
            //.
            break;
        case StatusDisplayType::Indicator:
            updateIndicatorStatusLine();
            indicatorStatusScreen_.render(
                RenderBufferBuilder<StatusDisplayCell> { *this,
                                                         _output,
                                                         state_.pageSize.lines.as<LineOffset>(),
                                                         !mainDisplayReverseVideo,
                                                         HighlightSearchMatches::No,
                                                         InputMethodData {} },
                ScrollOffset(0));
            break;
        case StatusDisplayType::HostWritable:
            hostWritableStatusLineScreen_.render(
                RenderBufferBuilder<StatusDisplayCell> { *this,
                                                         _output,
                                                         state_.pageSize.lines.as<LineOffset>(),
                                                         !mainDisplayReverseVideo,
                                                         HighlightSearchMatches::No,
                                                         InputMethodData {} },
                ScrollOffset(0));
            break;
    }

    if (lastCursorPos.has_value() != _output.cursor.has_value()
        || (_output.cursor.has_value() && _output.cursor->position != lastCursorPos->position))
    {
        eventListener_.cursorPositionChanged();
    }
}
// }}}

void Terminal::updateIndicatorStatusLine()
{
    assert(&currentScreen_.get() != &indicatorStatusScreen_);

    auto& savedActiveDisplay = currentScreen_.get();
    auto const savedActiveStatusDisplay = state_.activeStatusDisplay;
    auto savedCursor = state_.cursor;
    auto const savedWrapPending = state_.wrapPending;

    currentScreen_ = indicatorStatusScreen_;
    state_.activeStatusDisplay = ActiveStatusDisplay::StatusLine;

    // Prepare old status line's cursor position and some other flags.
    state_.cursor = {};
    state_.wrapPending = false;
    indicatorStatusScreen_.updateCursorIterator();

    // Run status-line update.
    // We cannot use VT writing here, because we shall not interfere with the application's VT state.
    // TODO: Future improvement would be to allow full VT sequence support for the Indicator-status-line,
    // such that we can pass display-control partially over to some user/thirdparty configuration.
    indicatorStatusScreen_.clearLine();
    indicatorStatusScreen_.writeTextFromExternal(
        fmt::format(" {} │ {}", state_.terminalId, modeString(inputHandler().mode())));

    if (!state_.searchMode.pattern.empty() || state_.inputHandler.isEditingSearch())
        indicatorStatusScreen_.writeTextFromExternal(" SEARCH");

    if (!allowInput())
    {
        state_.cursor.graphicsRendition.foregroundColor = BrightColor::Red;
        state_.cursor.graphicsRendition.flags |= CellFlags::Bold;
        indicatorStatusScreen_.writeTextFromExternal(" (PROTECTED)");
        state_.cursor.graphicsRendition.foregroundColor = DefaultColor();
        state_.cursor.graphicsRendition.flags &= ~CellFlags::Bold;
    }

    // TODO: Disabled for now, but generally I want that functionality, but configurable somehow.
    auto constexpr indicatorLineShowCodepoints = false;
    if (indicatorLineShowCodepoints)
    {
        auto const cursorPosition = state_.inputHandler.mode() == ViMode::Insert
                                        ? state_.cursor.position
                                        : state_.viCommands.cursorPosition;
        auto const text =
            codepointText(isPrimaryScreen() ? primaryScreen().useCellAt(cursorPosition).codepoints()
                                            : alternateScreen().useCellAt(cursorPosition).codepoints());
        indicatorStatusScreen_.writeTextFromExternal(fmt::format(" | {}", text));
    }

    if (state_.inputHandler.isEditingSearch())
        indicatorStatusScreen_.writeTextFromExternal(fmt::format(
            " │ Search: {}█", unicode::convert_to<char>(u32string_view(state_.searchMode.pattern))));

    // Cleaning up.
    currentScreen_ = savedActiveDisplay;
    state_.activeStatusDisplay = savedActiveStatusDisplay;
    // restoreCursor(savedCursor, savedWrapPending);
    state_.wrapPending = savedWrapPending;
    state_.cursor = savedCursor;
    state_.cursor.position = clampCoordinate(savedCursor.position);
    currentScreen_.get().updateCursorIterator();
    verifyState();
}

bool Terminal::sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    if (allowInput() && state_.inputHandler.sendKeyPressEvent(_key, _modifier))
        return true;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    viewport_.scrollToBottom();
    bool const success = state_.inputGenerator.generate(_key, _modifier);
    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::sendCharPressEvent(char32_t _value, Modifier _modifier, Timestamp _now)
{
    cursorBlinkState_ = 1;
    lastCursorBlink_ = _now;

    // Early exit if KAM is enabled.
    if (isModeEnabled(AnsiMode::KeyboardAction))
        return true;

    if (state_.inputHandler.sendCharPressEvent(_value, _modifier))
        return true;

    auto const success = state_.inputGenerator.generate(_value, _modifier);

    flushInput();
    viewport_.scrollToBottom();
    return success;
}

bool Terminal::isMouseGrabbedByApp() const noexcept
{
    return allowInput() && respectMouseProtocol_ && state_.inputGenerator.mouseProtocol().has_value()
           && !state_.inputGenerator.passiveMouseTracking();
}

bool Terminal::sendMousePressEvent(Modifier _modifier,
                                   MouseButton _button,
                                   PixelCoordinate _pixelPosition,
                                   bool _uiHandledHint,
                                   Timestamp /*_now*/)
{
    verifyState();

    respectMouseProtocol_ =
        mouseProtocolBypassModifier_ == Modifier::None || !_modifier.contains(mouseProtocolBypassModifier_);

    if (allowInput() && respectMouseProtocol_
        && state_.inputGenerator.generateMousePress(
            _modifier, _button, currentMousePosition_, _pixelPosition, _uiHandledHint))
    {
        // TODO: Ctrl+(Left)Click's should still be catched by the terminal iff there's a hyperlink
        // under the current position
        flushInput();
        return !isModeEnabled(DECMode::MousePassiveTracking);
    }

    return false;
}

bool Terminal::handleMouseSelection(Modifier _modifier, Timestamp _now)
{
    verifyState();

    double const diff_ms = chrono::duration<double, milli>(_now - lastClick_).count();
    lastClick_ = _now;
    speedClicks_ = diff_ms >= 0.0 && diff_ms <= 500.0 ? speedClicks_ + 1 : 1;
    leftMouseButtonPressed_ = true;

    auto const startPos = CellLocation {
        currentMousePosition_.line - boxed_cast<LineOffset>(viewport_.scrollOffset()),
        currentMousePosition_.column,
    };

    switch (speedClicks_)
    {
        case 1:
            if (state_.searchMode.initiatedByDoubleClick)
                clearSearch();
            if (_modifier == mouseBlockSelectionModifier_)
                selection_ = make_unique<RectangularSelection>(selectionHelper_, startPos);
            else
                selection_ = make_unique<LinearSelection>(selectionHelper_, startPos);
            break;
        case 2: {
            selection_ = make_unique<WordWiseSelection>(selectionHelper_, startPos);
            selection_->extend(startPos);
            if (visualizeSelectedWord_)
            {
                auto const text = extractSelectionText();
                auto const text32 = unicode::convert_to<char32_t>(string_view(text.data(), text.size()));
                setNewSearchTerm(text32, true);
                state_.searchMode.initiatedByDoubleClick = true;
            }
            break;
        }
        case 3:
            selection_ = make_unique<FullLineSelection>(selectionHelper_, startPos);
            selection_->extend(startPos);
            break;
        default: clearSelection(); break;
    }

    breakLoopAndRefreshRenderBuffer();
    return true;
}

void Terminal::clearSelection()
{
    if (state_.inputHandler.isVisualMode())
        // Don't clear if in visual mode.
        return;

    InputLog()("Clearing selection.");
    selection_.reset();
    speedClicks_ = 0;

    breakLoopAndRefreshRenderBuffer();
}

bool Terminal::sendMouseMoveEvent(Modifier _modifier,
                                  CellLocation newPosition,
                                  PixelCoordinate _pixelPosition,
                                  bool _uiHandledHint,
                                  Timestamp /*_now*/)
{
    speedClicks_ = 0;

    if (leftMouseButtonPressed_ && isSelectionComplete())
        clearSelection();

    // Force refresh on mouse position grid-cell change to get the Indicator status line updated.
    auto changed = false;
    auto const cursorPositionHasChanged = newPosition != currentMousePosition_;
    auto const uiMaybeDisplayingMousePosition = state_.statusDisplayType == StatusDisplayType::Indicator;
    if (cursorPositionHasChanged && uiMaybeDisplayingMousePosition)
    {
        currentMousePosition_ = newPosition;
        markScreenDirty();
        eventListener_.renderBufferUpdated();
        changed = true;
    }

    if (newPosition == currentMousePosition_ && !isModeEnabled(DECMode::MouseSGRPixels))
        return false;

    currentMousePosition_ = newPosition;

    auto relativePos = viewport_.translateScreenToGridCoordinate(currentMousePosition_);

    changed = changed || updateCursorHoveringState();

    // Do not handle mouse-move events in sub-cell dimensions.
    if (allowInput() && respectMouseProtocol_
        && state_.inputGenerator.generateMouseMove(_modifier, relativePos, _pixelPosition, _uiHandledHint || (leftMouseButtonPressed_ && !selectionAvailable())))
    {
        flushInput();

        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return true;
    }

    if (leftMouseButtonPressed_ && !selectionAvailable())
    {
        changed = true;
        setSelector(make_unique<LinearSelection>(selectionHelper_, relativePos));
    }

    if (selectionAvailable() && selector()->state() != Selection::State::Complete
        && inputHandler().mode() == ViMode::Insert)
    {
        if (currentScreen().isCellEmpty(relativePos) && !currentScreen().compareCellTextAt(relativePos, 0x20))
        {
            relativePos.column = ColumnOffset { 0 } + *(state_.pageSize.columns - 1);
        }
        changed = true;
        selector()->extend(relativePos);
        breakLoopAndRefreshRenderBuffer();
        return true;
    }

    // TODO: adjust selector's start lines according the the current viewport

    return changed;
}

bool Terminal::sendMouseReleaseEvent(Modifier _modifier,
                                     MouseButton _button,
                                     PixelCoordinate _pixelPosition,
                                     bool _uiHandledHint,
                                     Timestamp /*_now*/)
{
    verifyState();

    if (allowInput() && respectMouseProtocol_
        && state_.inputGenerator.generateMouseRelease(
            _modifier, _button, currentMousePosition_, _pixelPosition, _uiHandledHint))
    {
        flushInput();

        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return true;
    }
    respectMouseProtocol_ = true;

    if (_button == MouseButton::Left)
    {
        leftMouseButtonPressed_ = false;
        if (selectionAvailable())
        {
            switch (selector()->state())
            {
                case Selection::State::InProgress:
                    selector()->complete();
                    eventListener_.onSelectionCompleted();
                    break;
                case Selection::State::Waiting: selection_.reset(); break;
                case Selection::State::Complete: break;
            }
        }
    }

    return true;
}

bool Terminal::sendFocusInEvent()
{
    state_.focused = true;
    breakLoopAndRefreshRenderBuffer();

    if (state_.inputGenerator.generateFocusInEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

bool Terminal::sendFocusOutEvent()
{
    state_.focused = false;
    breakLoopAndRefreshRenderBuffer();

    if (state_.inputGenerator.generateFocusOutEvent())
    {
        flushInput();
        return true;
    }

    return false;
}

void Terminal::sendPaste(string_view _text)
{
    if (!allowInput())
        return;

    if (state_.inputHandler.isEditingSearch())
    {
        state_.searchMode.pattern += unicode::convert_to<char32_t>(_text);
        screenUpdated();
        return;
    }

    state_.inputGenerator.generatePaste(_text);
    flushInput();
}

bool Terminal::hasInput() const noexcept
{
    return !state_.inputGenerator.peek().empty();
}

size_t Terminal::pendingInputBytes() const noexcept
{
    return !state_.inputGenerator.peek().size();
}

void Terminal::flushInput()
{
    if (state_.inputGenerator.peek().empty())
        return;

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    auto const input = state_.inputGenerator.peek();
    auto const rv = pty_->write(input.data(), input.size());
    if (rv > 0)
        state_.inputGenerator.consume(rv);
}

void Terminal::writeToScreen(string_view _data)
{
    {
        auto const _l = std::lock_guard { *this };
        while (!_data.empty())
        {
            if (currentPtyBuffer_->bytesAvailable() < 64
                && currentPtyBuffer_->bytesAvailable() < _data.size())
                currentPtyBuffer_ = ptyBufferPool_.allocateBufferObject();
            auto const chunk = _data.substr(0, std::min(_data.size(), currentPtyBuffer_->bytesAvailable()));
            _data.remove_prefix(chunk.size());
            state_.parser.parseFragment(currentPtyBuffer_->writeAtEnd(chunk));
        }
    }

    if (!state_.modes.enabled(DECMode::BatchedRendering))
    {
        screenUpdated();
    }
}

string_view Terminal::lockedWriteToPtyBuffer(string_view data)
{
    if (currentPtyBuffer_->bytesAvailable() < 64 && currentPtyBuffer_->bytesAvailable() < data.size())
        currentPtyBuffer_ = ptyBufferPool_.allocateBufferObject();

    auto const chunk = data.substr(0, std::min(data.size(), currentPtyBuffer_->bytesAvailable()));
    auto const _l = scoped_lock { *currentPtyBuffer_ };
    auto const ref = currentPtyBuffer_->writeAtEnd(chunk);
    return string_view(ref.data(), ref.size());
}

void Terminal::writeToScreenInternal(std::string_view data)
{
    while (!data.empty())
    {
        auto const chunk = lockedWriteToPtyBuffer(data);
        data.remove_prefix(chunk.size());
        state_.parser.parseFragment(chunk);
    }
}

void Terminal::updateCursorVisibilityState() const
{
    if (cursorDisplay_ == CursorDisplay::Steady)
        return;

    auto const passed = chrono::duration_cast<chrono::milliseconds>(currentTime_ - lastCursorBlink_);
    if (passed < cursorBlinkInterval_)
        return;

    lastCursorBlink_ = currentTime_;
    cursorBlinkState_ = (cursorBlinkState_ + 1) % 2;
}

bool Terminal::updateCursorHoveringState()
{
    verifyState();

    auto const mouseInView = isPrimaryScreen() ? primaryScreen_.contains(currentMousePosition_)
                                               : alternateScreen_.contains(currentMousePosition_);
    if (!mouseInView)
        return false;

    auto const relCursorPos = viewport_.translateScreenToGridCoordinate(currentMousePosition_);
    auto const mouseInView2 = currentScreen_.get().contains(currentMousePosition_);
    auto const newState = mouseInView2 && !!currentScreen_.get().hyperlinkIdAt(relCursorPos);

    auto const oldState = hoveringHyperlink_.exchange(newState);
    return newState != oldState;
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    if (!isModeEnabled(DECMode::VisibleCursor))
        return nullopt;

    if (cursorDisplay_ != CursorDisplay::Blink && !isBlinkOnScreen())
        return nullopt;

    auto const passedCursor = chrono::duration_cast<chrono::milliseconds>(currentTime_ - lastCursorBlink_);
    auto const passedSlowBlink = chrono::duration_cast<chrono::milliseconds>(currentTime_ - _lastBlink);
    auto const passedRapidBlink = chrono::duration_cast<chrono::milliseconds>(currentTime_ - _lastRapidBlink);
    auto nextBlink = chrono::milliseconds::max();
    if (passedCursor <= cursorBlinkInterval_)
        nextBlink = std::min(nextBlink, cursorBlinkInterval_ - passedCursor);
    if (passedSlowBlink <= _slowBlinker.interval)
        nextBlink = std::min(nextBlink, _slowBlinker.interval - passedSlowBlink);
    if (passedRapidBlink <= _rapidBlinker.interval)
        nextBlink = std::min(nextBlink, _rapidBlinker.interval - passedRapidBlink);
    if (nextBlink != std::chrono::milliseconds::max())
        return nextBlink;
    else
        return chrono::milliseconds::min();
}

void Terminal::resizeScreen(PageSize totalPageSize, optional<ImageSize> _pixels)
{
    auto const _l = lock_guard { *this };
    resizeScreenInternal(totalPageSize, _pixels);
}

void Terminal::resizeScreenInternal(PageSize totalPageSize, std::optional<ImageSize> _pixels)
{
    Require(hostWritableStatusLineScreen_.pageSize() == indicatorStatusScreen_.pageSize());

    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.
    auto const statusLineHeight = hostWritableStatusLineScreen_.pageSize().lines;
    auto const mainDisplayPageSize = state_.statusDisplayType == StatusDisplayType::None
                                         ? totalPageSize
                                         : totalPageSize - statusLineHeight;

    auto const oldMainDisplayPageSize = state_.pageSize;

    state_.pageSize = mainDisplayPageSize;
    currentMousePosition_ = clampToScreen(currentMousePosition_);
    if (_pixels)
        setCellPixelSize(_pixels.value() / totalPageSize);

    // Reset margin to their default.
    state_.margin = Margin { Margin::Vertical { {}, mainDisplayPageSize.lines.as<LineOffset>() - 1 },
                             Margin::Horizontal { {}, mainDisplayPageSize.columns.as<ColumnOffset>() - 1 } };

    applyPageSizeToCurrentBuffer();

    pty_->resizeScreen(mainDisplayPageSize, _pixels);

    // Adjust Normal-mode's cursor in order to avoid drift when growing/shrinking in main page line count.
    if (mainDisplayPageSize.lines > oldMainDisplayPageSize.lines)
        state_.viCommands.cursorPosition.line +=
            boxed_cast<LineOffset>(mainDisplayPageSize.lines - oldMainDisplayPageSize.lines);
    else if (oldMainDisplayPageSize.lines > mainDisplayPageSize.lines)
        state_.viCommands.cursorPosition.line -=
            boxed_cast<LineOffset>(oldMainDisplayPageSize.lines - mainDisplayPageSize.lines);

    verifyState();
}

void Terminal::resizeColumns(ColumnCount _newColumnCount, bool _clear)
{
    // DECCOLM / DECSCPP
    if (_clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin({}, unbox<LineOffset>(state_.pageSize.lines) - LineOffset(1));       // DECSTBM
        setLeftRightMargin({}, unbox<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1)); // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(DECMode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = PageSize { state_.pageSize.lines, _newColumnCount };
    auto const pixels = cellPixelSize() * newSize;
    resizeScreen(newSize, pixels);

    requestWindowResize(newSize);
}

void Terminal::verifyState()
{
#if !defined(NDEBUG)
    auto const thePageSize = state_.pageSize;
    Require(*currentMousePosition_.column < *thePageSize.columns);
    Require(*currentMousePosition_.line < *thePageSize.lines);
    Require(0 <= *state_.margin.horizontal.from && *state_.margin.horizontal.to < *thePageSize.columns);
    Require(0 <= *state_.margin.vertical.from && *state_.margin.vertical.to < *thePageSize.lines);

    if (isPrimaryScreen())
        Require(state_.primaryBuffer.pageSize() == state_.pageSize);
    else
        Require(state_.alternateBuffer.pageSize() == state_.pageSize);

    Require(state_.hostWritableStatusBuffer.pageSize().columns == state_.pageSize.columns);
    Require(state_.indicatorStatusBuffer.pageSize().columns == state_.pageSize.columns);

    Require(*state_.cursor.position.column < *state_.pageSize.columns);
    Require(*state_.cursor.position.line < *state_.pageSize.lines);

    Require(state_.tabs.empty() || state_.tabs.back() < unbox<ColumnOffset>(state_.pageSize.columns));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(state_.cursor.position);
    if (state_.cursor.position != clampedCursorPos)
    {
        auto const errorMessage =
            fmt::format("Cursor {} does not match clamp to screen {}.", state_.cursor, clampedCursorPos);
        currentScreen_.get().fail(errorMessage);
        // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)
    }

    currentScreen_.get().verifyState();
#endif
}

void Terminal::setCursorDisplay(CursorDisplay _display)
{
    cursorDisplay_ = _display;
}

void Terminal::setCursorShape(CursorShape _shape)
{
    cursorShape_ = _shape;
}

void Terminal::setWordDelimiters(string const& _wordDelimiters)
{
    wordDelimiters_ = unicode::from_utf8(_wordDelimiters);
}

namespace
{
    template <typename Cell>
    struct SelectionRenderer
    {
        Terminal const& term;
        ColumnOffset const rightPage;
        ColumnOffset lastColumn {};
        string text {};
        string currentLine {};

        void operator()(CellLocation _pos, Cell const& _cell)
        {
            auto const isNewLine = _pos.column < lastColumn || (_pos.column == lastColumn && !text.empty());
            bool const touchesRightPage = term.isSelected({ _pos.line, rightPage });
            if (isNewLine && (!term.isLineWrapped(_pos.line) || !touchesRightPage))
            {
                // TODO: handle logical line in word-selection (don't include LF in wrapped lines)
                trimSpaceRight(currentLine);
                text += currentLine;
                text += '\n';
                currentLine.clear();
            }
            currentLine += _cell.toUtf8();
            lastColumn = _pos.column;
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
    auto const _lock = scoped_lock { *this };

    if (!selection_)
        return "";

    if (isPrimaryScreen())
    {
        auto se = SelectionRenderer<PrimaryScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        terminal::renderSelection(*selection_, [&](CellLocation _pos) { se(_pos, primaryScreen_.at(_pos)); });
        return se.finish();
    }
    else
    {
        auto se = SelectionRenderer<AlternateScreenCell> { *this, pageSize().columns.as<ColumnOffset>() - 1 };
        terminal::renderSelection(*selection_,
                                  [&](CellLocation _pos) { se(_pos, alternateScreen_.at(_pos)); });
        return se.finish();
    }
}

string Terminal::extractLastMarkRange() const
{
    auto const _l = std::lock_guard { *this };

    // -1 because we always want to start extracting one line above the cursor by default.
    auto const bottomLine = state_.cursor.position.line + LineOffset(-1) + copyLastMarkRangeOffset_;

    auto const marker1 = optional { bottomLine };

    auto const marker0 = primaryScreen_.findMarkerUpwards(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 + 1;
    auto const lastLine = *marker1;

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        auto const lineText = primaryScreen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += primaryScreen_.grid().lineAt(lineNum).toUtf8Trimmed();
        text += '\n';
    }

    return text;
}

// {{{ ScreenEvents overrides
void Terminal::requestCaptureBuffer(LineCount lines, bool logical)
{
    return eventListener_.requestCaptureBuffer(lines, logical);
}

void Terminal::bell()
{
    eventListener_.bell();
}

void Terminal::bufferChanged(ScreenType _type)
{
    selection_.reset();
    viewport_.forceScrollToBottom();
    eventListener_.bufferChanged(_type);
}

void Terminal::scrollbackBufferCleared()
{
    selection_.reset();
    viewport_.scrollToBottom();
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::screenUpdated()
{
    if (!renderBufferUpdateEnabled_)
        return;

    if (renderBuffer_.state == RenderBufferState::TrySwapBuffers)
    {
        renderBuffer_.swapBuffers(renderBuffer_.lastUpdate);
        return;
    }

    screenDirty_ = true;
    eventListener_.screenUpdated();
}

FontDef Terminal::getFontDef()
{
    return eventListener_.getFontDef();
}

void Terminal::setFontDef(FontDef const& _fontDef)
{
    eventListener_.setFontDef(_fontDef);
}

void Terminal::copyToClipboard(string_view _data)
{
    eventListener_.copyToClipboard(_data);
}

void Terminal::inspect()
{
    eventListener_.inspect();
}

void Terminal::notify(string_view _title, string_view _body)
{
    eventListener_.notify(_title, _body);
}

void Terminal::reply(string_view _reply)
{
    // this is invoked from within the terminal thread.
    // most likely that's not the main thread, which will however write
    // the actual input events.
    // TODO: introduce new mutex to guard terminal writes.
    state_.inputGenerator.generateRaw(_reply);
}

void Terminal::requestWindowResize(PageSize _size)
{
    eventListener_.requestWindowResize(_size.lines, _size.columns);
}

void Terminal::requestWindowResize(ImageSize _size)
{
    eventListener_.requestWindowResize(_size.width, _size.height);
}

void Terminal::setApplicationkeypadMode(bool _enabled)
{
    state_.inputGenerator.setApplicationKeypadMode(_enabled);
}

void Terminal::setBracketedPaste(bool _enabled)
{
    state_.inputGenerator.setBracketedPaste(_enabled);
}

void Terminal::setCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    cursorDisplay_ = _display;
    cursorShape_ = _shape;
}

void Terminal::setCursorVisibility(bool /*_visible*/)
{
    // don't do anything for now
}

void Terminal::setGenerateFocusEvents(bool _enabled)
{
    state_.inputGenerator.setGenerateFocusEvents(_enabled);
}

void Terminal::setMouseProtocol(MouseProtocol _protocol, bool _enabled)
{
    state_.inputGenerator.setMouseProtocol(_protocol, _enabled);
}

void Terminal::setMouseTransport(MouseTransport _transport)
{
    state_.inputGenerator.setMouseTransport(_transport);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode _mode)
{
    state_.inputGenerator.setMouseWheelMode(_mode);
}

void Terminal::setWindowTitle(string_view _title)
{
    state_.windowTitle = _title;
    eventListener_.setWindowTitle(_title);
}

std::string const& Terminal::windowTitle() const noexcept
{
    return state_.windowTitle;
}

void Terminal::saveWindowTitle()
{
    state_.savedWindowTitles.push(state_.windowTitle);
}

void Terminal::restoreWindowTitle()
{
    if (!state_.savedWindowTitles.empty())
    {
        state_.windowTitle = state_.savedWindowTitles.top();
        state_.savedWindowTitles.pop();
        setWindowTitle(state_.windowTitle);
    }
}

void Terminal::setTerminalProfile(string const& _configProfileName)
{
    eventListener_.setTerminalProfile(_configProfileName);
}

void Terminal::useApplicationCursorKeys(bool _enable)
{
    auto const keyMode = _enable ? KeyMode::Application : KeyMode::Normal;
    state_.inputGenerator.setCursorKeysMode(keyMode);
}

void Terminal::setMode(AnsiMode _mode, bool _enable)
{
    if (!isValidAnsiMode(static_cast<unsigned int>(_mode)))
        return;

    if (_mode == AnsiMode::KeyboardAction)
    {
        if (_enable)
            pushStatusDisplay(StatusDisplayType::Indicator);
        else
            popStatusDisplay();
    }

    state_.modes.set(_mode, _enable);
}

void Terminal::setMode(DECMode _mode, bool _enable)
{
    if (!isValidDECMode(static_cast<unsigned int>(_mode)))
        return;

    switch (_mode)
    {
        case DECMode::AutoWrap: state_.cursor.autoWrap = _enable; break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                state_.margin.horizontal =
                    Margin::Horizontal { ColumnOffset(0),
                                         boxed_cast<ColumnOffset>(state_.pageSize.columns - 1) };
            break;
        case DECMode::Origin: state_.cursor.originMode = _enable; break;
        case DECMode::Columns132:
            if (!isModeEnabled(DECMode::AllowColumns80to132))
                break;
            if (_enable != isModeEnabled(DECMode::Columns132))
            {
                auto const clear = _enable != isModeEnabled(_mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(_enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case DECMode::BatchedRendering:
            if (state_.modes.enabled(DECMode::BatchedRendering) != _enable)
                synchronizedOutput(_enable);
            break;
        case DECMode::TextReflow:
            if (state_.allowReflowOnResize && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = _enable ? LineOffset(0) : realCursorPosition().line;
                for (auto line = startLine; line < boxed_cast<LineOffset>(state_.pageSize.lines); ++line)
                    primaryScreen_.grid().lineAt(line).setWrappable(_enable);
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(_enable);
            break;
        case DECMode::UseAlternateScreen:
            if (_enable)
                setScreen(ScreenType::Alternate);
            else
                setScreen(ScreenType::Primary);
            break;
        case DECMode::UseApplicationCursorKeys:
            useApplicationCursorKeys(_enable);
            if (isAlternateScreen())
            {
                if (_enable)
                    setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case DECMode::BracketedPaste: setBracketedPaste(_enable); break;
        case DECMode::MouseSGR:
            if (_enable)
                setMouseTransport(MouseTransport::SGR);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseExtended: setMouseTransport(MouseTransport::Extended); break;
        case DECMode::MouseURXVT: setMouseTransport(MouseTransport::URXVT); break;
        case DECMode::MousePassiveTracking:
            state_.inputGenerator.setPassiveMouseTracking(_enable);
            setMode(DECMode::MouseSGR, _enable);                    // SGR is required.
            setMode(DECMode::MouseProtocolButtonTracking, _enable); // ButtonTracking is default
            break;
        case DECMode::MouseSGRPixels:
            if (_enable)
                setMouseTransport(MouseTransport::SGRPixels);
            else
                setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseAlternateScroll:
            if (_enable)
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
        case DECMode::FocusTracking: setGenerateFocusEvents(_enable); break;
        case DECMode::UsePrivateColorRegisters: state_.usePrivateColorRegisters = _enable; break;
        case DECMode::VisibleCursor: setCursorVisibility(_enable); break;
        case DECMode::MouseProtocolX10: setMouseProtocol(MouseProtocol::X10, _enable); break;
        case DECMode::MouseProtocolNormalTracking:
            setMouseProtocol(MouseProtocol::NormalTracking, _enable);
            break;
        case DECMode::MouseProtocolHighlightTracking:
            setMouseProtocol(MouseProtocol::HighlightTracking, _enable);
            break;
        case DECMode::MouseProtocolButtonTracking:
            setMouseProtocol(MouseProtocol::ButtonTracking, _enable);
            break;
        case DECMode::MouseProtocolAnyEventTracking:
            setMouseProtocol(MouseProtocol::AnyEventTracking, _enable);
            break;
        case DECMode::SaveCursor:
            if (_enable)
                saveCursor();
            else
                restoreCursor();
            break;
        case DECMode::ExtendedAltScreen:
            if (_enable)
            {
                state_.savedPrimaryCursor = cursor();
                setMode(DECMode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(DECMode::UseAlternateScreen, false);
                restoreCursor(state_.savedPrimaryCursor);
            }
            break;
        default: break;
    }

    state_.modes.set(_mode, _enable);
}

void Terminal::setTopBottomMargin(optional<LineOffset> _top, optional<LineOffset> _bottom)
{
    auto const defaultTop = LineOffset(0);
    auto const defaultBottom = boxed_cast<LineOffset>(state_.pageSize.lines) - 1;
    auto const top = max(defaultTop, _top.value_or(defaultTop));
    auto const bottom = min(defaultBottom, _bottom.value_or(defaultBottom));

    if (top < bottom)
    {
        state_.margin.vertical.from = top;
        state_.margin.vertical.to = bottom;
        moveCursorTo({}, {});
    }
}

void Terminal::setLeftRightMargin(optional<ColumnOffset> _left, optional<ColumnOffset> _right)
{
    if (isModeEnabled(DECMode::LeftRightMargin))
    {
        auto const defaultLeft = ColumnOffset(0);
        auto const defaultRight = boxed_cast<ColumnOffset>(state_.pageSize.columns) - 1;
        auto const right = min(_right.value_or(defaultRight), defaultRight);
        auto const left = max(_left.value_or(defaultLeft), defaultLeft);
        if (left < right)
        {
            state_.margin.horizontal.from = left;
            state_.margin.horizontal.to = right;
            moveCursorTo({}, {});
        }
    }
}

void Terminal::clearScreen()
{
    if (isPrimaryScreen())
        primaryScreen_.clearScreen();
    else
        alternateScreen_.clearScreen();
}

void Terminal::moveCursorTo(LineOffset _line, ColumnOffset _column)
{
    currentScreen_.get().moveCursorTo(_line, _column);
}

void Terminal::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    state_.savedCursor = state_.cursor;
}

void Terminal::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    restoreCursor(state_.savedCursor);

    setMode(DECMode::AutoWrap, state_.savedCursor.autoWrap);
    setMode(DECMode::Origin, state_.savedCursor.originMode);
}

void Terminal::restoreCursor(Cursor const& _savedCursor)
{
    state_.wrapPending = false;
    state_.cursor = _savedCursor;
    state_.cursor.position = clampCoordinate(_savedCursor.position);
    currentScreen().updateCursorIterator();
    verifyState();
}

void Terminal::softReset()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, state_.allowReflowOnResize);
    setGraphicsRendition(GraphicsRendition::Reset);    // SGR
    state_.savedCursor.position = {};                  // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true);             // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);                   // DECOM
    setMode(AnsiMode::KeyboardAction, false);          // KAM
    setMode(DECMode::AutoWrap, false);                 // DECAWM
    setMode(AnsiMode::Insert, false);                  // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, boxed_cast<LineOffset>(state_.pageSize.lines) - LineOffset(1));       // DECSTBM
    setLeftRightMargin({}, boxed_cast<ColumnOffset>(state_.pageSize.columns) - ColumnOffset(1)); // DECRLM

    state_.cursor.hyperlink = {};
    state_.colorPalette = state_.defaultColorPalette;

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

void Terminal::setGraphicsRendition(GraphicsRendition _rendition)
{
    if (_rendition == GraphicsRendition::Reset)
        state_.cursor.graphicsRendition = {};
    else
        state_.cursor.graphicsRendition.flags =
            CellUtil::makeCellFlags(_rendition, state_.cursor.graphicsRendition.flags);
}

void Terminal::setForegroundColor(Color _color)
{
    state_.cursor.graphicsRendition.foregroundColor = _color;
}

void Terminal::setBackgroundColor(Color _color)
{
    state_.cursor.graphicsRendition.backgroundColor = _color;
}

void Terminal::setUnderlineColor(Color _color)
{
    state_.cursor.graphicsRendition.underlineColor = _color;
}

void Terminal::hardReset()
{
    setScreen(ScreenType::Primary);

    state_.modes = Modes {};
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::Unicode, true);
    setMode(DECMode::TextReflow, state_.allowReflowOnResize);
    setMode(DECMode::SixelCursorNextToGraphic, state_.sixelCursorConformance);
    setMode(DECMode::VisibleCursor, true);

    state_.primaryBuffer.reset();
    state_.alternateBuffer.reset();
    state_.hostWritableStatusBuffer.reset();
    state_.indicatorStatusBuffer.reset();

    state_.imagePool.clear();

    state_.cursor = {};
    state_.tabs.clear();

    state_.lastCursorPosition = state_.cursor.position;

    state_.margin = Margin {
        Margin::Vertical { {}, boxed_cast<LineOffset>(hostWritableStatusLineScreen_.pageSize().lines) - 1 },
        Margin::Horizontal { {},
                             boxed_cast<ColumnOffset>(hostWritableStatusLineScreen_.pageSize().columns) - 1 }
    };

    state_.colorPalette = state_.defaultColorPalette;

    if (isPrimaryScreen())
        primaryScreen_.updateCursorIterator();
    else
        alternateScreen_.updateCursorIterator();

    setActiveStatusDisplay(ActiveStatusDisplay::Main);
    setStatusDisplay(StatusDisplayType::None);
    hostWritableStatusLineScreen_.clearScreen();
    hostWritableStatusLineScreen_.updateCursorIterator();

    state_.margin =
        Margin { Margin::Vertical { {}, boxed_cast<LineOffset>(state_.pageSize.lines) - 1 },
                 Margin::Horizontal { {}, boxed_cast<ColumnOffset>(state_.pageSize.columns) - 1 } };
    primaryScreen_.verifyState();

    state_.inputGenerator.reset();
}

void Terminal::setScreen(ScreenType _type)
{
    if (_type == state_.screenType)
        return;

    switch (_type)
    {
        case ScreenType::Primary:
            currentScreen_ = primaryScreen_;
            setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
            break;
        case ScreenType::Alternate:
            currentScreen_ = alternateScreen_;
            if (isModeEnabled(DECMode::MouseAlternateScroll))
                setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
    }

    state_.screenType = _type;

    // Reset wrapPending-flag when switching buffer.
    state_.wrapPending = false;

    // Reset last-cursor position.
    state_.lastCursorPosition = state_.cursor.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    bufferChanged(_type);
}

void Terminal::applyPageSizeToCurrentBuffer()
{
    auto cursorPosition = state_.cursor.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    cursorPosition = isPrimaryScreen()
                         ? state_.primaryBuffer.resize(state_.pageSize, cursorPosition, state_.wrapPending)
                         : state_.alternateBuffer.resize(state_.pageSize, cursorPosition, state_.wrapPending);
    cursorPosition = clampCoordinate(cursorPosition);

    (void) state_.hostWritableStatusBuffer.resize(
        PageSize { LineCount(1), state_.pageSize.columns }, CellLocation {}, false);
    (void) state_.indicatorStatusBuffer.resize(
        PageSize { LineCount(1), state_.pageSize.columns }, CellLocation {}, false);

    if (state_.cursor.position.column < boxed_cast<ColumnOffset>(state_.pageSize.columns))
        state_.wrapPending = false;

    // update (last-)cursor position
    state_.cursor.position = cursorPosition;
    state_.lastCursorPosition = cursorPosition;
    if (isPrimaryScreen())
        primaryScreen_.updateCursorIterator();
    else
        alternateScreen_.updateCursorIterator();

    // truncating tabs
    while (!state_.tabs.empty() && state_.tabs.back() >= unbox<ColumnOffset>(state_.pageSize.columns))
        state_.tabs.pop_back();

        // TODO: find out what to do with DECOM mode. Reset it to?
#if 0
    inspect("after resize", std::cout);
    fmt::print("applyPageSizeToCurrentBuffer: cursor pos before: {} after: {}\n", oldCursorPos, state_.cursor.position);
#endif

    verifyState();
}

void Terminal::discardImage(Image const& _image)
{
    eventListener_.discardImage(_image);
}

void Terminal::markCellDirty(CellLocation _position) noexcept
{
    if (state_.activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!selection_)
        return;

    if (selection_->contains(_position))
        clearSelection();
}

void Terminal::markRegionDirty(Rect _area) noexcept
{
    if (state_.activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!selection_)
        return;

    if (selection_->intersects(_area))
        clearSelection();
}

void Terminal::synchronizedOutput(bool _enabled)
{
    renderBufferUpdateEnabled_ = !_enabled;
    if (_enabled)
        return;

    tick(steady_clock::now());

    auto const diff = currentTime_ - renderBuffer_.lastUpdate;
    if (diff < refreshInterval_)
        return;

    if (renderBuffer_.state == RenderBufferState::TrySwapBuffers)
        return;

    refreshRenderBuffer(true);
    eventListener_.screenUpdated();
}

void Terminal::onBufferScrolled(LineCount _n) noexcept
{
    // Adjust Normal-mode's cursor accordingly to make it fixed at the scroll-offset as if nothing has
    // happened.
    state_.viCommands.cursorPosition.line -= _n;

    // Adjust viewport accordingly to make it fixed at the scroll-offset as if nothing has happened.
    if (viewport().scrolled())
        viewport().scrollUp(_n);

    if (!selection_)
        return;

    auto const top = -boxed_cast<LineOffset>(primaryScreen_.historyLineCount());
    if (selection_->from().line > top && selection_->to().line > top)
        selection_->applyScroll(boxed_cast<LineOffset>(_n), primaryScreen_.historyLineCount());
    else
        selection_.reset();
}
// }}}

void Terminal::setMaxHistoryLineCount(MaxHistoryLineCount _maxHistoryLineCount)
{
    primaryScreen_.grid().setMaxHistoryLineCount(_maxHistoryLineCount);
}

LineCount Terminal::maxHistoryLineCount() const noexcept
{
    return primaryScreen_.grid().maxHistoryLineCount();
}

void Terminal::setStatusDisplay(StatusDisplayType statusDisplayType)
{
    assert(&currentScreen_.get() != &indicatorStatusScreen_);

    if (state_.statusDisplayType == statusDisplayType)
        return;

    markScreenDirty();

    auto const statusLineVisibleBefore = state_.statusDisplayType != StatusDisplayType::None;
    auto const statusLineVisibleAfter = statusDisplayType != StatusDisplayType::None;
    auto const theTotalPageSize = totalPageSize();
    state_.statusDisplayType = statusDisplayType;

    if (statusLineVisibleBefore != statusLineVisibleAfter)
        resizeScreenInternal(theTotalPageSize, nullopt);
}

void Terminal::setActiveStatusDisplay(ActiveStatusDisplay activeDisplay)
{
    if (state_.activeStatusDisplay == activeDisplay)
        return;

    assert(&currentScreen_.get() != &indicatorStatusScreen_);

    state_.activeStatusDisplay = activeDisplay;

    switch (activeDisplay)
    {
        case ActiveStatusDisplay::Main:
            switch (state_.screenType)
            {
                case ScreenType::Primary: currentScreen_ = primaryScreen_; break;
                case ScreenType::Alternate: currentScreen_ = alternateScreen_; break;
            }
            restoreCursor(state_.savedCursorStatusLine);
            break;
        case ActiveStatusDisplay::StatusLine: {
            currentScreen_ = hostWritableStatusLineScreen_;
            // Prepare old status line's cursor position and some other flags.
            auto cursor = state_.cursor;
            cursor.position = state_.savedCursorStatusLine.position;
            cursor.originMode = false;

            // Backup current cursor state.
            state_.savedCursorStatusLine = state_.cursor;

            // Activate cursor.
            restoreCursor(cursor);
            break;
        }
    }
}

void Terminal::pushStatusDisplay(StatusDisplayType type)
{
    // Only remember the outermost saved status display type.
    if (!state_.savedStatusDisplayType)
        state_.savedStatusDisplayType = state_.statusDisplayType;

    setStatusDisplay(type);
}

void Terminal::popStatusDisplay()
{
    if (!state_.savedStatusDisplayType)
        return;

    setStatusDisplay(state_.savedStatusDisplayType.value());
    state_.savedStatusDisplayType.reset();
}

void Terminal::setAllowInput(bool enabled)
{
    setMode(AnsiMode::KeyboardAction, !enabled);
}

bool Terminal::setNewSearchTerm(std::u32string text, bool initiatedByDoubleClick)
{
    state_.searchMode.initiatedByDoubleClick = initiatedByDoubleClick;

    if (state_.searchMode.pattern == text)
        return false;

    state_.searchMode.pattern = std::move(text);
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
    auto const searchText = u32string_view(state_.searchMode.pattern);
    auto const matchLocation = currentScreen().search(searchText, searchPosition);

    if (matchLocation)
        viewport().makeVisible(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

void Terminal::clearSearch()
{
    state_.searchMode.pattern.clear();
    state_.searchMode.initiatedByDoubleClick = false;
}

bool Terminal::wordDelimited(CellLocation position) const noexcept
{
    // Word selection may be off by one
    position.column = min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    if (isPrimaryScreen())
        return primaryScreen_.grid().cellEmptyOrContainsOneOf(position, wordDelimiters_);
    else
        return alternateScreen_.grid().cellEmptyOrContainsOneOf(position, wordDelimiters_);
}

std::tuple<std::u32string, CellLocationRange> Terminal::extractWordUnderCursor(
    CellLocation position) const noexcept
{
    if (isPrimaryScreen())
    {
        auto const range =
            primaryScreen_.grid().wordRangeUnderCursor(position, u32string_view(wordDelimiters_));
        return { primaryScreen_.grid().extractText(range), range };
    }
    else
    {
        auto const range =
            alternateScreen_.grid().wordRangeUnderCursor(position, u32string_view(wordDelimiters_));
        return { alternateScreen_.grid().extractText(range), range };
    }
}

optional<CellLocation> Terminal::searchReverse(CellLocation searchPosition)
{
    auto const searchText = u32string_view(state_.searchMode.pattern);
    auto const matchLocation = currentScreen().searchReverse(searchText, searchPosition);

    if (matchLocation)
        viewport().makeVisible(matchLocation.value().line);

    screenUpdated();
    return matchLocation;
}

bool Terminal::isHighlighted(CellLocation _cell) const noexcept
{
    return highlightRange_.has_value()
           && std::visit(
               [_cell](auto&& highlightRange) {
                   using T = std::decay_t<decltype(highlightRange)>;
                   if constexpr (std::is_same_v<T, LinearHighlight>)
                   {
                       return crispy::ascending(highlightRange.from, _cell, highlightRange.to)
                              || crispy::ascending(highlightRange.to, _cell, highlightRange.from);
                   }
                   else
                   {
                       return crispy::ascending(highlightRange.from.line, _cell.line, highlightRange.to.line)
                              && crispy::ascending(
                                  highlightRange.from.column, _cell.column, highlightRange.to.column);
                   }
               },
               highlightRange_.value());
}

void Terminal::resetHighlight()
{
    highlightRange_ = std::nullopt;
    eventListener_.screenUpdated();
}

void Terminal::setHighlightRange(HighlightRange _range)
{
    if (std::holds_alternative<RectangularHighlight>(_range))
    {
        auto range = std::get<RectangularHighlight>(_range);
        auto points = orderedPoints(range.from, range.to);
        _range = RectangularHighlight { { points.first, points.second } };
    }
    highlightRange_ = _range;
    eventListener_.updateHighlights();
}

constexpr auto MagicStackTopId = size_t { 0 };

void Terminal::pushColorPalette(size_t slot)
{
    if (slot > MaxColorPaletteSaveStackSize)
        return;

    auto const index = slot == MagicStackTopId
                           ? state_.savedColorPalettes.empty() ? 0 : state_.savedColorPalettes.size() - 1
                           : slot - 1;

    if (index >= state_.savedColorPalettes.size())
        state_.savedColorPalettes.resize(index + 1);

    // That's a totally weird idea.
    // Looking at the xterm's source code, and simply mimmicking their semantics without questioning,
    // simply to stay compatible (sadface).
    if (slot != MagicStackTopId && state_.lastSavedColorPalette < state_.savedColorPalettes.size())
        state_.lastSavedColorPalette = state_.savedColorPalettes.size();

    state_.savedColorPalettes[index] = state_.colorPalette;
}

void Terminal::reportColorPaletteStack()
{
    // XTREPORTCOLORS
    reply(fmt::format("\033[{};{}#Q", state_.savedColorPalettes.size(), state_.lastSavedColorPalette));
}

void Terminal::popColorPalette(size_t slot)
{
    if (state_.savedColorPalettes.empty())
        return;

    auto const index = slot == MagicStackTopId ? state_.savedColorPalettes.size() - 1 : slot - 1;

    state_.colorPalette = state_.savedColorPalettes[index];
    if (slot == MagicStackTopId)
        state_.savedColorPalettes.pop_back();
}

} // namespace terminal
