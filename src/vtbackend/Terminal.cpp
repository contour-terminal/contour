// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Terminal.h>

#include <vtbackend/ControlCode.h>
#include <vtbackend/Functions.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/RenderBufferBuilder.h>
#include <vtbackend/SequenceBuilder.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <vtpty/MockPty.h>

#include <crispy/assert.h>
#include <crispy/base64.h>
#include <crispy/escape.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <gsl/pointers>

#include <sys/types.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <ranges>
#include <regex>
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
    /// Non-zero while this thread is inside VTParser::parseFragment().
    ///
    /// The parser is a state machine that is not safe to re-enter. A sequence handler that replies
    /// reaches Terminal::flushInput() (Screen::reportColorPaletteUpdate(), Screen::replyGipStatus(),
    /// ... all reply and flush), and with SRM reset that echoes -- which parses. Parsing there would
    /// clobber the state machine half-way through the sequence being dispatched, and would take the
    /// non-recursive _stateMutex that writeToScreen() already holds. xterm guards the identical hazard
    /// with ParseState::check_recur and defers the bytes instead -- "Defer parsing when parser is
    /// already running as the parser is not safe to reenter" (xterm-406, charproc.c, doparsing()).
    ///
    /// Thread-local rather than a member because the question is "is *this thread* parsing": a GUI
    /// thread echoing a keystroke while the parser thread happens to be parsing must echo, not defer.
    ///
    /// @see Terminal::echoLocally(), Terminal::processPendingLocalEcho().
    thread_local int tlParseDepth = 0;

    /// Marks this thread as being inside the VT parser for as long as it is alive.
    struct ParseDepthGuard
    {
        ParseDepthGuard() noexcept { ++tlParseDepth; }
        ~ParseDepthGuard() { --tlParseDepth; }

        ParseDepthGuard(ParseDepthGuard const&) = delete;
        ParseDepthGuard& operator=(ParseDepthGuard const&) = delete;
        ParseDepthGuard(ParseDepthGuard&&) = delete;
        ParseDepthGuard& operator=(ParseDepthGuard&&) = delete;
    };

    std::optional<std::string> resolveExistingLocalPath(std::string const& cwd,
                                                        std::string const& home,
                                                        std::string const& match)
    {
        auto candidate = std::filesystem::path {};
        if (match.starts_with("~/"))
        {
            if (home.empty())
                return std::nullopt;
            candidate = std::filesystem::path(home) / match.substr(2);
        }
        else if (auto const matchPath = std::filesystem::path(match); matchPath.is_absolute())
            candidate = matchPath;
        else
        {
            if (cwd.empty())
                return std::nullopt;
            candidate = std::filesystem::path(cwd) / matchPath;
        }

        auto ec = std::error_code {};
        if (!std::filesystem::exists(candidate, ec))
            return std::nullopt;
        return candidate.lexically_normal().string();
    }

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
    _currentScreen { &_hostWritableStatusLineScreen }, // temporary; reassigned after _pages is populated
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
    _focused { _settings.focused },
    _pageMargins {},
    _hostWritableScreenMargin { .vertical = Margin::Vertical { .from = {}, .to = LineOffset(0) },
                                .horizontal =
                                    Margin::Horizontal { .from = {},
                                                         .to = _settings.pageSize.columns.as<ColumnOffset>()
                                                               - ColumnOffset(1) } },
    _maxSixelColorRegisters { _settings.maxImageRegisterCount },
    _sixelColorPalette { std::make_shared<SixelColorPalette>(_maxSixelColorRegisters,
                                                             _maxSixelColorRegisters) },
    _imagePool { [this](Image const* image) { discardImage(*image); } },
    _hyperlinks { .cache = HyperlinkCache { 1024 } },
    _sequenceBuilder { ModeDependantSequenceHandler { *this }, TerminalInstructionCounter { *this } },
    _parser { std::ref(_sequenceBuilder) },
    _viCommands { *this },
    _inputHandler { _viCommands, ViMode::Insert },
    _shellIntegration { std::make_unique<NullShellIntegration>() }
{
    // Initialize all page margins to defaults.
    _pageMargins.fill(makeDefaultMargin(_settings.pageSize));

    // Populate the 16-page array: page 0 (primary) gets scrollback/reflow, pages 1-15 do not.
    _pages.reserve(MaxPageCount);
    _pages.push_back(std::make_unique<Screen>(*this,
                                              _pageMargins.data(),
                                              _settings.pageSize,
                                              _settings.primaryScreen.allowReflowOnResize,
                                              _settings.maxHistoryLineCount,
                                              "page-1"));
    for (auto const i: std::views::iota(1, MaxPageCount))
        _pages.push_back(std::make_unique<Screen>(
            *this, &_pageMargins[i], _settings.pageSize, false, LineCount(0), std::format("page-{}", i + 1)));
    _currentScreen = _pages[0].get();

    _savedColorPalettes.reserve(MaxColorPaletteSaveStackSize);

    _inputGenerator.setMouseWheelScrollMultiplier(
        static_cast<unsigned>(unbox(_settings.mouseWheelScrollMultiplier)));

    // TODO(should be this instead?): hardReset();

    // SRM, set: the terminal does *not* echo what it sends -- the host does. This is the default every
    // VT terminal ships with, and it has to be said out loud: the mode register starts at all zeroes,
    // and a *reset* SRM means local echo is on. @see flushInput().
    setMode(AnsiMode::SendReceive, true);

    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::AutoRepeat, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, _settings.graphemeClustering);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::PageCursorCoupling, true);
    setMode(DECMode::LeftRightMargin, false);

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);
}

void Terminal::onViewportChanged()
{
    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = _viewport.clampCellLocation(_viCommands.cursorPosition);

    if (_hintModeHandler.isActive())
        refreshHints();

    extendSelectionAfterScroll();

    _eventListener.onScrollOffsetChanged(_viewport.scrollOffset());
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::extendSelectionAfterScroll()
{
    if (_isAutoScrolling || !_leftMouseButtonPressed || !selectionAvailable()
        || selector()->state() == Selection::State::Complete)
        return;

    auto const relativePos = _viewport.translateScreenToGridCoordinate(_currentMousePosition);
    _viCommands.cursorPosition = relativePos;
    if (selector()->extend(relativePos))
        updateSelectionMatches();
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

std::optional<Terminal::PtyReadResult> Terminal::readFromPty()
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

    // Capture the buffer pointer BEFORE the read call. This is critical to avoid
    // a race condition where another thread could change _currentPtyBuffer during
    // or after the read operation.
    auto const bufferUsedForReading = _currentPtyBuffer;

    auto result = _pty->read(*bufferUsedForReading, timeout, _ptyReadBufferSize);
    if (!result)
        return std::nullopt;

    return PtyReadResult { .readResult = *result, .buffer = bufferUsedForReading };
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

    auto const ptyReadResult = readFromPty();

    if (!ptyReadResult)
    {
        terminalLog()("PTY read failed. {}", strerror(errno));
        if (errno == EINTR || errno == EAGAIN)
            return true;

        _pty->close();
        return false;
    }

    string_view const buf = ptyReadResult->readResult.data;
    _usingStdoutFastPipe = ptyReadResult->readResult.fromStdoutFastPipe;

    if (buf.empty())
    {
        terminalLog()("PTY read returned with zero bytes. Closing PTY.");
        _pty->close();
        return false;
    }

    {
        auto const _ = std::lock_guard { *this };
        // Use the buffer that readFromPty() actually read into, not _currentPtyBuffer
        // which might have been changed by another thread (e.g., writeToScreen()).
        // This is critical to ensure buffer_fragment in TrivialLineBuffer holds
        // the correct buffer reference.
        _parsingBuffer = ptyReadResult->buffer;
        {
            auto const parseGuard = ParseDepthGuard {};
            _parser.parseFragment(buf);
        }
        _parsingBuffer.reset();

        // Process any macros that were queued by DECINVM during the parse.
        processPendingMacros();

        // Process any local echo (SRM) that a reply deferred during the parse.
        processPendingLocalEcho();
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

    ScopedHyperlinkHover(Terminal const& terminal, Screen const& /*screen*/):
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

    // When smooth scrolling is active with a non-zero pixel offset,
    // render one extra line at the bottom to fill the gap caused by the sub-cell shift.
    auto const smoothScrollExtra = smoothScrollExtraLines();

    // When DECPCCM is reset and cursor/display pages differ, hide the cursor from rendering.
    auto const effectiveCursorPosition =
        (_cursorPage != _displayedPage) ? std::optional<CellLocation> {} : theCursorPosition;

    auto& displayedScreen = pageAt(_displayedPage);

    if (_displayedPage == PageIndex(0))
        _lastRenderPassHints = displayedScreen.render(RenderBufferBuilder { *this,
                                                                            displayedScreen,
                                                                            output,
                                                                            baseLine,
                                                                            mainDisplayReverseVideo,
                                                                            _colorPalette.colorLookupTable,
                                                                            HighlightSearchMatches::Yes,
                                                                            _inputMethodData,
                                                                            effectiveCursorPosition,
                                                                            includeSelection },
                                                      _viewport.scrollOffset(),
                                                      highlightSearchMatches,
                                                      smoothScrollExtra);
    else
        _lastRenderPassHints = displayedScreen.render(RenderBufferBuilder { *this,
                                                                            displayedScreen,
                                                                            output,
                                                                            baseLine,
                                                                            mainDisplayReverseVideo,
                                                                            _colorPalette.colorLookupTable,
                                                                            HighlightSearchMatches::Yes,
                                                                            _inputMethodData,
                                                                            effectiveCursorPosition,
                                                                            includeSelection },
                                                      _viewport.scrollOffset(),
                                                      highlightSearchMatches);

    // Save the baseLine used for the main screen before the bottom status line shifts it.
    auto const mainScreenLine = baseLine;

    if (_settings.statusDisplayPosition == StatusDisplayPosition::Bottom)
    {
        baseLine += pageSize().lines.as<LineOffset>();
        fillRenderBufferStatusLine(output, includeSelection, baseLine);
    }

    applyHintOverlay(output, mainScreenLine);
    updateCursorMotionAnimation(output);
    applyScreenTransitionBlending(output);
}

void Terminal::updateCursorMotionAnimation(RenderBuffer& output)
{
    if (!output.cursor.has_value())
        return;

    // Effective animation duration: at least 3 frames at the current refresh rate
    // to guarantee visible interpolation.
    constexpr auto MinFrames = 3;
    auto const effectiveDuration =
        std::max(_settings.cursorMotionAnimationDuration, _refreshInterval.value * MinFrames);

    // Use grid coordinates (without scroll offset or baseLine) for animation state.
    // This prevents viewport scrolling from triggering spurious cursor animations,
    // since the grid position doesn't change when the user scrolls.
    auto const gridCursorPos = [&]() -> CellLocation {
        if (inputHandler().mode() == ViMode::Insert)
            return currentScreen().cursor().position;
        return _viCommands.cursorPosition;
    }();

    // The line offset from grid to screen coordinates (accounts for baseLine and scrollOffset).
    auto const gridToScreenLineOffset = output.cursor->position.line - gridCursorPos.line;

    if (_cursorMotion.active && !_cursorMotion.isComplete(_currentTime))
    {
        // Animation in progress — check if target changed (chaining)
        if (gridCursorPos != _cursorMotion.toPosition)
        {
            // Chain: start new animation from current interpolated position
            auto const currentProgress = _cursorMotion.progress(_currentTime);
            _cursorMotion.fromPosition =
                lerpCellLocation(_cursorMotion.fromPosition, _cursorMotion.toPosition, currentProgress);
            _cursorMotion.fromColor =
                mixColor(_cursorMotion.fromColor, _cursorMotion.toColor, currentProgress);
            _cursorMotion.toPosition = gridCursorPos;
            _cursorMotion.toColor = output.cursor->cursorColor;
            _cursorMotion.startTime = _currentTime;
            _cursorMotion.duration = effectiveDuration;
        }
        // Inject animation data (convert grid -> screen coordinates)
        auto fromScreen = _cursorMotion.fromPosition;
        fromScreen.line += gridToScreenLineOffset;
        output.cursor->animateFrom = fromScreen;
        output.cursor->animationProgress = _cursorMotion.progress(_currentTime);
        output.cursor->animateFromColor = _cursorMotion.fromColor;
    }
    else if (gridCursorPos != _cursorMotion.toPosition && _settings.cursorMotionAnimationDuration.count() > 0)
    {
        // Start new animation (store grid positions)
        _cursorMotion.active = true;
        _cursorMotion.fromPosition = _cursorMotion.toPosition; // previous target
        _cursorMotion.fromColor = _cursorMotion.toColor;       // previous target's color
        _cursorMotion.toPosition = gridCursorPos;
        _cursorMotion.toColor = output.cursor->cursorColor;
        _cursorMotion.startTime = _currentTime;
        _cursorMotion.duration = effectiveDuration;

        auto fromScreen = _cursorMotion.fromPosition;
        fromScreen.line += gridToScreenLineOffset;
        output.cursor->animateFrom = fromScreen;
        output.cursor->animationProgress = _cursorMotion.progress(_currentTime);
        output.cursor->animateFromColor = _cursorMotion.fromColor;
    }
    else
    {
        _cursorMotion.toPosition = gridCursorPos;
        _cursorMotion.toColor = output.cursor->cursorColor;
    }
}

void Terminal::applyScreenTransitionBlending(RenderBuffer& output)
{
    if (!_screenTransition.active)
        return;

    auto const progress = _screenTransition.progress(_currentTime);
    if (progress >= 1.0f)
    {
        finalizeScreenTransition();
        return;
    }

    auto const defaultBg = _colorPalette.defaultBackground;

    // Determine the line offset range that covers the main display (excluding the status line).
    auto const mainLineBegin = (_settings.statusDisplayPosition == StatusDisplayPosition::Top)
                                   ? statusLineHeight().as<LineOffset>()
                                   : LineOffset(0);
    auto const mainLineEnd = mainLineBegin + pageSize().lines.as<LineOffset>();

    auto const isMainDisplayCell = [&](CellLocation const& pos) {
        return pos.line >= mainLineBegin && pos.line < mainLineEnd;
    };
    auto const isMainDisplayLine = [&](LineOffset offset) {
        return offset >= mainLineBegin && offset < mainLineEnd;
    };

    if (progress < 0.5f)
    {
        // Phase 1: Fade-out — replace main display cells with snapshot cells fading to background.
        auto const fadeOut = progress * 2.0f; // 0→1 over the first half

        // Preserve status line cells and lines, discard main display entries.
        std::erase_if(output.cells, [&](auto const& c) { return isMainDisplayCell(c.position); });
        std::erase_if(output.lines, [&](auto const& l) { return isMainDisplayLine(l.lineOffset); });
        output.cursor.reset();

        output.cells.reserve(output.cells.size() + _screenTransition.snapshotCells.size());
        for (auto const& snap: _screenTransition.snapshotCells)
        {
            if (!isMainDisplayCell(snap.position))
                continue;
            auto cell = snap;
            blendAttributesTo(cell.attributes, defaultBg, fadeOut);
            output.cells.push_back(std::move(cell));
        }

        // Re-sort cells and lines by position to restore the sorted invariant
        // required by the renderer's partition_point binary search.
        std::ranges::sort(output.cells, {}, &RenderCell::position);
        std::ranges::sort(output.lines, {}, &RenderLine::lineOffset);
    }
    else
    {
        // Phase 2: Fade-in — blend main display from default background.
        auto const fadeIn = (progress - 0.5f) * 2.0f; // 0→1 over the second half

        for (auto& cell: output.cells)
        {
            if (!isMainDisplayCell(cell.position))
                continue;
            blendAttributesFrom(cell.attributes, defaultBg, fadeIn);
        }

        for (auto& line: output.lines)
        {
            if (!isMainDisplayLine(line.lineOffset))
                continue;
            blendAttributesFrom(line.textAttributes, defaultBg, fadeIn);
            blendAttributesFrom(line.fillAttributes, defaultBg, fadeIn);
        }
    }
}

LineCount Terminal::fillRenderBufferStatusLine(RenderBuffer& output, bool includeSelection, LineOffset base)
{
    auto const mainDisplayReverseVideo = isModeEnabled(vtbackend::DECMode::ReverseVideo);

    // The status lines are host-owned chrome painted from the configured status-line colors, not part of
    // the application's canvas. They therefore always render through the ANSI SGR color table: an
    // application that selects DECSTGLT Alternate mode recolors its own text, never the terminal's
    // furniture. (colorLookupTable lives on the terminal-global palette, which the status-line screens
    // share, so the mode has to be overridden here rather than merely read.)
    auto constexpr StatusLineColorLookupTable = ColorLookupTable::AnsiSgr;

    switch (_statusDisplayType)
    {
        case StatusDisplayType::None:
            //.
            return LineCount(0);
        case StatusDisplayType::Indicator:
            updateIndicatorStatusLine();
            _indicatorStatusScreen.render(RenderBufferBuilder { *this,
                                                                _indicatorStatusScreen,
                                                                output,
                                                                base,
                                                                !mainDisplayReverseVideo,
                                                                StatusLineColorLookupTable,
                                                                HighlightSearchMatches::No,
                                                                InputMethodData {},
                                                                nullopt,
                                                                includeSelection },
                                          ScrollOffset(0));
            return _indicatorStatusScreen.pageSize().lines;
        case StatusDisplayType::HostWritable:
            _hostWritableStatusLineScreen.render(RenderBufferBuilder { *this,
                                                                       _hostWritableStatusLineScreen,
                                                                       output,
                                                                       base,
                                                                       !mainDisplayReverseVideo,
                                                                       StatusLineColorLookupTable,
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
                case ViMode::Normal:
                case ViMode::Hint: return colorPalette().indicatorStatusLineNormalMode;
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

void Terminal::autoScrollToBottomIfEnabled()
{
    if (_settings.autoScrollOnUpdate)
        _viewport.scrollToBottom();
}

void Terminal::forceAutoScrollToBottomIfEnabled()
{
    if (_settings.autoScrollOnUpdate)
        _viewport.forceScrollToBottom();
}

void Terminal::scrollToBottomOnInput()
{
    // Unconditional on purpose: user input must always jump the viewport back to the bottom, even
    // when `autoScrollOnUpdate` (which governs output-driven scrolling only) is turned off. The
    // alt-screen guard still applies via `scrollToBottom()`'s own `scrollingDisabled()` check.
    _viewport.scrollToBottom();
}

Handled Terminal::sendKeyEvent(Key key,
                               KeyboardModifiers modifiers,
                               KeyboardEventType eventType,
                               Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    if (!allowInput())
        return Handled { true };

    // Suppress key repeat events when DECARM (auto-repeat mode) is disabled.
    if (eventType == KeyboardEventType::Repeat && !isModeEnabled(DECMode::AutoRepeat))
        return Handled { true };

    // Every consumer below — the hint-mode handler, the DECUDK gate and the Vi input handler — makes
    // a UI decision, and therefore sees the chord only. The latched lock state goes to the input
    // generator alone, which reports it to the application on purpose (Kitty CSI u, Win32
    // dwControlKeyState, numpad CSI parameters).
    auto const chord = modifiers.chord;

    // Route Escape to hint mode handler if active (ignore key release events).
    if (_hintModeHandler.isActive() && key == Key::Escape && eventType != KeyboardEventType::Release)
    {
        _hintModeHandler.processInput(U'\x1B');
        return Handled { true };
    }

    if (_inputHandler.sendKeyPressEvent(key, chord, eventType))
        return Handled { true };

    // Check for User-Defined Keys (DECUDK) — override function keys F6-F20
    // when no modifiers are pressed and the key is being pressed (not released).
    if (chord.none() && eventType == KeyboardEventType::Press)
    {
        if (auto const udkStr = udkStringForKey(key); udkStr.has_value())
        {
            _inputGenerator.generateRaw(*udkStr);
            flushInput();
            scrollToBottomOnInput();
            return Handled { true };
        }
    }

    bool const success = _inputGenerator.generate(key, modifiers, eventType);
    if (success)
    {
        flushInput();
        // A key release is never "typed content": under win32-input-mode / the Kitty keyboard
        // protocol the release of a viewport-scroll shortcut (e.g. the Up in Shift+Up) still
        // generates PTY input, and snapping to the bottom here would undo the scroll the press just
        // performed. Only press/repeat reveal the cursor.
        if (!isModifierKey(key) && eventType != KeyboardEventType::Release)
            scrollToBottomOnInput();
    }
    return Handled { success };
}

Handled Terminal::sendCharEvent(char32_t ch,
                                uint32_t physicalKey,
                                KeyboardModifiers modifiers,
                                KeyboardEventType eventType,
                                Timestamp now)
{
    _cursorBlinkState = 1;
    _lastCursorBlink = now;

    // Early exit if KAM is enabled.
    if (!allowInput())
        return Handled { true };

    // Suppress character repeat events when DECARM (auto-repeat mode) is disabled.
    if (eventType == KeyboardEventType::Repeat && !isModeEnabled(DECMode::AutoRepeat))
        return Handled { true };

    // See sendKeyEvent(): the hint-mode handler and the Vi input handler match on the chord, the
    // input generator reports the latched lock state to the application.
    auto const chord = modifiers.chord;

    // Route input to hint mode handler if active (no modifiers — just label chars).
    // Ignore key release events to prevent the activating key's release from being
    // processed as a label character (e.g. 'h' release after 'gh' triggered hint mode).
    if (_hintModeHandler.isActive() && chord.none() && eventType != KeyboardEventType::Release)
    {
        if (_hintModeHandler.processInput(ch))
            return Handled { true };
    }

    if (_inputHandler.sendCharPressEvent(ch, chord, eventType))
        return Handled { true };

    auto const success = _inputGenerator.generate(ch, physicalKey, modifiers, eventType);
    if (success)
    {
        flushInput();
        // See sendKeyEvent(): key releases are not typed content, so they must not snap the viewport
        // back to the bottom even when the protocol reports them to the application.
        if (eventType != KeyboardEventType::Release)
            scrollToBottomOnInput();
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
        if (!allowPassMouseEventToApp(modifiers) || isModeEnabled(DECMode::MousePassiveTracking))
            uiHandledHint = handleMouseSelection(modifiers) || uiHandledHint;
    }

    verifyState();

    // DEC Locator: intercept mouse press if locator mode is active
    if (_locatorState.enabled)
    {
        auto const btn = [&]() -> int {
            switch (button)
            {
                case MouseButton::Left: return 1;
                case MouseButton::Middle: return 2;
                case MouseButton::Right: return 4;
                default: return 0;
            }
        }();
        if (btn != 0 && handleLocatorMouseEvent(btn, true, _currentMousePosition))
            return Handled { true };
    }

    // Hand the event to the input generator for app tracking or wheel->cursor-key translation;
    // generateMousePress() self-rejects the cases where neither applies.
    auto const eventHandledByApp =
        (allowPassMouseEventToApp(modifiers) || allowWheelTranslationToApp(button, modifiers))
        && _inputGenerator.generateMousePress(
            modifiers, button, _currentMousePosition, pixelPosition, uiHandledHint);

    // TODO: Ctrl+(Left)Click's should still be caught by the terminal iff there's a hyperlink
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

    // Shift+Click extends a completed selection to the new click position.
    // Re-create as LinearSelection to avoid breaking WordWise/FullLine invariants.
    if (modifiers.contains(Modifier::Shift) && selectionAvailable()
        && selector()->state() == Selection::State::Complete)
    {
        _speedClicks = 0; // Don't count Shift+Click in the speed-click sequence.

        // Anchor at the farthest endpoint so the selection grows in either direction.
        // NB: Store values before std::minmax — minmax returns references, and
        //     selector()->from()/to() return temporaries that would dangle.
        auto const fromPos = selector()->from();
        auto const toPos = selector()->to();
        auto const [selStart, selEnd] = std::minmax(fromPos, toPos);
        auto const anchor = (startPos < selStart) ? selEnd : selStart;

        setSelector(std::make_unique<LinearSelection>(_selectionHelper, anchor, selectionUpdatedHelper()));
        if (selector()->extend(startPos))
            updateSelectionMatches();

        breakLoopAndRefreshRenderBuffer();
        return true;
    }

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

void Terminal::performAutoScroll(int direction, LineCount lineCount)
{
    if (!selectionAvailable() || selector()->state() == Selection::State::Complete)
        return;

    // Suppress extendSelectionAfterScroll() in onViewportChanged() — we handle
    // selection extension ourselves with the boundary position, not the mouse position.
    _isAutoScrolling = true;
    auto const scrolled = (direction < 0) ? _viewport.scrollUp(lineCount) : _viewport.scrollDown(lineCount);
    _isAutoScrolling = false;

    if (!scrolled)
        return;

    // Extend selection to the boundary row (top when scrolling up, bottom when scrolling down).
    auto const boundaryLine = (direction < 0) ? LineOffset(0) : LineOffset(*_settings.pageSize.lines - 1);
    auto const boundaryCol =
        (direction < 0) ? ColumnOffset(0) : ColumnOffset(*_settings.pageSize.columns - 1);

    auto const gridPos = _viewport.translateScreenToGridCoordinate(
        CellLocation { .line = boundaryLine, .column = boundaryCol });

    _viCommands.cursorPosition = gridPos;
    if (selector()->extend(gridPos))
        updateSelectionMatches();
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
    if (cellPixelWidth == 0)
        return newPosition != selectionCorner;
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
            // NB: The drag end is taken exactly where the pointer is, including over the blank
            // region right of a short line. Snapping it to the right margin there -- as this used to
            // -- selected (and copied) the whole line the moment the pointer crossed the last
            // character. It was redundant besides: Selection::ranges() already gives the first line
            // of a MULTI-line selection its full width, and Selection::contains is lexicographic, so
            // hit-testing agrees without help.
            relativePos = clampDragWithinMulticellBlock(selector()->from(), relativePos);
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

    // DEC Locator: intercept mouse release if locator mode is active
    if (_locatorState.enabled)
    {
        if (handleLocatorMouseEvent(0, false, _currentMousePosition))
            return Handled { true };
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

    // Own the bytes before anything is allowed to touch the generator again: peek() returns a view into
    // InputGenerator::_pendingSequence, and both steps below invalidate it. consume() may clear that
    // string outright, and the local echo parses these bytes -- a query among them replies, which appends
    // to that very string and reallocates it, leaving the view dangling. @see echoLocally().
    auto const input = std::string(_inputGenerator.peek());

    // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
    auto const rv = _pty->write(input);
    if (rv <= 0)
        return;

    _inputGenerator.consume(rv);

    // SRM, reset: the terminal echoes everything it sends. This is the "local echo" a host that does not
    // echo for itself relies on, and it is off by default -- SRM is set, and the host echoes.
    //
    // Echoed here, at the one point where anything reaches the host, which is also where xterm echoes
    // (unparseputc()). That means a *reply* is echoed too, which looks surprising until you remember what
    // the mode says: it is about what the terminal sends, not about what the user typed.
    //
    // Echoed *after* the send, and only as much as was actually sent: echoing can itself produce a reply,
    // whose flush would re-send anything still pending here.
    if (!isModeEnabled(AnsiMode::SendReceive))
        echoLocally(std::string_view(input).substr(0, static_cast<size_t>(rv)));
}

void Terminal::echoLocally(std::string_view bytes)
{
    if (bytes.empty())
        return;

    if (tlParseDepth > 0)
    {
        // The parser is running on this thread and is not safe to re-enter, so hand the bytes to the
        // barrier that runs once it has unwound. _pendingLocalEcho is only ever touched from inside a
        // parse or from the drain that follows it, both under _stateMutex.
        _pendingLocalEcho += bytes;
        return;
    }

    writeToScreen(bytes);
}

void Terminal::processPendingLocalEcho()
{
    // A drain step parses, and parsing can reply, and a reply flushes and thus echoes again -- which
    // lands back in _pendingLocalEcho, because parseFragmentChunked() holds the parse depth above zero.
    // Loop until it stops growing, as xterm's main parse loop does with its deferred area.
    while (!_pendingLocalEcho.empty())
    {
        auto const bytes = std::exchange(_pendingLocalEcho, std::string {});
        parseFragmentChunked(bytes);
    }
}

void Terminal::parseFragmentChunked(string_view vtStream)
{
    auto const parseGuard = ParseDepthGuard {};

    while (!vtStream.empty())
    {
        if (_currentPtyBuffer->bytesAvailable() < 64 && _currentPtyBuffer->bytesAvailable() < vtStream.size())
            _currentPtyBuffer = _ptyBufferPool.allocateBufferObject();
        auto const chunk = vtStream.substr(0, std::min(vtStream.size(), _currentPtyBuffer->bytesAvailable()));
        vtStream.remove_prefix(chunk.size());
        // Set parsingBuffer to ensure buffer_fragment holds the correct buffer reference.
        _parsingBuffer = _currentPtyBuffer;
        _parser.parseFragment(_currentPtyBuffer->writeAtEnd(chunk));
        _parsingBuffer.reset();
    }
}

void Terminal::writeToScreen(string_view vtStream)
{
    {
        auto const l = std::lock_guard { *this };
        parseFragmentChunked(vtStream);

        // Any local echo the parse deferred (a sequence that replied) is safe to parse now.
        processPendingLocalEcho();
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

    // Only use bulk write path when current line has uniform attributes.
    // This matches the baseline behavior (isTrivialBuffer check) and avoids
    // a timing-sensitive race in screenUpdated() where TrySwapBuffers state
    // causes _screenDirty to not be set, leaving the display stale.
    if (!primaryScreen().currentLine().isTrivialBuffer())
        return 0;

    return unbox<size_t>(currentPageMargin().horizontal.to - _currentScreen->cursor().position.column);
}

// {{{ SimpleSequenceHandler
// This simple sequence handler is used to write to the screen
// without any optimizations (and no parser hooking).
// We use this for rendering the status line.
struct SimpleSequenceHandler
{
    Screen& targetScreen;

    void executeControlCode(char controlCode) { targetScreen.executeControlCode(controlCode); }

    void processSequence(Sequence const& seq)
    {
        // NB: We might want to check for some VT sequences that should not be processed here.
        // We might make use of Terminal::activeSequences() here somehow.
        targetScreen.processSequence(seq);
    }

    void processAPC(std::string_view body) { targetScreen.processAPC(body); }

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

void Terminal::writeToScreenInternal(Screen& screen, std::string_view vtStream)
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
    auto nextWakeup = chrono::milliseconds::max();

    // Cursor blink scheduling
    if (isModeEnabled(DECMode::VisibleCursor) && _settings.cursorDisplay == CursorDisplay::Blink)
    {
        auto const passedCursor =
            chrono::duration_cast<chrono::milliseconds>(_currentTime - _lastCursorBlink);
        if (passedCursor <= _settings.cursorBlinkInterval)
            nextWakeup = std::min(nextWakeup, _settings.cursorBlinkInterval - passedCursor);
    }

    // Cell blink scheduling
    if (isBlinkOnScreen())
    {
        if (_settings.blinkStyle == BlinkStyle::Classic)
        {
            // Classic mode only needs redraws at toggle transitions.
            nextWakeup = std::min(nextWakeup, _slowBlinker.nextToggleIn(_currentTime));
            nextWakeup = std::min(nextWakeup, _rapidBlinker.nextToggleIn(_currentTime));
        }
        else
        {
            // Smooth/Linger modes require continuous animation at ~30fps.
            nextWakeup = std::min(nextWakeup, chrono::milliseconds { 33 });
        }
    }

    if (_statusDisplayType == StatusDisplayType::Indicator)
    {
        auto const currentSecond =
            chrono::time_point_cast<chrono::seconds>(chrono::system_clock::now()).time_since_epoch().count()
            % 60;
        auto const millisUntilNextMinute =
            chrono::duration_cast<chrono::milliseconds>(chrono::seconds(60 - currentSecond));
        nextWakeup = std::min(nextWakeup, millisUntilNextMinute);
    }

    // Screen transition animation scheduling at display refresh rate.
    if (_screenTransition.active && !_screenTransition.isComplete(_currentTime))
        nextWakeup = std::min(nextWakeup, _refreshInterval.value);

    // Cursor motion animation scheduling at display refresh rate.
    if (_cursorMotion.active && !_cursorMotion.isComplete(_currentTime))
        nextWakeup = std::min(nextWakeup, _refreshInterval.value);

    // Momentum scrolling animation scheduling at display refresh rate.
    if (_scrollMomentum.active && !_scrollMomentum.shouldStop())
        nextWakeup = std::min(nextWakeup, _refreshInterval.value);

    if (nextWakeup == chrono::milliseconds::max())
        return nullopt;

    return nextWakeup;
}

void Terminal::tick(chrono::steady_clock::time_point now) noexcept
{
    auto const changes = _changes.exchange(0);
    (void) changes;
    // TODO: update render buffer if  (changes != 0)

    _currentTime = now;
    updateCursorVisibilityState();

    if (_screenTransition.active && _screenTransition.isComplete(_currentTime))
        finalizeScreenTransition();

    if (_cursorMotion.active && _cursorMotion.isComplete(_currentTime))
        _cursorMotion.active = false;

    // Apply momentum scrolling each frame.
    if (_scrollMomentum.active)
    {
        auto const dt = chrono::duration<float>(now - _scrollMomentum.lastUpdate).count();
        if (dt > 0.0f)
        {
            // Snapshot the viewport position so we can detect a no-progress frame (glide hit the
            // history top / viewport bottom). applySmoothScrollPixelDelta clamps but still returns
            // Applied, so the result alone does not signal the boundary.
            auto const positionBefore = std::pair { _viewport.scrollOffset(), _viewport.pixelOffset() };
            auto const requestedDelta = _scrollMomentum.velocity * dt;

            auto const result = applySmoothScrollPixelDelta(requestedDelta);
            _scrollMomentum.velocity *= std::pow(_scrollMomentum.tuning.frictionDecayPerSecond, dt);
            _scrollMomentum.lastUpdate = now;

            auto const hitBoundary =
                requestedDelta != 0.0f
                && std::pair { _viewport.scrollOffset(), _viewport.pixelOffset() } == positionBefore;

            if (_scrollMomentum.shouldStop() || result != SmoothScrollResult::Applied || hitBoundary)
                cancelMomentumScroll();
        }
    }
}

void Terminal::resetSmoothScroll() noexcept
{
    cancelMomentumScroll();
    _viewport.resetPixelOffset();
}

// {{{ VelocityTracker

void Terminal::VelocityTracker::reset() noexcept
{
    count = 0;
    writeIndex = 0;
}

void Terminal::VelocityTracker::addSample(std::chrono::steady_clock::time_point time,
                                          float pixelDelta) noexcept
{
    samples[writeIndex] = { .time = time, .pixelDelta = pixelDelta };
    writeIndex = (writeIndex + 1) % MaxSamples;
    if (count < MaxSamples)
        ++count;
}

float Terminal::VelocityTracker::computeVelocity() const noexcept
{
    if (count < 2)
        return 0.0f;

    auto const oldestIndex = (writeIndex + MaxSamples - count) % MaxSamples;
    auto const newestIndex = (writeIndex + MaxSamples - 1) % MaxSamples;

    auto const dt =
        std::chrono::duration<float>(samples[newestIndex].time - samples[oldestIndex].time).count();
    if (dt <= 0.0f)
        return 0.0f;

    // Sum pixel deltas, excluding the oldest sample.
    // Each sample's pixelDelta is an incremental scroll since the previous event.
    // The oldest sample's delta occurred *before* the measurement window [t_oldest, t_newest],
    // so it must not be counted in the distance traversed during that window.
    auto totalPixels = 0.0f;
    for (size_t i = 1; i < count; ++i)
        totalPixels += samples[(oldestIndex + i) % MaxSamples].pixelDelta;

    return totalPixels / dt;
}

// }}} VelocityTracker

// {{{ ScrollMomentumState

bool Terminal::ScrollMomentumState::shouldStop() const noexcept
{
    if (!active)
        return true;
    // One stop rule for every source: whichever of the two thresholds the tuning enables (the other
    // is 0). Touchpad uses an absolute px/s floor; the wheel glide uses a fraction of its seed
    // velocity so its settle time and landed distance stay independent of the notch count.
    auto const threshold =
        std::max(tuning.minVelocityThreshold, tuning.stopVelocityFractionOfSeed * initialVelocity);
    return std::abs(velocity) < threshold;
}

// }}} ScrollMomentumState

// {{{ Momentum scrolling

void Terminal::handleScrollPhase(ScrollPhase phase,
                                 float pixelDelta,
                                 std::chrono::steady_clock::time_point now)
{
    if (!_settings.smoothScrolling || !_settings.momentumScrolling)
        return;

    switch (phase)
    {
        case ScrollPhase::Begin:
            cancelMomentumScroll();
            _scrollVelocityTracker.reset();
            break;
        case ScrollPhase::Update:
            // The first sample of a gesture (tracker still empty) supersedes any momentum still in
            // flight — including a mouse-wheel glide. This also covers a stray Update that arrives
            // without a preceding Begin: without it, the immediate apply in the caller plus the live
            // glide would both move the viewport, double-scrolling the content.
            if (_scrollVelocityTracker.count == 0)
                cancelMomentumScroll();
            _scrollVelocityTracker.addSample(now, pixelDelta);
            break;
        case ScrollPhase::End: {
            auto const velocity = _scrollVelocityTracker.computeVelocity();
            if (std::abs(velocity) >= ScrollMomentumState::StartThreshold)
                armMomentum(velocity, TouchpadMomentumTuning, now);
            _scrollVelocityTracker.reset();
            break;
        }
        case ScrollPhase::Momentum:
            // Discard OS-generated momentum events when our own momentum is active.
            break;
        case ScrollPhase::NoPhase:
            // No phase info (e.g. mouse wheel or X11 without phase support). No-op.
            break;
    }
}

void Terminal::cancelMomentumScroll() noexcept
{
    _scrollMomentum = ScrollMomentumState {};
}

bool Terminal::isMomentumScrollActive() const noexcept
{
    return _scrollMomentum.active;
}

SmoothScrollResult Terminal::injectWheelMomentum(float pixelDelta,
                                                 chrono::steady_clock::time_point now) noexcept
{
    // Gated on smoothScrolling only (independent of momentumScrolling, which governs touchpad
    // inertia). Alt screen keeps the legacy line-based wheel path.
    if (!_settings.smoothScrolling || isAlternateScreen())
        return SmoothScrollResult::Disabled;

    // The cell height is not needed by the seed math below (pixelDelta already arrives in pixels),
    // but a zero height signals the display has not laid out yet: refuse to arm and let the caller
    // fall through to the line-based path rather than silently swallowing the notch.
    auto const cellHeight = static_cast<float>(_cellPixelSize.height.as<int>());
    if (cellHeight <= 0.0f)
        return SmoothScrollResult::InvalidCellSize;

    // Seed velocity so that, under the wheel friction and the fractional stop threshold, the glide
    // integrates to the intended distance (see the derivation on WheelMomentumTuning).
    auto const v0 = pixelDelta * -std::log(WheelMomentumTuning.frictionDecayPerSecond) * WheelSeedCorrection;

    // Adding to an already-active wheel glide lets rapid notches build one longer, still-consistent
    // glide (armMomentum re-anchors the fraction-of-seed stop threshold to the accumulated velocity).
    auto const isWheelGlideActive =
        _scrollMomentum.active && _scrollMomentum.tuning.stopVelocityFractionOfSeed != 0.0f;
    auto const velocity = isWheelGlideActive ? _scrollMomentum.velocity + v0 : v0;

    // Two exactly-opposing notches (e.g. +N then -N before a frame tick) accumulate to zero. Arming
    // at velocity 0 would anchor the fraction-of-seed stop threshold to 0, so shouldStop() could
    // never fire and the glide would spin forever. Cancel any residual glide and refuse to arm.
    if (velocity == 0.0f)
    {
        cancelMomentumScroll();
        return SmoothScrollResult::InvalidCellSize;
    }

    armMomentum(velocity, WheelMomentumTuning, now);
    return SmoothScrollResult::Applied;
}

void Terminal::armMomentum(float velocity,
                           MomentumTuning const& tuning,
                           chrono::steady_clock::time_point now) noexcept
{
    _scrollMomentum.active = true;
    _scrollMomentum.tuning = tuning;
    _scrollMomentum.velocity = velocity;
    // Anchor the fraction-of-seed stop rule to this (possibly accumulated) velocity so the glide
    // stops crisply rather than crawling toward an ever-smaller threshold.
    _scrollMomentum.initialVelocity = std::abs(velocity);
    _scrollMomentum.lastUpdate = now;

    // Kick the render loop so tick() advances the glide even when the display is otherwise idle;
    // without this the display idles after the last input and momentum would only advance on the
    // next unrelated event (e.g. a mouse move).
    breakLoopAndRefreshRenderBuffer();
}

// }}} Momentum scrolling

SmoothScrollResult Terminal::applySmoothScrollPixelDelta(float pixelDelta)
{
    if (!_settings.smoothScrolling || isAlternateScreen())
        return SmoothScrollResult::Disabled;

    auto const cellHeight = static_cast<float>(_cellPixelSize.height.as<int>());
    if (cellHeight <= 0.0f)
        return SmoothScrollResult::InvalidCellSize;

    // Compute the total pixel position and decompose into whole-line scroll offset + sub-line remainder.
    // This avoids calling scrollUp/scrollDown in a loop (which triggers intermediate viewport change
    // notifications and causes visual glitches).
    auto const totalPixels = _viewport.pixelOffset() + pixelDelta;
    auto const linesDelta = static_cast<int>(std::floor(totalPixels / cellHeight));
    auto const remainder = totalPixels - static_cast<float>(linesDelta) * cellHeight;

    auto const maxOffset = boxed_cast<ScrollOffset>(primaryScreen().historyLineCount());
    auto const unclampedOffset = _viewport.scrollOffset().value + linesDelta;
    auto const newScrollOffset = std::clamp(unclampedOffset, 0, maxOffset.value);

    // Set pixel offset first (before scrollTo triggers _modified() and render buffer update).
    // If the computed offset was clamped at either boundary, zero the pixel remainder
    // to prevent overshooting past the scrollable area.
    if (unclampedOffset >= maxOffset.value)
        _viewport.setPixelOffset(0.0f); // At or past top of history.
    else if (unclampedOffset < 0)
        _viewport.setPixelOffset(0.0f); // Overshot bottom.
    else
        _viewport.setPixelOffset(remainder);

    // Apply the scroll offset change — triggers at most one _modified() notification.
    if (!_viewport.scrollTo(ScrollOffset(newScrollOffset)))
        breakLoopAndRefreshRenderBuffer(); // Pixel offset changed but scroll offset didn't.

    return SmoothScrollResult::Applied;
}

void Terminal::resizeScreenKeepingCellSize(PageSize totalPageSize)
{
    totalPageSize = clampedTotalPageSize(totalPageSize);

    // Derive the pixel size from the cell size we already have, rather than passing none at all: without
    // a pixel size UnixPty::resizeScreen() leaves ws_xpixel/ws_ypixel at zero, and TIOCSWINSZ hands the
    // child a window with no pixel geometry -- withdrawing what sixel-capable applications size their
    // images from. resizeScreen() divides this back out by the same main-page size, so the cell size comes
    // out unchanged, which is what a column-count switch wants: the cells keep their size, the page its
    // width in cells.
    auto const pixels = cellPixelSize() * (totalPageSize - statusLineHeight());

    resizeScreen(totalPageSize, pixels);

    // A selection is anchored to columns that a narrower page no longer has; renderSelection() would walk
    // it against the new grid. Every other resizeScreen() caller drops it too (@see contour::applyResize).
    clearSelection();
}

void Terminal::resizeScreen(PageSize totalPageSize, optional<ImageSize> pixels)
{
    // The total page must leave room for at least one main-display line ON TOP of the visible status
    // line(s): the main page is derived as `totalPageSize - statusLineHeight()` below, so a 1x1 total
    // with the indicator status line active (statusLineHeight() == 1, the GUI default) would yield a
    // zero-line main page and trip applyPageSizeToCurrentBuffer()/verifyState(). The frontend already
    // clamps the TOTAL to >= 1x1 (helper.cpp), which is sufficient only when no status line is shown;
    // clamp here so the backend invariant holds for every caller and every status-line height. Frontend
    // callers query the same rule via clampedTotalPageSize() to keep their bookkeeping in agreement.
    totalPageSize = clampedTotalPageSize(totalPageSize);

    // Finalize any active screen transition on resize (snapshot dimensions no longer match).
    if (_screenTransition.active)
        finalizeScreenTransition();

    // Cancel any active cursor motion animation on resize.
    _cursorMotion.active = false;

    // Reset smooth scroll state on resize.
    resetSmoothScroll();

    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.
    auto const mainDisplayPageSize = totalPageSize - statusLineHeight();

    auto const oldMainDisplayPageSize = _settings.pageSize - statusLineHeight();

    _factorySettings.pageSize = totalPageSize;
    _settings.pageSize = totalPageSize;
    // Keep the lock-free mirror in lockstep with _settings.pageSize so the render thread's totalPageSize()
    // never observes a torn value (see _atomicTotalPageSize).
    _atomicTotalPageSize.store(totalPageSize, std::memory_order_release);
    _currentMousePosition = clampToScreen(_currentMousePosition);
    if (pixels)
        // Divide the total page's pixels by the total page. Dividing by mainDisplayPageSize would
        // mix bases -- `pixels` spans the status line, mainDisplayPageSize does not -- inflating the
        // cell height. forceRedraw() feeds cellPixelSize() * totalPageSize back through here, so
        // that error compounded by roughly a pixel per call.
        setCellPixelSize(pixels.value() / totalPageSize);

    // Reset margins for all pages to defaults on resize.
    _pageMargins.fill(makeDefaultMargin(mainDisplayPageSize));

    applyPageSizeToCurrentBuffer();

    // Report the main page in both units. ws_row already excluded the status line, so passing the
    // full display's pixels alongside it made ws_ypixel/ws_row disagree with the real cell height --
    // and that division is exactly how applications derive cell size to size an image canvas.
    _pty->resizeScreen(mainDisplayPageSize,
                       pixels ? std::optional { cellPixelSize() * mainDisplayPageSize } : std::nullopt);

    // Adjust Normal-mode's cursor in order to avoid drift when growing/shrinking in main page line count.
    if (mainDisplayPageSize.lines > oldMainDisplayPageSize.lines)
        _viCommands.cursorPosition.line +=
            boxed_cast<LineOffset>(mainDisplayPageSize.lines - oldMainDisplayPageSize.lines);
    else if (oldMainDisplayPageSize.lines > mainDisplayPageSize.lines)
        _viCommands.cursorPosition.line -=
            boxed_cast<LineOffset>(oldMainDisplayPageSize.lines - mainDisplayPageSize.lines);

    _viCommands.cursorPosition = clampToScreen(_viCommands.cursorPosition);

    reportInBandWindowResize();

    verifyState();
}

void Terminal::reportInBandWindowResize()
{
    if (!_modes.enabled(DECMode::InBandWindowResize))
        return;

    // `CSI 48 ; rows ; cols ; height ; width t`. Cell counts first, pixels second -- the opposite of
    // the order XTWINOPS reports them in, which is easy to get backwards.
    //
    // The size reported is the MAIN page, not the total: the status line is the terminal's own
    // furniture and is not addressable by the application, so counting it would tell the application
    // it has a row it cannot use.
    auto const size = pageSize();
    auto const pixels = cellPixelSize() * size;
    reply("\033[48;{};{};{};{}t",
          size.lines.value,
          size.columns.value,
          pixels.height.value,
          pixels.width.value);

    // A reply produced outside the parser loop has to reach the PTY itself -- the loop's own flush is
    // what carries every other reply, and a window resize does not go through it: resizeScreen() runs
    // on the GUI thread. Left unflushed, the report sat in the input generator until some unrelated
    // event happened to flush it, which is precisely no use to the applications this mode exists for.
    // They cannot receive SIGWINCH -- they are behind a socket or an ssh multiplexer -- so an
    // application blocked on read across a resize went on drawing at the old size indefinitely.
    // Flushing here rather than at the call site is what keeps that true for every caller; the parser
    // path (setMode) reaches this too, where flushInput() is equally safe and already the convention
    // for a replying sequence handler. @see Screen::reportColorPaletteUpdate.
    flushInput();
}

void Terminal::verifyState()
{
#if defined(CONTOUR_VERIFY_STATE)
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

    // Margins are terminal state, so verify them against the terminal's OWN main-display page size —
    // the exact basis resizeScreen() fills them with. pageSize() would compare against the PTY's page
    // size instead, which freezes once the PTY closes (UnixPty::resizeScreen early-returns without a
    // master fd), so any grid change after the shell exited (DPR settlement, window growth, pane
    // teardown reflows) made the two legitimately diverge and aborted debug builds.
    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();
    Require(0 <= *currentPageMargin().horizontal.from
            && *currentPageMargin().horizontal.to < *mainDisplayPageSize.columns);
    Require(0 <= *currentPageMargin().vertical.from
            && *currentPageMargin().vertical.to < *mainDisplayPageSize.lines);
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
    struct SelectionRenderer
    {
        gsl::not_null<Terminal const*> term;
        ColumnOffset rightPage {};
        ColumnOffset lastColumn {};
        string text {};
        string currentLine {};
        bool currentLineHasContent = false;

        /// Whether any line has been written to @c text yet, which is what decides if the next one
        /// needs a separator before it. The accumulated text cannot answer that: a blank line is a
        /// line, but contributes no characters.
        bool linesEmitted = false;

        /// Whether a cell has been taken for the line being built. A one-column selection emits every
        /// callback at the same column, so this -- not the accumulated text -- is what tells the
        /// break test that a line is already under way.
        bool lineStarted = false;

        SelectionRenderer(Terminal const& term, ColumnOffset rightPage): term(&term), rightPage(rightPage) {}

        void operator()(CellLocation pos, CellProxy const& cell)
        {
            // "Have we started a line yet" is answered by whether a cell has been taken, not by
            // whether the accumulated text is non-empty -- the same distinction flushLine draws. A
            // one-column rectangular selection emits every callback at the SAME column, so this
            // predicate is the only thing that can start a new line, and the text proxy can never
            // become true because text is only written inside the flush it was gating: the whole
            // selection came out as one run.
            auto const isNewLine = pos.column < lastColumn || (pos.column == lastColumn && lineStarted);
            if (isNewLine && (!term->isLineWrapped(pos.line)))
                // TODO: handle logical line in word-selection (don't include LF in wrapped lines)
                flushLine();

            lineStarted = true;
            lastColumn = pos.column;

            // A cell that only CONTINUES another carries no text of its own -- horizontally, where its
            // codepoint is 0, or vertically, where a tall block reaches down into the row. Without
            // this they fall into the empty-cell branch below and copy as spaces: 中a would yield
            // "中 a", and a six-column text-sizing block would trail five spaces.
            if (cell.isFlagEnabled(CellFlag::WideCharContinuation)
                || cell.isFlagEnabled(CellFlag::MulticellContinuation))
                return;

            currentLineHasContent = true;
            if (cell.empty())
                currentLine += ' ';
            else
                currentLine += cell.toUtf8();
        }

        /// Ends the line being built, dropping it entirely when it held nothing but continuation cells.
        ///
        /// Such a row is not a line of text -- it is the lower half of the blocks on the row above --
        /// so emitting its break would copy a scaled word as "ab\n". kitty trims exactly these rows in
        /// flag_selection_to_extract_text(). A genuinely BLANK selected line still counts as content,
        /// because a blank line inside a selection is a line the user selected.
        void flushLine()
        {
            if (currentLineHasContent)
            {
                // The break is a SEPARATOR between lines that carry content, not a terminator emitted
                // when one ends -- otherwise a trailing continuation-only row, whose emptiness is only
                // known after the break was already written, leaves "ab\n" behind.
                //
                // What decides that is whether a line has been emitted, NOT whether the accumulated
                // text is non-empty: a selected blank line carries content (the user selected it) but
                // trims to nothing, so leading blank lines left the text empty and their separators
                // were never written. Interior ones survived, which made the loss position-dependent.
                if (linesEmitted)
                    text += '\n';
                trimSpaceRight(currentLine);
                text += currentLine;
                linesEmitted = true;
            }
            currentLine.clear();
            currentLineHasContent = false;
            lineStarted = false;
        }

        std::string finish()
        {
            flushLine();
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

    auto const& screen = pageAt(_displayedPage);
    auto se = SelectionRenderer { *this, pageSize().columns.as<ColumnOffset>() - 1 };
    vtbackend::renderSelection(*_selection, [&](CellLocation pos) { se(pos, screen.at(pos)); });
    return se.finish();
}

void Terminal::selectAll()
{
    auto const& grid = _currentScreen->grid();

    auto const top =
        CellLocation { .line = -boxed_cast<LineOffset>(grid.historyLineCount()), .column = ColumnOffset(0) };
    auto const bottom =
        CellLocation { .line = boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1),
                       .column = boxed_cast<ColumnOffset>(pageSize().columns) - ColumnOffset(1) };

    // FullLineSelection rather than LinearSelection: it normalizes the columns to whole lines and follows
    // wrapped ones, which is what "all" means here (and what ViMode's VisualLine already does).
    setSelector(std::make_unique<FullLineSelection>(_selectionHelper, top, selectionUpdatedHelper()));
    (void) _selection->extend(bottom);

    // Completing a selection is Insert mode's business — exactly the gate sendMouseReleaseEvent() applies
    // to a finished drag. In a Visual mode the Vi layer owns the selection and every motion extends it
    // (ViCommands::moveCursorTo → Selection::extend, whose first statement is an assert that the state is
    // not Complete), so handing it a completed one aborts on the next keystroke.
    if (_inputHandler.mode() == ViMode::Insert)
        _selection->complete();

    // Deliberately NOT updateSelectionMatches(): that serializes the selection into a search pattern to
    // highlight the other occurrences of a selected WORD. For a selection that spans the whole scrollback
    // the pattern is the entire buffer — megabytes built under the terminal lock, for a search whose only
    // match is what is already selected, and which then disables the trivial-line render fast path for
    // every frame that follows. The quadruple-click full-line selection skips it for the same reason.
    onSelectionUpdated();
}

std::optional<CommandBlockText> Terminal::lastCommandBlock() const
{
    return primaryScreen().lastCommandBlock();
}

std::expected<LivePromptSpan, PromptRegionError> Terminal::livePromptSpan() const
{
    // An alt-screen application owns the whole page and paints no shell prompt into it, so the primary
    // screen's marks — which are still there, waiting below — say nothing about where the caret now is.
    if (isAlternateScreen())
        return std::unexpected(PromptRegionError::NoPromptMark);

    return primaryScreen().livePromptSpan();
}

string Terminal::extractLastMarkRange() const
{
    // -1 because we always want to start extracting one line above the cursor by default.
    // The cursor is read from the PRIMARY screen, which is also the grid the lines are read from below: an
    // alt-screen app (vim, less) has a cursor of its own, and pairing it with the primary grid would slice
    // a line range out of one screen using the other screen's cursor.
    auto const bottomLine =
        primaryScreen().cursor().position.line + LineOffset(-1) + _settings.copyLastMarkRangeOffset;

    auto const marker1 = optional { bottomLine };

    auto const marker0 = primaryScreen().findMarkerUpwards(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 + 1;
    auto const lastLine = *marker1;

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        text += primaryScreen().grid().lineAt(lineNum).toUtf8Trimmed();
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
    forceAutoScrollToBottomIfEnabled();
    _eventListener.bufferChanged(type);
}

void Terminal::scrollbackBufferCleared()
{
    clearSelection();
    autoScrollToBottomIfEnabled();
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

void Terminal::setPointerShape(std::string shape)
{
    // A Set at the bottom of the stack makes that entry the APPLICATION's base shape rather than the
    // terminal's default, which is what a later pop has to restore.
    if (_pointerShapes.size() == 1)
        _pointerShapeBaseSetByApplication = true;

    _pointerShapes.back() = std::move(shape);
    _eventListener.setPointerShape(_pointerShapes.back());
}

void Terminal::pushPointerShape(std::string shape)
{
    // Bounded so that an application looping on push cannot grow this without limit. Past the cap the
    // newest shape still takes effect; only the ability to restore that many levels is lost.
    constexpr auto MaxDepth = size_t { 16 };
    if (_pointerShapes.size() >= MaxDepth)
        _pointerShapes.back() = std::move(shape);
    else
        _pointerShapes.push_back(std::move(shape));
    _eventListener.setPointerShape(_pointerShapes.back());
}

void Terminal::popPointerShape()
{
    if (_pointerShapes.size() > 1)
        _pointerShapes.pop_back();

    // Landing on the bottom means the application is no longer imposing anything ONLY when it never
    // set a base shape -- "an empty stack means the terminal is free to use whatever shape it likes".
    // Reporting the bottom entry's name in that case would look identical to an application setting
    // it, and a frontend caching the application's choice would never restore its own screen-type
    // defaults again. But when the application DID set a base shape, that shape is exactly what the
    // pop restores, and signalling a reset instead would throw away a shape it never withdrew.
    if (_pointerShapes.size() == 1 && !_pointerShapeBaseSetByApplication)
        resetPointerShape();
    else
        _eventListener.setPointerShape(_pointerShapes.back());
}

void Terminal::resetPointerShape()
{
    _pointerShapes.erase(std::next(_pointerShapes.begin()), _pointerShapes.end());
    _pointerShapes.back() = std::string(pointer_shape::DefaultName);
    _pointerShapeBaseSetByApplication = false;

    // The empty name is the signal, distinct from any shape an application can name: the terminal is
    // back to its own default and a frontend may resume its own.
    _eventListener.setPointerShape("");
}

void Terminal::copyToClipboard(string_view data)
{
    _eventListener.copyToClipboard(data);
}

void Terminal::requestClipboardRead(string_view pc)
{
    // Reading the clipboard is opt-in: an application that could read it unbidden could exfiltrate
    // whatever the user last copied. When disabled, stay silent (as xterm does when the operation is
    // not permitted) rather than reveal even that a clipboard exists.
    if (!_settings.allowClipboardRead)
        return;

    auto const content = _eventListener.getClipboard();
    // An empty Pc is reported back as xterm's default selection, "s0".
    auto const selection = pc.empty() ? string_view { "s0" } : pc;
    reply("\033]52;{};{}\033\\", selection, crispy::base64::encode(content.begin(), content.end()));
}

void Terminal::openDocument(string_view data)
{
    _eventListener.openDocument(data);
}

std::optional<std::string> Terminal::localPathAtMousePosition() const
{
    auto const mousePosition = currentMouseGridPosition();
    if (!mousePosition)
        return std::nullopt;

    auto const lineText = currentScreen().lineTextAt(mousePosition->line, false, false);
    auto const mouseColumn = static_cast<size_t>(*mousePosition->column);
    auto const cwd = extractPathFromFileUrl(currentWorkingDirectory());
    auto const* const homeEnv = std::getenv("HOME");
    auto const home = std::string(homeEnv ? homeEnv : "");

    static auto const localPathRegex = [] {
        // Matches, in order: drive-letter absolute paths (C:/foo, C:\foo) for Windows,
        // ~/ home-relative paths, / absolute paths, ./ and ../ relative paths, paths
        // containing a separator, and bare filenames. Both '/' and '\\' separators are
        // accepted so native Windows paths are detected as well as POSIX ones. A literal '~'
        // is allowed inside path components so Windows 8.3 short names (e.g. RUNNER~1) and
        // tilde-suffixed backup files (e.g. file~) are matched in full rather than truncated.
        return std::regex(
            R"((?:[A-Za-z]:[\\/][\w.~\\/-]+|~?[\\/][\w.~\\/-]+|\.{1,2}[\\/][\w.~\\/-]+|[\w.][\w.~-]*[\\/][\w.~\\/-]+|[\w.][\w.~-]+))",
            std::regex_constants::ECMAScript | std::regex_constants::optimize);
    }();

    auto matchIter = std::sregex_iterator(lineText.begin(), lineText.end(), localPathRegex);
    auto const matchEnd = std::sregex_iterator();
    for (; matchIter != matchEnd; ++matchIter)
    {
        auto const& match = *matchIter;
        if (match.empty())
            continue;

        auto const startColumn =
            utf8ByteOffsetToCodepointIndex(lineText, static_cast<size_t>(match.position()));
        auto const endColumn =
            utf8ByteOffsetToCodepointIndex(lineText, static_cast<size_t>(match.position() + match.length()))
            - 1;

        if (mouseColumn < startColumn || mouseColumn > endColumn)
            continue;

        if (auto path = resolveExistingLocalPath(cwd, home, match.str()))
            return path;
    }

    return std::nullopt;
}

// {{{ Hint mode

void Terminal::refreshHints()
{
    auto const lines = unbox<int>(pageSize().lines);
    auto const scrollOff = unbox<int>(_viewport.scrollOffset());
    auto visibleLines = std::vector<std::string>();
    visibleLines.reserve(static_cast<size_t>(lines));
    for (auto const i: std::views::iota(0, lines))
        visibleLines.push_back(currentScreen().lineTextAt(LineOffset(i - scrollOff), false, false));

    _hintModeHandler.refresh(visibleLines, pageSize());
}

void Terminal::activateHintMode(std::vector<HintPattern> const& patterns, HintAction action)
{
    auto const lines = unbox<int>(pageSize().lines);
    auto const scrollOff = unbox<int>(_viewport.scrollOffset());
    auto visibleLines = std::vector<std::string>();
    visibleLines.reserve(static_cast<size_t>(lines));
    for (auto const i: std::views::iota(0, lines))
        visibleLines.push_back(currentScreen().lineTextAt(LineOffset(i - scrollOff), false, false));

    // Make a mutable copy so we can attach validators.
    auto mutablePatterns = patterns;

    // When CWD is available, attach a filesystem-existence validator to filepath patterns.
    auto const cwdUrl = currentWorkingDirectory();
    auto const cwd = extractPathFromFileUrl(cwdUrl);
    if (!cwd.empty())
    {
        auto const* const homeEnv = std::getenv("HOME");
        auto const home = std::string(homeEnv ? homeEnv : "");
        for (auto& pattern: mutablePatterns)
        {
            if (pattern.name != "filepath")
                continue;

            // With CWD available, broaden the regex to also match bare filenames,
            // extensionless files (e.g. "Makefile"), and directories (e.g. "src").
            // The validator ensures only entries that actually exist on disk are kept.
            pattern.regex =
                std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                           std::regex_constants::ECMAScript | std::regex_constants::optimize);

            // Resolve a matched path to an absolute filesystem path.
            // When HOME is unset and the path starts with ~/, return it unchanged.
            auto const resolvePath = [cwd, home](std::string const& matchStr) -> std::string {
                if (matchStr.starts_with("/"))
                    return matchStr;
                if (matchStr.starts_with("~/"))
                    return home.empty() ? matchStr : home + matchStr.substr(1);
                return cwd + "/" + matchStr;
            };

            pattern.validator = [resolvePath, home](std::string const& matchStr) -> bool {
                // Cannot resolve ~/ paths when HOME is unset — let them through unvalidated.
                if (matchStr.starts_with("~/") && home.empty())
                    return true;
                auto ec = std::error_code {};
                return std::filesystem::exists(resolvePath(matchStr), ec);
            };

            // Transform matched text to absolute path so Copy/Open actions work correctly.
            pattern.transformer = resolvePath;
        }
    }

    _hintModeHandler.activate(visibleLines, pageSize(), mutablePatterns, action);
    _inputHandler.setMode(ViMode::Hint);
}

void Terminal::applyHintOverlay(RenderBuffer& output, LineOffset baseLine) const
{
    if (!_hintModeHandler.isActive())
        return;

    auto const& matches = _hintModeHandler.matches();
    auto const& filter = _hintModeHandler.currentFilter();

    // Resolve palette colors for hint rendering.
    // CellRGBColor is a variant that may hold RGBColor directly, or CellForegroundColor/CellBackgroundColor
    // sentinels that refer to the terminal's default foreground/background.
    auto const resolveColor = [&](CellRGBColor const& color) -> RGBColor {
        if (std::holds_alternative<CellForegroundColor>(color))
            return _colorPalette.defaultForeground;
        if (std::holds_alternative<CellBackgroundColor>(color))
            return _colorPalette.defaultBackground;
        return std::get<RGBColor>(color);
    };

    auto const& hintLabel = _colorPalette.hintLabel;
    auto const& hintMatch = _colorPalette.hintMatch;
    auto const labelFg = resolveColor(hintLabel.foreground);
    auto const labelBg = resolveColor(hintLabel.background);
    // Dimmed version of label colors for the already-typed portion.
    auto const typedLabelFg = mixColor(labelFg, _colorPalette.defaultBackground, 0.5f);
    auto const typedLabelBg = mixColor(labelBg, _colorPalette.defaultBackground, 0.5f);
    auto const matchBg = resolveColor(hintMatch.background);
    auto const matchBgAlpha = hintMatch.backgroundAlpha;

    for (auto const& match: matches)
    {
        auto const labelLen = static_cast<int>(match.label.size());
        auto const matchLine = baseLine + match.start.line;

        // Apply overlay for each cell in the RenderBuffer.
        for (auto& cell: output.cells)
        {
            auto const line = cell.position.line;
            auto const col = cell.position.column;

            // Check if this cell is in the label region.
            if (line == matchLine && col >= match.start.column
                && col < match.start.column + ColumnOffset(labelLen))
            {
                auto const labelIdx = static_cast<size_t>(unbox(col) - unbox(match.start.column));
                if (labelIdx < match.label.size())
                {
                    // Replace codepoints with label character.
                    cell.codepoints = std::u32string(1, static_cast<char32_t>(match.label[labelIdx]));
                    cell.width = 1;

                    // Distinguish typed-so-far from remaining label characters.
                    if (labelIdx < filter.size())
                    {
                        // Already-typed portion: dimmed label styling.
                        cell.attributes.foregroundColor = typedLabelFg;
                        cell.attributes.backgroundColor = typedLabelBg;
                    }
                    else
                    {
                        // Remaining label: bright label styling from palette.
                        cell.attributes.foregroundColor = labelFg;
                        cell.attributes.backgroundColor = labelBg;
                    }
                    cell.attributes.flags |= CellFlag::Bold;
                }
            }
            // Check if this cell is in the match body region (after the label).
            else if (line == matchLine && col > match.start.column + ColumnOffset(labelLen - 1)
                     && col <= match.end.column)
            {
                // Highlight match body with palette-configured background blend.
                cell.attributes.backgroundColor =
                    mixColor(cell.attributes.backgroundColor, matchBg, matchBgAlpha);
            }
        }
    }

    // Dim all cells that don't belong to any match when hints are active.
    // This helps the labels stand out.
    if (!matches.empty())
    {
        for (auto& cell: output.cells)
        {
            auto const line = cell.position.line;
            auto const col = cell.position.column;

            auto const belongsToMatch = std::ranges::any_of(matches, [&](auto const& m) {
                auto const mLine = baseLine + m.start.line;
                return line == mLine && col >= m.start.column && col <= m.end.column;
            });

            if (!belongsToMatch)
            {
                // Fade non-match cells toward the terminal's default background.
                blendAttributesTo(cell.attributes, _colorPalette.defaultBackground, 0.5f);
            }
        }
    }
}

void Terminal::HintModeExecutor::onHintSelected(std::string const& matchedText,
                                                HintAction action,
                                                CellLocation start,
                                                CellLocation end)
{
    switch (action)
    {
        case HintAction::Copy: terminal._eventListener.copyToClipboard(matchedText); break;
        case HintAction::Open: terminal._eventListener.openDocument(matchedText); break;
        case HintAction::Paste: terminal.sendRawInput(matchedText); break;
        case HintAction::CopyAndPaste:
            terminal._eventListener.copyToClipboard(matchedText);
            terminal.sendRawInput(matchedText);
            break;
        case HintAction::Select: {
            // Convert viewport-relative coordinates to grid-relative coordinates.
            auto const scrollOff = LineOffset::cast_from(terminal._viewport.scrollOffset());
            auto const gridStart = CellLocation { .line = start.line - scrollOff, .column = start.column };
            auto const gridEnd = CellLocation { .line = end.line - scrollOff, .column = end.column };

            // Clear any existing selection so that entering Visual mode uses our
            // cursor position as the anchor, not a stale selection start.
            terminal.clearSelection();

            // Enter vi visual mode with the match range pre-selected.
            terminal._viCommands.cursorPosition = gridStart;
            terminal._inputHandler.setMode(ViMode::Visual);
            terminal._viCommands.moveCursorTo(gridEnd);
            break;
        }
    }
}

void Terminal::HintModeExecutor::onHintModeEntered()
{
    previousViMode = terminal._inputHandler.mode();
    terminal.breakLoopAndRefreshRenderBuffer();
}

void Terminal::HintModeExecutor::onHintModeExited()
{
    terminal._inputHandler.setMode(previousViMode.value_or(ViMode::Normal));
    previousViMode.reset();
    terminal.breakLoopAndRefreshRenderBuffer();
}

void Terminal::HintModeExecutor::requestRedraw()
{
    terminal.breakLoopAndRefreshRenderBuffer();
}
// }}} Hint mode

void Terminal::inspect()
{
    _eventListener.inspect();
}

void Terminal::notify(string_view title, string_view body)
{
    _eventListener.notify(title, body);
}

void Terminal::showDesktopNotification(DesktopNotification const& notification)
{
    _eventListener.showDesktopNotification(notification);
}

void Terminal::discardDesktopNotification(string_view identifier)
{
    _eventListener.discardDesktopNotification(identifier);
}

void Terminal::focusTerminalWindow()
{
    _eventListener.focusTerminalWindow();
}

std::string foldC1ControlsToEightBit(std::string_view sevenBit)
{
    // A single-pass state machine: hold back each ESC and decide when its following byte arrives.
    // `ESC X` with X in 0x40..0x5F is a 7-bit C1 control and folds to the single byte X + 0x40; anything
    // else (a lone trailing ESC, or ESC before a non-C1 byte) is emitted verbatim.
    std::string out;
    out.reserve(sevenBit.size());
    auto pendingEsc = false;
    for (auto const ch: sevenBit)
    {
        auto const byte = static_cast<unsigned char>(ch);
        if (pendingEsc)
        {
            pendingEsc = false;
            if (byte >= 0x40 && byte <= 0x5F)
            {
                out.push_back(static_cast<char>(byte + 0x40));
                continue;
            }
            out.push_back('\033'); // the ESC we held back was not a C1 introducer
        }
        if (byte == 0x1B)
            pendingEsc = true; // hold the ESC; the next byte decides whether it is a C1 control
        else
            out.push_back(ch);
    }
    if (pendingEsc)
        out.push_back('\033'); // a lone ESC at the very end
    return out;
}

void Terminal::reply(string_view text)
{
    // this is invoked from within the terminal thread.
    // most likely that's not the main thread, which will however write
    // the actual input events.
    // TODO: introduce new mutex to guard terminal writes.

    // Under S8C1T the terminal transmits its C1 control introducers as single 8-bit bytes (CSI -> 0x9B,
    // DCS -> 0x90, ST -> 0x9C, ...). 8-bit C1 transmission is a VT200+ capability, so it applies only
    // while operating at VT level 2 or above: a terminal that has dropped back to VT100 level -- e.g.
    // after a VT52 round-trip, where setVT52Mode() resets the operating level -- replies in 7-bit even
    // if S8C1T was selected earlier. This is xterm's rule and is exactly what vttest's post-VT52 check
    // expects.
    if (_c1TransmissionMode == ControlTransmissionMode::S8C1T && conformanceLevelOf(_operatingLevel) >= 2)
        _inputGenerator.generateRaw(foldC1ControlsToEightBit(text));
    else
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

void Terminal::requestWindowIconify(bool iconify)
{
    _eventListener.requestWindowIconify(iconify);
}

void Terminal::requestWindowMove(WindowPosition position)
{
    _eventListener.requestWindowMove(position);
}

void Terminal::requestWindowMaximize(WindowMaximize how)
{
    _eventListener.requestWindowMaximize(how);
}

void Terminal::requestWindowFullScreen(WindowFullScreen how)
{
    _eventListener.requestWindowFullScreen(how);
}

void Terminal::setApplicationkeypadMode(bool enabled)
{
    _inputGenerator.setApplicationKeypadMode(enabled);
}

void Terminal::setBracketedPaste(bool enabled)
{
    _inputGenerator.setBracketedPaste(enabled);
}

void Terminal::setModifyOtherKeys(int mode)
{
    _inputGenerator.setModifyOtherKeys(mode);
}

int Terminal::modifyOtherKeys() const noexcept
{
    return _inputGenerator.modifyOtherKeys();
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

void Terminal::setMouseWheelScrollMultiplier(LineCount lines)
{
    _settings.mouseWheelScrollMultiplier = lines;
    _inputGenerator.setMouseWheelScrollMultiplier(static_cast<unsigned>(unbox(lines)));
}

void Terminal::setWindowTitle(string_view title)
{
    // Writes _windowTitle lock-free intentionally: the parser-thread callers (OSC 0/2 dispatch in
    // Screen, save/restoreWindowTitle) reach this from inside writeToScreen()'s _stateMutex hold, so
    // the write is already serialized under the lock; taking the non-recursive _stateMutex here would
    // self-deadlock. GUI-thread readers must use resolvedWindowTitle(), which locks and copies.
    _windowTitle = title;
    _eventListener.setWindowTitle(title);
}

std::string const& Terminal::windowTitle() const noexcept
{
    return _windowTitle;
}

std::string Terminal::resolvedWindowTitle() const
{
    auto const l = std::lock_guard { _stateMutex };
    return _windowTitle;
}

std::string Terminal::decodeTitle(std::string_view raw) const
{
    // Under SetHex the OSC title argument is a hex string (xterm's title mode 0). A malformed hex string
    // (odd length or a non-hex digit) leaves fromHexString empty; fall back to the raw bytes then, as
    // there is nothing better to decode.
    if (isTitleModeEnabled(TitleModeFeature::SetHex))
        if (auto decoded = crispy::fromHexString(raw); decoded.has_value())
            return std::move(*decoded);
    return std::string { raw };
}

std::string Terminal::encodeTitleForReport(std::string_view title) const
{
    if (!isTitleModeEnabled(TitleModeFeature::QueryHex))
        return std::string { title };

    // xterm's title mode 1 reports the label as lowercase hexadecimal, one byte at a time (masking to a
    // byte so high UTF-8 bytes encode as e.g. "c3a9", not a sign-extended value).
    std::string hex;
    hex.reserve(title.size() * 2);
    for (auto const ch: title)
        hex += std::format("{:02x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
    return hex;
}

void Terminal::setTabName(string_view title)
{
    // Parser-thread path (SETTABNAME escape sequence, OSC 30): writeToScreen() already holds _stateMutex
    // across the whole parse, so _tabName is written under the lock here.
    _tabName = title;
    _eventListener.setTabName(title);
}

std::optional<std::string> Terminal::tabName() const noexcept
{
    return _tabName;
}

std::optional<std::string> Terminal::resolvedTabName() const
{
    // Single lock hold for the whole resolution: _tabName/_windowTitle are written on the parser thread
    // under _stateMutex (setTabName()/setWindowTitle()), and this runs on the GUI thread, so reading them
    // unlocked (or across three separate locked accessor calls) would race the writer. getTabsNamingMode()
    // reads _settings, which is not mutated by the parser thread, so it needs no extra protection.
    auto const l = std::lock_guard { _stateMutex };
    if (_tabName)
        return _tabName;
    if (_settings.tabNamingMode == TabsNamingMode::Title)
        return _windowTitle;
    return std::nullopt;
}

void Terminal::setIconTitle(string_view title)
{
    // Written lock-free for the same reason setWindowTitle() is: every caller reaches this from the
    // parser thread, inside writeToScreen()'s _stateMutex hold.
    _iconTitle = title;
    _eventListener.setIconTitle(title);
}

std::string const& Terminal::iconTitle() const noexcept
{
    return _iconTitle;
}

void Terminal::saveTitles(TitleKinds kinds)
{
    // A push onto a full stack discards the oldest entry, rather than letting an application grow the
    // stack without bound.
    if (_savedTitles.size() >= MaxSavedTitles)
        _savedTitles.erase(_savedTitles.begin());

    _savedTitles.push_back(SavedTitles {
        .icon = kinds.test(TitleKind::Icon) ? std::optional { _iconTitle } : std::nullopt,
        .window = kinds.test(TitleKind::Window) ? std::optional { _windowTitle } : std::nullopt,
    });
}

void Terminal::restoreTitles(TitleKinds kinds)
{
    if (_savedTitles.empty())
        return;

    // One entry comes off the stack, whatever it holds -- so pushing both titles and then popping only
    // the icon's leaves nothing behind for a later pop of the window's.
    auto const top = _savedTitles.back();
    _savedTitles.pop_back();

    // An entry that does not carry the title we were asked for sends us looking further down the stack
    // for the nearest entry that does. That is what makes "push the icon's, push the window's, pop both"
    // restore both, rather than only the window's. @see xterm's TryHigher().
    auto const deeper = [this](auto SavedTitles::* title) -> std::optional<std::string> {
        for (auto const& entry: _savedTitles | std::views::reverse)
            if ((entry.*title).has_value())
                return entry.*title;
        return std::nullopt;
    };

    if (kinds.test(TitleKind::Icon))
        if (auto const title = top.icon.has_value() ? top.icon : deeper(&SavedTitles::icon);
            title.has_value())
            setIconTitle(*title);

    if (kinds.test(TitleKind::Window))
        if (auto const title = top.window.has_value() ? top.window : deeper(&SavedTitles::window);
            title.has_value())
            setWindowTitle(*title);
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

    // LNM has two halves. The output half (LF also returns the carriage) reads the mode bit in
    // Screen::linefeed(); the input half (Return sends CR LF) lives in the input generator, which
    // has no view of the mode register and must therefore be told.
    if (mode == AnsiMode::AutomaticNewLine)
        _inputGenerator.setAutomaticNewLineMode(enable);

    _modes.set(mode, enable);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Terminal::setMode(DECMode mode, bool enable)
{
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
        // AutoWrap (DECAWM) is a terminal mode with no per-cursor copy: it is stored in _modes below and
        // read via isModeEnabled(). Keeping it out of the Cursor struct is what makes DECSC/DECRC leave
        // it alone -- autowrap is not cursor state (DEC STD 070), unlike DECOM.
        case DECMode::AutoWrap: break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!enable)
            {
                currentPageMargin().horizontal =
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

            // Erases all data in page memory -- unless DECNCSM (No Clearing Screen on Column change)
            // is set, in which case a column-width change preserves the page (VT500 behaviour).
            if (!isModeEnabled(DECMode::NoClearScreenOnColumnChange))
                clearScreen();

            // DECCOLM is authoritative and synchronous. An application switches to 132 columns and
            // immediately draws to the new width in the same output burst (vttest's cursor-movement test
            // is the canonical example: it addresses the box border at absolute column 132 right after
            // the switch). Resize the grid here — on the parser thread, under the state lock
            // writeToScreen() already holds — so that drawing lands on the new width. The window is asked
            // to follow below, best-effort. Relying on the window resize alone (as this once did) resizes
            // the grid a GUI round-trip later, by which time the application has already drawn onto the
            // old, narrower page.
            resizeScreenKeepingCellSize(PageSize { totalPageSize().lines, columns });

            // The frontend is handed the USABLE (main-page) line count, not the total: it adds the
            // status-line height back itself (as it does for XTWINOPS `CSI 8 t`). Passing
            // totalPageSize().lines here double-counts the status line, growing the window one row on
            // every DECCOLM when the indicator status line is shown.
            requestWindowResize(PageSize { pageSize().lines, columns });
        }
        break;
        case DECMode::BatchedRendering:
            if (_modes.enabled(DECMode::BatchedRendering) != enable)
                synchronizedOutput(enable);
            break;
        case DECMode::InBandWindowResize:
            // Report once on enabling, so an application never has to ask separately for the size it
            // starts with. The mode has to be recorded first -- reportInBandWindowResize() checks it.
            if (enable && !_modes.enabled(DECMode::InBandWindowResize))
            {
                _modes.set(DECMode::InBandWindowResize, true);
                reportInBandWindowResize();
            }
            break;
        case DECMode::TextReflow:
            if (_settings.primaryScreen.allowReflowOnResize && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = enable ? LineOffset(0) : currentScreen().cursor().position.line;
                for (auto line = startLine; line < boxed_cast<LineOffset>(_settings.pageSize.lines); ++line)
                    primaryScreen().grid().lineAt(line).setWrappable(enable);
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(enable);
            break;
        case DECMode::UseAlternateScreen: // DECSET 47
        case DECMode::OptionalAltScreen:  // DECSET 1047
        case DECMode::ExtendedAltScreen:  // DECSET 1049
            // The three alternate-screen modes differ only in their cursor-carry and clear policy,
            // which alternateScreenBehavior() describes as data. @see Terminal::setAlternateScreen.
            setAlternateScreen(mode, enable);
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
        case DECMode::PageCursorCoupling:
            if (enable && _displayedPage != _cursorPage)
                _displayedPage = _cursorPage;
            break;
        case DECMode::DesignateCharsetUSASCII:
            // DEC private mode 2 is DECANM: reset (`CSI ? 2 l`) enters VT52. The set form (`CSI ? 2 h`,
            // select ANSI) is a no-op -- it can only ever be received in ANSI mode, since VT52 has no
            // CSI grammar, so it must NOT drop the operating level. VT52 is left only by `ESC <`.
            if (!enable)
                setVT52Mode(true);
            break;
        case DECMode::ApplicationKeypad: setApplicationkeypadMode(enable); break;
        case DECMode::AutoRepeat: break;
        case DECMode::BackarrowKey: _inputGenerator.setBackarrowKeyMode(enable); break;
        case DECMode::Win32InputMode: _inputGenerator.setWin32InputMode(enable); break;
        case DECMode::SemanticBlockProtocol:
            _semanticBlockTracker.setEnabled(enable);
            if (enable)
            {
                auto const& t = *_semanticBlockTracker.token();
                reply("\033P>2034;1b{};{};{};{}\033\\", t[0], t[1], t[2], t[3]);
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
        currentPageMargin().vertical.from = sanitizedTop;
        currentPageMargin().vertical.to = sanitizedBottom;
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
        currentPageMargin().horizontal.from = sanitizedLeft;
        currentPageMargin().horizontal.to = sanitizedRight;
    }
}

void Terminal::clearScreen()
{
    pageAt(_cursorPage).clearScreen();
}

void Terminal::setAlternateScreen(DECMode mode, bool enable)
{
    // Modes 47, 1047 and 1049 all switch between the primary and alternate screen buffers; they differ
    // only in whether they carry the cursor across, and whether they erase the alternate page on the
    // way in or out. That policy is data (alternateScreenBehavior), so the switch below is a single
    // path, modelled on xterm's ToAlternate / FromAlternate (charproc.c).
    auto const behavior = alternateScreenBehavior(mode);
    Require(behavior.has_value());

    if (enable && !isAlternateScreen())
    {
        // xterm's ToAlternate: switch in, carrying the cursor across (it is terminal-level, not
        // per-buffer, so it does not move), then optionally erase the alternate page. The alternate
        // page inherits the primary's margins, as xterm's alternate screen traditionally does.
        _pageMargins[AlternateScreenPageIndex.value] = currentPageMargin();
        auto const carried = _currentScreen->cursor();
        setScreen(ScreenType::Alternate);
        if (behavior->carryCursor)
            _currentScreen->cursor() = carried;
        if (behavior->clearOnEnter)
            clearScreen();
    }
    else if (!enable && isAlternateScreen())
    {
        // xterm's FromAlternate: optionally erase the alternate page first (ED 2 leaves the cursor put),
        // then switch back, carrying the cursor across.
        if (behavior->clearOnExit)
            clearScreen();
        auto const carried = _currentScreen->cursor();
        setScreen(ScreenType::Primary);
        if (behavior->carryCursor)
            _currentScreen->cursor() = carried;
    }

    // Modes 47, 1047 and 1049 are three views of one piece of state -- whether the alternate screen
    // buffer is in use -- so DECRQM must report all three consistently, no matter which one switched
    // (xterm keys every one off screen->whichBuf). Mirror the resulting buffer state onto all three
    // bits; the trailing _modes.set(mode, enable) in setMode() then agrees with it.
    auto const onAlternate = isAlternateScreen();
    for (auto const altMode:
         { DECMode::UseAlternateScreen, DECMode::OptionalAltScreen, DECMode::ExtendedAltScreen })
        _modes.set(altMode, onAlternate);
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
    setGraphicsRendition(GraphicsRendition::Reset); // SGR
    _currentScreen->resetSavedCursorState();        // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true);          // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);                // DECOM
    // DECLRMM (left/right margin mode). DEC STD 070 lists it among the modes DECSTR resets; turning it
    // off here also restores the horizontal margins to full width and swaps DECSLRM back out for SCOSC
    // (see setMode's LeftRightMargin case), so a later DECSLRM is inert until DECLRMM is set again.
    setMode(DECMode::LeftRightMargin, false); // DECLRMM
    setMode(AnsiMode::KeyboardAction, false); // KAM

    // DECAWM. The VT510 manual has DECSTR RESET autowrap, and every terminal in the field declines to:
    // xterm restores the bit to the value it was configured with rather than clearing it, foot turns
    // auto-margin back on unconditionally, and wezterm says so in as many words — "xterm deviates from the
    // documented DECSTR setting for dec auto wrap, so we do too". They are right to. A soft reset is what
    // a user reaches for to REPAIR a garbled terminal, and no shell ever sends DECSET 7 on its own, so
    // obeying the letter of the spec here hands back a terminal that no longer wraps — more broken than
    // the one they started with. Restored, like TextReflow just above it, rather than cleared.
    setMode(DECMode::AutoWrap, true);

    setMode(AnsiMode::Insert, false); // IRM

    // SRM, set: the terminal does *not* echo what it sends. This is the default every VT terminal ships
    // with -- the host echoes -- and it must be set explicitly, because the mode register starts at all
    // zeroes and a reset SRM means local echo is on.
    setMode(AnsiMode::SendReceive, true);

    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)

    // Reverse wraparound, both forms. A soft reset puts an xterm private mode back to the value the
    // terminal was configured with, and neither is configurable here, so both go off. Leaving them on
    // would let one application's choice outlive it: a backspace at the left margin would keep walking
    // backwards into the line above long after whoever asked for that had gone.
    setMode(DECMode::ReverseWraparound, false);
    setMode(DECMode::ReverseWraparoundExtended, false);

    setTopBottomMargin({}, boxed_cast<LineOffset>(_settings.pageSize.lines) - LineOffset(1));       // DECSTBM
    setLeftRightMargin({}, boxed_cast<ColumnOffset>(_settings.pageSize.columns) - ColumnOffset(1)); // DECRLM

    _currentScreen->cursor().hyperlink = {};

    resetColorPalette();
    clearMacros();
    clearUDKs();
    clearDRCS();
    resetLocator();

    setActiveStatusDisplay(ActiveStatusDisplay::Main);
    setStatusDisplay(StatusDisplayType::None);

    setMode(DECMode::ApplicationKeypad, false); // DECNKM
    setMode(DECMode::AutoRepeat, true);         // DECARM
    setMode(DECMode::BackarrowKey, false);      // DECBKM

    // XTCHECKSUM goes back to what the terminal was configured with, not to zero -- matching xterm,
    // which restores its `checksumExtension` resource here. A test suite that configures the
    // extension up front would otherwise lose it to the first DECSTR it sends.
    _checksumExtension = _settings.checksumExtension;

    // DECSCA is reset by setGraphicsRendition(GraphicsRendition::Reset) above. The character-protection
    // *mode* (DEC/ISO) is separate screen state, so clear it too -- xterm's ReallyReset zeroes
    // protected_mode unconditionally, i.e. on a soft reset as well as a hard one.
    _currentScreen->resetProtection();

    // UPSS goes back to what the terminal was configured with. xterm restores the charsets from
    // ReallyReset() unconditionally -- i.e. on DECSTR as well as RIS -- so a soft reset restores UPSS
    // just as it restores XTCHECKSUM above.
    _userPreferredSupplementalSet = _settings.userPreferredSupplementalSet;

    // TODO: DECNRCM (National replacement character set)
    // TODO: GL, GR (G0, G1, G2, G3)
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

    // xterm returns a DECCOLM 132-column switch to 80 columns on RIS, but only when 80/132 switching was
    // allowed AND the terminal is currently in 132 columns. Crucially this is checked BEFORE the modes
    // are reset -- older xterm cleared the mode first and could then no longer tell it had been in 132
    // columns, which is exactly what esctest RISTests.test_RIS_ResetDECCOLM guards against. 80 is the
    // DECCOLM-off width by definition (DECRESET(DECCOLM) resizes to it), not an arbitrary constant.
    auto const wasIn132Columns =
        isModeEnabled(DECMode::AllowColumns80to132) && isModeEnabled(DECMode::Columns132);

    setScreen(ScreenType::Primary);

    // Ensure that the alternate screen buffer is having the correct size, as well.
    applyPageSizeToMainDisplay(ScreenType::Alternate);

    if (wasIn132Columns)
    {
        // Authoritative, synchronous grid resize (as DECCOLM itself is), then ask the window to follow.
        resizeScreenKeepingCellSize(PageSize { totalPageSize().lines, ColumnCount(80) });
        requestWindowResize(PageSize { pageSize().lines, ColumnCount(80) });
    }

    // RIS leaves VT52 and returns to the configured conformance level. VT52 has no ANSI grammar, so
    // `ESC <` is the only sequence that can leave it: a program that dies in VT52 leaves a terminal that
    // nothing the host sends can recover, and a hard reset -- what the user's Reset action reaches -- must
    // therefore restore the ANSI parser.
    //
    // The parser is left directly rather than through setVT52Mode(false), because that carries the `ESC <`
    // rule of landing at VT100 (VT52 being level-less, it has no level to restore). RIS is a reset to the
    // power-on state, so it restores the level the terminal was configured with, undoing any DECSCL.
    _parser.setVT52Mode(false);
    setOperatingLevel(_factorySettings.terminalId);

    _modes = Modes {};

    // SRM, set: the terminal does *not* echo what it sends -- the host does. The mode register was just
    // cleared to all zeroes, and a *reset* SRM means local echo is on, so leaving it here would echo every
    // keystroke on top of the shell's own echo for the rest of the session. @see flushInput(), softReset().
    setMode(AnsiMode::SendReceive, true);

    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::AutoRepeat, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, _settings.graphemeClustering);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::PageCursorCoupling, true);

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);

    _checksumExtension = _settings.checksumExtension;                       // XTCHECKSUM
    _userPreferredSupplementalSet = _settings.userPreferredSupplementalSet; // DECAUPSS

    // RIS restores the title modes to their default (xterm resets title_modes only on a full reset, not
    // on DECSTR). @see resetTitleModes, TitleModeFeature.
    resetTitleModes();

    // Reset all pages.
    for (auto& page: _pages)
        page->hardReset();
    _cursorPage = PageIndex(0);
    _displayedPage = PageIndex(0);
    _hostWritableStatusLineScreen.hardReset();
    _indicatorStatusScreen.hardReset();

    _imagePool.clear();
    _tabs.clear();

    // A pointer shape is application state like any other, so RIS withdraws it: a program that dies
    // holding a hand cursor must not leave the user with one. The stack keeps its bottom entry -- the
    // terminal's own default -- exactly as popPointerShape does.
    resetPointerShape();

    resetColorPalette();

    // A hard reset (RIS) clears the application-assigned window-frame/tab color (DECAC item 2). The
    // frame color is not part of the color palette, so it must be reset explicitly. (Soft reset
    // deliberately leaves it untouched, matching how window decorations survive DECSTR.) This only
    // withdraws what the application itself assigned: a color the user picked in the GUI is a separate,
    // higher-precedence source that no escape sequence can clear.
    resetWindowFrameColor();

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

    // Reset margins for all pages to defaults.
    _pageMargins.fill(makeDefaultMargin(mainDisplayPageSize));
    primaryScreen().verifyState();

    setStatusDisplay(_factorySettings.statusDisplayType);

    _inputGenerator.reset();
    _pendingLocalEcho.clear();
}

void Terminal::forceRedraw(std::function<void()> const& artificialSleep)
{
    auto const totalPageSize = _settings.pageSize;
    auto const tmpPageSize = PageSize { totalPageSize.lines, totalPageSize.columns + ColumnCount(1) };

    // Read the cell size once, up front: resizeScreen() re-derives it from what it is handed, so
    // asking again in between would return whatever the first call concluded.
    auto const cellSize = cellPixelSize();

    // Each resize carries the pixel size OF THE PAGE IT NAMES. resizeScreen() derives the cell size
    // as pixels/page, so handing the real page's pixels to the one-column-wider page derived a cell
    // width of cellW*columns/(columns+1) and pushed that to the child. A program that reads
    // TIOCGWINSZ on the resulting SIGWINCH -- which is exactly what img2sixel and chafa do -- then
    // sized its image canvas from a cell a pixel too narrow per column, and the second resize below
    // issues no third SIGWINCH to correct one that already read the first.
    resizeScreen(tmpPageSize, cellSize * tmpPageSize);
    if (artificialSleep)
        artificialSleep();
    resizeScreen(totalPageSize, cellSize * totalPageSize);
}

void Terminal::finalizeScreenTransition() noexcept
{
    _screenTransition.active = false;
    _screenTransition.snapshotCells.clear();
    _screenTransition.snapshotCursor.reset();
}

void Terminal::setPage(PageIndex target, bool moveCursorHome)
{
    // Clamp target to valid range.
    auto const clamped = PageIndex(std::clamp(target.value, 0, MaxPageCount - 1));
    if (clamped == _cursorPage)
        return;

    // Cancel any active cursor motion animation on page change.
    _cursorMotion.active = false;

    // Reset smooth scroll state on page change.
    resetSmoothScroll();

    // Capture snapshot of the outgoing screen for crossfade transition.
    if (isModeEnabled(DECMode::PageCursorCoupling)
        && _settings.screenTransitionStyle == ScreenTransitionStyle::Fade)
    {
        if (_screenTransition.active)
            finalizeScreenTransition();

        auto const savedChanges = _changes.load();
        auto const savedScreenDirty = _screenDirty;
        auto const savedFrameID = _lastFrameID.load();

        RenderBuffer snapshotBuffer;
        fillRenderBufferInternal(snapshotBuffer, false);

        _changes.store(savedChanges);
        _screenDirty = savedScreenDirty;
        _lastFrameID.store(savedFrameID);

        _screenTransition.snapshotCells = std::move(snapshotBuffer.cells);
        _screenTransition.snapshotCursor = snapshotBuffer.cursor;
        _screenTransition.startTime = _currentTime;
        _screenTransition.duration = _settings.screenTransitionDuration;
        _screenTransition.active = true;
    }

    _cursorPage = clamped;
    _currentScreen = _pages[clamped.value].get();

    // Update mouse wheel mode based on whether this is the primary page.
    if (clamped == PageIndex(0))
        setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
    else if (isModeEnabled(DECMode::MouseAlternateScroll))
        setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
    else
        setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);

    // When DECPCCM is set, the displayed page follows the cursor page.
    if (isModeEnabled(DECMode::PageCursorCoupling))
        _displayedPage = clamped;

    _currentScreenType = screenTypeFromPage(clamped);

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    if (moveCursorHome)
        _currentScreen->moveCursorTo(LineOffset(0), ColumnOffset(0));

    bufferChanged(_currentScreenType);
}

void Terminal::saveCursorPage()
{
    // Per-screen: the primary and alternate screens keep their own saved cursor page, exactly as they
    // keep their own saved cursor (Screen::_savedCursor). Sharing one slot would let a DECSC on one
    // screen clobber the other's saved page -- and, because the alternate screen is modelled as a page,
    // drag a later DECRC across the primary/alternate boundary, which is DECSET 47/1049's job, not
    // DECSC/DECRC's. xterm likewise keeps saved-cursor state separate per screen. The slot is keyed on
    // the page identity (is this THE alternate page?), not _currentScreenType, so a primary VT420 page
    // reached via PPA/NP/PP still uses the primary slot.
    _savedCursorPage[savedCursorPageSlot()] = _cursorPage;
}

void Terminal::restoreCursorPage()
{
    auto const saved = _savedCursorPage[savedCursorPageSlot()];
    if (saved != _cursorPage)
        setPage(saved, false);
}

void Terminal::setScreen(ScreenType type)
{
    setPage(type == ScreenType::Primary ? PageIndex(0) : AlternateScreenPageIndex, false);
}

void Terminal::applyPageSizeToCurrentBuffer()
{
    applyPageSizeToMainDisplay(screenType());
}

void Terminal::applyPageSizeToMainDisplay(ScreenType screenType)
{
    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();

    // Apply page size to the appropriate page buffer.
    // clang-format off
    switch (screenType)
    {
        case ScreenType::Primary:
            pageAt(_cursorPage).applyPageSizeToMainDisplay(mainDisplayPageSize);
            break;
        case ScreenType::Alternate:
            pageAt(_cursorPage).applyPageSizeToMainDisplay(mainDisplayPageSize);
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
    if (viewport().scrolled() || _viewport.pixelOffset() > 0.0f || _inputHandler.mode() == ViMode::Normal)
        viewport().scrollUp(n);

    if (!_selection)
        return;

    auto const top = -boxed_cast<LineOffset>(primaryScreen().historyLineCount());
    if (_selection->from().line > top && _selection->to().line > top)
        _selection->applyScroll(boxed_cast<LineOffset>(n), primaryScreen().historyLineCount());
    else
        clearSelection();
}
// }}}

void Terminal::setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount)
{
    primaryScreen().grid().setMaxHistoryLineCount(maxHistoryLineCount);
}

LineCount Terminal::maxHistoryLineCount() const noexcept
{
    return primaryScreen().grid().maxHistoryLineCount();
}

void Terminal::setTerminalId(VTType id) noexcept
{
    _terminalId = id;
    _operatingLevel = id;
    _supportedVTSequences.reset(id);
}

void Terminal::setOperatingLevel(VTType level) noexcept
{
    _operatingLevel = level;
    _supportedVTSequences.reset(level);
}

void Terminal::setVT52Mode(bool enable) noexcept
{
    _parser.setVT52Mode(enable);
    if (!enable)
        // Leaving VT52 (ESC <) enters ANSI mode at the base VT100 level, not the level held before
        // entering VT52: VT52 is a pre-ANSI, level-less mode, so there is no level to restore. A real
        // VT500 therefore no longer recognises VT300+ sequences (DECSCL, DECRQSS, S8C1T) after a VT52
        // round-trip -- which is exactly what vttest checks, and what xterm's CASE_VT52_FINISH does.
        setOperatingLevel(VTType::VT100);
}

void Terminal::defineMacro(int id, bool deleteAll, std::string body)
{
    if (deleteAll)
        _macros.clear();

    if (id < 0 || id >= MaxMacroCount)
        return;

    if (body.empty())
        _macros.erase(id);
    else
        _macros[id] = std::move(body);
}

void Terminal::invokeMacro(int id)
{
    auto const it = _macros.find(id);
    if (it == _macros.end())
        return;

    if (_macroRecursionDepth >= MaxMacroRecursionDepth)
        return; // Guard against infinite recursion

    // Queue the macro body for deferred execution.
    // We cannot call parseFragment() re-entrantly during an active parse,
    // so we buffer the body and process it after the current sequence completes.
    _pendingMacroInvocations.push(it->second);
}

void Terminal::processPendingMacros()
{
    while (!_pendingMacroInvocations.empty())
    {
        auto body = std::move(_pendingMacroInvocations.front());
        _pendingMacroInvocations.pop();

        ++_macroRecursionDepth;

        // Write the macro body into the pty buffer so that parseFragment
        // can reference it through the buffer management system.
        auto const chunk = _currentPtyBuffer->writeAtEnd(std::string_view { body });
        _parsingBuffer = _currentPtyBuffer;
        {
            auto const parseGuard = ParseDepthGuard {};
            _parser.parseFragment(chunk);
        }
        _parsingBuffer.reset();

        --_macroRecursionDepth;
    }
}

std::shared_ptr<RasterizedImage> Terminal::createDRCSImage(DRCSGlyph const& glyph, RGBColor foregroundColor)
{
    // Convert monochrome bitmap to RGBA
    auto const pixelCount = static_cast<size_t>(glyph.width * glyph.height);
    auto rgbaData = Image::Data(pixelCount * 4, 0);

    for (size_t i = 0; i < pixelCount && i < glyph.bitmap.size(); ++i)
    {
        if (glyph.bitmap[i])
        {
            rgbaData[i * 4 + 0] = foregroundColor.red;
            rgbaData[i * 4 + 1] = foregroundColor.green;
            rgbaData[i * 4 + 2] = foregroundColor.blue;
            rgbaData[i * 4 + 3] = 0xFF;
        }
        // else: leave as transparent (0,0,0,0)
    }

    auto const pixelSize = ImageSize { Width::cast_from(glyph.width), Height::cast_from(glyph.height) };
    auto const imageRef = _imagePool.create(ImageFormat::RGBA, pixelSize, std::move(rgbaData));
    auto const cellSpan = GridSize { .lines = LineCount(1), .columns = ColumnCount(1) };

    return std::make_shared<RasterizedImage>(
        imageRef, ImageAlignment::TopStart, ImageResize::ResizeToFit, RGBAColor {}, cellSpan, _cellPixelSize);
}

void Terminal::defineDRCS(int fontNumber,
                          int startingCharacter,
                          int eraseControl,
                          int charMatrixWidth,
                          int fontWidth,
                          int /*textOrFullCell*/,
                          int charMatrixHeight,
                          int /*charsetSize*/,
                          std::string_view designator,
                          std::string_view data)
{
    // Erase control: 0 = erase all chars in set, 1 = erase only chars being reloaded, 2 = erase all
    if (eraseControl == 0 || eraseControl == 2)
        _drcsCharsets[fontNumber].glyphs.clear();

    // Store designator → font number mapping for SCS lookup
    if (!designator.empty())
        _drcsDesignatorMap[std::string(designator)] = fontNumber;

    auto const width = [&]() {
        if (charMatrixWidth > 0)
            return charMatrixWidth;
        if (fontWidth > 0)
            return fontWidth;
        return 10;
    }();
    auto const height = (charMatrixHeight > 0) ? charMatrixHeight : 20;

    auto charPos = startingCharacter > 0 ? startingCharacter : 0x21; // Default starting at '!'

    // Parse sixel-like glyph data: each glyph separated by ';', rows within a glyph separated by '/'
    auto glyphStart = size_t { 0 };
    while (glyphStart <= data.size())
    {
        auto const glyphEnd = data.find(';', glyphStart);
        auto const glyphData = data.substr(
            glyphStart, glyphEnd == std::string_view::npos ? std::string_view::npos : glyphEnd - glyphStart);

        if (!glyphData.empty())
        {
            auto glyph = DRCSGlyph { .width = width, .height = height, .bitmap = {} };
            glyph.bitmap.resize(static_cast<size_t>(width * height), 0);

            auto row = 0;
            auto colBase = size_t { 0 };
            for (auto const ch: glyphData)
            {
                if (ch == '/')
                {
                    row += 6; // Each sixel row encodes 6 pixel rows
                    colBase = 0;
                    continue;
                }
                if (ch < 0x3F || ch > 0x7E)
                    continue;

                auto const sixel = static_cast<uint8_t>(ch - 0x3F);
                auto const col = static_cast<int>(colBase);
                for (auto bit = 0; bit < 6 && (row + bit) < height; ++bit)
                {
                    if ((sixel >> bit) & 1)
                    {
                        auto const idx = static_cast<size_t>((row + bit) * width + col);
                        if (idx < glyph.bitmap.size())
                            glyph.bitmap[idx] = 1;
                    }
                }
                ++colBase;
            }

            _drcsCharsets[fontNumber].glyphs[charPos] = std::move(glyph);
        }

        ++charPos;
        if (glyphEnd == std::string_view::npos)
            break;
        glyphStart = glyphEnd + 1;
    }
}

void Terminal::setLocatorMode(int ps, int pu) noexcept
{
    switch (ps)
    {
        case 0: // Locator disabled
            _locatorState.enabled = false;
            _locatorState.oneShot = false;
            break;
        case 1: // Locator enabled — reports on button press/release
            _locatorState.enabled = true;
            _locatorState.oneShot = false;
            break;
        case 2: // One-shot mode — report once then disable
            _locatorState.enabled = true;
            _locatorState.oneShot = true;
            break;
        default: break;
    }

    // Pu: coordinate unit
    switch (pu)
    {
        case 1: _locatorState.coordUnit = LocatorCoordUnit::DevicePixels; break;
        case 0: [[fallthrough]];
        case 2: [[fallthrough]];
        default: _locatorState.coordUnit = LocatorCoordUnit::CharacterCells; break;
    }
}

void Terminal::selectLocatorEvents(std::span<int const> params) noexcept
{
    for (auto const ps: params)
    {
        switch (ps)
        {
            case 0: // Disable all button events (but locator stays enabled)
                _locatorState.reportButtonDown = false;
                _locatorState.reportButtonUp = false;
                break;
            case 1: // Enable button down events
                _locatorState.reportButtonDown = true;
                break;
            case 2: // Disable button down events
                _locatorState.reportButtonDown = false;
                break;
            case 3: // Enable button up events
                _locatorState.reportButtonUp = true;
                break;
            case 4: // Disable button up events
                _locatorState.reportButtonUp = false;
                break;
            default: break;
        }
    }
}

void Terminal::sendLocatorReport(int event, int button, int row, int col)
{
    // DECLRP — Locator Report: CSI Pe ; Pb ; Pr ; Pc ; Pp & w
    // Pp = page number (always 1)
    reply("\033[{};{};{};{};1&w", event, button, row, col);
    flushInput();
}

void Terminal::requestLocatorPosition()
{
    if (!_locatorState.enabled)
    {
        // If locator is not enabled, report locator unavailable
        sendLocatorReport(0, 0, 0, 0);
        return;
    }
    // Report last known mouse position (1-based)
    auto const row = *_currentMousePosition.line + 1;
    auto const col = *_currentMousePosition.column + 1;
    sendLocatorReport(1, 0, row, col);
}

bool Terminal::handleLocatorMouseEvent(int button, bool press, CellLocation pos)
{
    if (!_locatorState.enabled)
        return false;

    if (press && !_locatorState.reportButtonDown)
        return false;
    if (!press && !_locatorState.reportButtonUp)
        return false;

    // Pe: 2 = button down, 3 = button up
    auto const event = press ? 2 : 3;

    // Pb: button encoding (1=left, 2=middle, 4=right; 0 for up with no button info)
    auto const pb = press ? button : 0;

    // Row and column are 1-based
    auto const row = *pos.line + 1;
    auto const col = *pos.column + 1;

    sendLocatorReport(event, pb, row, col);

    if (_locatorState.oneShot)
    {
        _locatorState.enabled = false;
        _locatorState.oneShot = false;
    }

    return true;
}

void Terminal::programUDK(bool clearAll, bool locked, std::string_view data)
{
    if (_udkLocked)
        return;

    if (clearAll)
        _userDefinedKeys.clear();

    // Parse "Ky1/St1;Ky2/St2;..." pairs
    // Each pair: decimal key number, '/', hex-encoded string
    auto pos = size_t { 0 };
    while (pos < data.size())
    {
        // Parse key ID (decimal)
        auto const slashPos = data.find('/', pos);
        if (slashPos == std::string_view::npos)
            break;

        auto keyId = 0;
        auto const keyStr = data.substr(pos, slashPos - pos);
        if (auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), keyId);
            ec != std::errc {})
            break;

        // Parse hex-encoded string
        auto const semiPos = data.find(';', slashPos + 1);
        auto const hexStr =
            data.substr(slashPos + 1,
                        semiPos == std::string_view::npos ? std::string_view::npos : semiPos - slashPos - 1);

        // Decode hex pairs to bytes
        auto decoded = std::string {};
        decoded.reserve(hexStr.size() / 2);
        for (size_t i = 0; i + 1 < hexStr.size(); i += 2)
        {
            auto const hexToByte = [](char ch) -> uint8_t {
                if (ch >= '0' && ch <= '9')
                    return static_cast<uint8_t>(ch - '0');
                if (ch >= 'A' && ch <= 'F')
                    return static_cast<uint8_t>(ch - 'A' + 10);
                if (ch >= 'a' && ch <= 'f')
                    return static_cast<uint8_t>(ch - 'a' + 10);
                return 0;
            };
            decoded.push_back(static_cast<char>((hexToByte(hexStr[i]) << 4) | hexToByte(hexStr[i + 1])));
        }

        _userDefinedKeys[keyId] = std::move(decoded);

        if (semiPos == std::string_view::npos)
            break;
        pos = semiPos + 1;
    }

    if (locked)
        _udkLocked = true;
}

std::optional<std::string> Terminal::udkStringForKey(Key key) const noexcept
{
    // DEC UDK key IDs map to function keys F6-F20.
    // These IDs match the CSI tilde parameter numbers.
    static constexpr auto KeyMapping = std::array<std::pair<Key, int>, 15> { {
        { Key::F6, 17 },
        { Key::F7, 18 },
        { Key::F8, 19 },
        { Key::F9, 20 },
        { Key::F10, 21 },
        { Key::F11, 23 },
        { Key::F12, 24 },
        { Key::F13, 25 },
        { Key::F14, 26 },
        { Key::F15, 28 },
        { Key::F16, 29 },
        { Key::F17, 31 },
        { Key::F18, 32 },
        { Key::F19, 33 },
        { Key::F20, 34 },
    } };

    auto const it = std::ranges::find_if(KeyMapping, [key](auto const& pair) { return pair.first == key; });
    if (it != KeyMapping.end())
        return udkString(it->second);
    return std::nullopt;
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
            _currentScreen = _pages[_cursorPage.value].get();
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

    return pageAt(_cursorPage).grid().cellEmptyOrContainsOneOf(position, wordDelimiters);
}

std::tuple<std::u32string, CellLocationRange> Terminal::extractWordUnderCursor(
    CellLocation position) const noexcept
{
    auto const& screen = pageAt(_cursorPage);
    auto const range = screen.grid().wordRangeUnderCursor(position, u32string_view(_settings.wordDelimiters));
    return { screen.grid().extractText(range), range };
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

CellLocation Terminal::clampDragWithinMulticellBlock(CellLocation anchor, CellLocation pointer) const noexcept
{
    // A scale>1 block is several screen rows tall but reads as ONE line of text. Dragging along such
    // a line, the pointer inevitably strays into the row below the one it started on -- and without
    // this, that one-row wobble turns a single-line selection into a two-line one, which sweeps the
    // first line to its right margin and swallows everything after the sized run.
    //
    // So while both ends sit in the SAME block-row of blocks of the same shape, the drag is treated
    // as horizontal: the pointer's row is snapped back to the anchor's. It releases as soon as the
    // pointer leaves those blocks, which is how a genuine multi-line selection is still made.
    //
    // kitty solves it the same way, in clamp_selection_input_to_multicell().
    if (anchor.line == pointer.line)
        return pointer;

    auto const anchorBlock = currentScreen().multicellBlockAt(anchor);
    if (!anchorBlock || anchorBlock->rows < 2)
        return pointer;

    auto const pointerBlock = currentScreen().multicellBlockAt(pointer);
    if (!pointerBlock || pointerBlock->rows != anchorBlock->rows
        || pointerBlock->origin.line != anchorBlock->origin.line)
        return pointer;

    return CellLocation { .line = anchor.line, .column = pointer.column };
}

bool Terminal::isSelected(Screen const& screen, CellLocation coord) const noexcept
{
    if (!_selection || _selection->state() == Selection::State::Waiting)
        return false;

    if (_selection->contains(coord))
        return true;

    // The cell itself is outside the selection -- but if it belongs to a block whose other cells are
    // inside, it is selected too. kitty expands its selection mask over a block's whole rectangle
    // (apply_selection / xrange_for_iteration_with_multicells) for the same reason.
    //
    // Reached only while a selection is live, and only for cells the selection did not already
    // cover, so ordinary text pays a grid lookup that a trivial line answers immediately.
    // Resolved against the screen the caller is working on: the render path feeds coordinates from
    // the DISPLAYED page, which with page-cursor coupling reset is not the current one.
    auto const block = screen.multicellBlockAt(coord);
    if (!block)
        return false;

    for (auto const row: std::views::iota(0, block->rows))
        for (auto const column: std::views::iota(0, block->columns))
            if (_selection->contains(
                    CellLocation { .line = block->origin.line + LineOffset::cast_from(row),
                                   .column = block->origin.column + ColumnOffset::cast_from(column) }))
                return true;

    return false;
}

bool Terminal::isSelected(LineOffset line) const noexcept
{
    if (!_selection || _selection->state() == Selection::State::Waiting)
        return false;

    if (_selection->containsLine(line))
        return true;

    // A block whose head sits on a selected line above reaches down into this one, and the whole
    // block is selected. Saying "no" here would send this line down the trivial fast path, which
    // renders it uniformly and never consults the per-cell test that knows about the block -- so the
    // highlight would stop at the block's first row.
    //
    // A block spans at most text_sizing::MaxScale lines, which bounds the look-back. Answering "yes"
    // for a line that turns out to hold no block only costs it the per-cell path for one frame.
    for (auto const above: std::views::iota(1, static_cast<int>(text_sizing::MaxScale)))
        if (_selection->containsLine(line - LineOffset::cast_from(above)))
            return true;

    return false;
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

bool Terminal::isHighlighted(LineOffset line) const noexcept // NOLINT(bugprone-exception-escape)
{
    // A line intersects the highlight range when it falls between the range's endpoints, whichever
    // way round they were recorded. Linear and rectangular highlights share the same line span
    // (the rectangle only additionally constrains columns, which do not matter for a line test).
    return _highlightRange.has_value()
           && std::visit(
               [line](auto&& highlightRange) {
                   auto const lo = std::min(highlightRange.from.line, highlightRange.to.line);
                   auto const hi = std::max(highlightRange.from.line, highlightRange.to.line);
                   return lo <= line && line <= hi;
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

void TraceHandler::processAPC(std::string_view body)
{
    // Queued like everything else, and for the same reason: this handler's whole job is to preserve
    // the ORDER in which the application sent things while letting the user step through them.
    // Forwarding an APC straight to the display let it overtake every sequence still queued ahead of
    // it -- a kitty image placed against the cursor position a queued CUP had not moved yet, so
    // tracing a graphics problem showed behaviour that never happens outside trace mode.
    _pendingSequences.emplace_back(ApplicationProgramCommand { .body = std::string(body) });
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
    else if (auto const* apc = std::get_if<ApplicationProgramCommand>(&pendingSequence))
    {
        std::cout << std::format("\tAPC {:<16} ; {} bytes\n", apc->body.substr(0, 16), apc->body.size());
        _terminal->activeDisplay().processAPC(apc->body);
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
        case DECMode::NoClearScreenOnColumnChange: return "NoClearScreenOnColumnChange";
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
        case DECMode::OptionalAltScreen: return "OptionalAltScreen";
        case DECMode::MoreFix: return "MoreFix";
        case DECMode::PageCursorCoupling: return "PageCursorCoupling";
        case DECMode::ApplicationKeypad: return "ApplicationKeypad";
        case DECMode::AutoRepeat: return "AutoRepeat";
        case DECMode::BackarrowKey: return "BackarrowKey";
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
        case DECMode::InBandWindowResize: return "InBandWindowResize";
        case DECMode::PasteMimeNotifications: return "PasteMimeNotifications";
        case DECMode::SemanticBlockProtocol: return "SemanticBlockProtocol";
        case DECMode::PrintFormFeed: return "PrintFormFeed";
        case DECMode::HebrewKeyboardMapping: return "HebrewKeyboardMapping";
        case DECMode::NationalReplacementCharacterSet: return "NationalReplacementCharacterSet";
        case DECMode::HorizontalCursorCoupling: return "HorizontalCursorCoupling";
        case DECMode::RightToLeftMode: return "RightToLeftMode";
        case DECMode::HebrewEncodingMode: return "HebrewEncodingMode";
        case DECMode::GreekKeyboardMapping: return "GreekKeyboardMapping";
        case DECMode::VerticalCursorCoupling: return "VerticalCursorCoupling";
        case DECMode::KeyboardUsageMode: return "KeyboardUsageMode";
        case DECMode::TransmitRateLimiting: return "TransmitRateLimiting";
        case DECMode::KeyPositionMode: return "KeyPositionMode";
        case DECMode::RightToLeftCopyMode: return "RightToLeftCopyMode";
        case DECMode::CRTSaveMode: return "CRTSaveMode";
        case DECMode::AutoResizeMode: return "AutoResizeMode";
        case DECMode::ModemControlMode: return "ModemControlMode";
        case DECMode::AutoAnswerbackMode: return "AutoAnswerbackMode";
        case DECMode::ConcealAnswerbackMode: return "ConcealAnswerbackMode";
        case DECMode::NullMode: return "NullMode";
        case DECMode::HalfDuplexMode: return "HalfDuplexMode";
        case DECMode::SecondaryKeyboardLanguageMode: return "SecondaryKeyboardLanguageMode";
        case DECMode::OverscanMode: return "OverscanMode";
        case DECMode::ReverseWraparound: return "ReverseWraparound";
        case DECMode::ReverseWraparoundExtended: return "ReverseWraparoundExtended";
        case DECMode::Win32InputMode: return "Win32InputMode";
        case DECMode::DECModeCount: break;
    }
    return std::format("({})", static_cast<unsigned>(mode));
}

} // namespace vtbackend
