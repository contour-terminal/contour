// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>

#include <vtbackend/Logging.hpp>
#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/grid/GraphemeClusters.hpp>
#include <vtbackend/input/HeldModifiers.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/PathResolution.hpp>
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>
#include <vtbackend/screen/RenderBufferBuilder.hpp>
#include <vtbackend/shell/CommandBlockPaths.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>
#include <vtbackend/shell/PromptRegion.hpp>
#include <vtbackend/vt/ControlCode.hpp>
#include <vtbackend/vt/Functions.hpp>
#include <vtbackend/vt/SequenceBuilder.hpp>

#include <vtparser/Parser.hpp>
#include <vtparser/Utf8Decoding.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Assert.hpp>
#include <core/Base64.hpp>
#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/UserPaths.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <gsl/pointers>

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <tracy/Tracy.hpp>

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
    /// @param info The window's tabs, as the host reports them.
    /// @return The same, as the status-line extensions are told them.
    [[nodiscard]] StatusLineEvents::TabsChanged statusLineTabsOf(TabsInfo const& info)
    {
        auto event = StatusLineEvents::TabsChanged {};
        event.tabs.reserve(info.tabs.size());
        for (auto const index: std::views::iota(size_t { 0 }, info.tabs.size()))
            event.tabs.push_back(StatusLineTab { .title = info.tabs[index].name.value_or(std::string {}),
                                                 // activeTabPosition counts from one.
                                                 .activity = index + 1 == info.activeTabPosition
                                                                 ? StatusLineTabActivity::Active
                                                                 : StatusLineTabActivity::Inactive });
        return event;
    }
} // namespace

namespace
{
    /// Non-zero while this thread is inside VTParser::parseFragment().
    ///
    /// The parser is a state machine that is not safe to re-enter. A sequence handler that replies
    /// reaches Terminal::flushInput() (Screen::reportColorPaletteUpdate(), Screen::replyGipStatus(),
    /// ... all reply and flush), and with SRM reset a flush echoes -- which parses. A flush sends nothing
    /// while the state lock is held, but a parse lets go of the lock mid-batch for a smooth-scrolling
    /// pause, and a flush there would parse half-way through the sequence being dispatched, clobbering the
    /// state machine. xterm guards the identical hazard
    /// with ParseState::check_recur and defers the bytes instead -- "Defer parsing when parser is
    /// already running as the parser is not safe to reenter" (xterm-406, charproc.c, doparsing()).
    ///
    /// Thread-local rather than a member because the question is "is *this thread* parsing": a GUI
    /// thread echoing a keystroke while the parser thread happens to be parsing must echo, not defer.
    ///
    /// @see Terminal::echoLocally(), Terminal::processPendingLocalEcho().
    thread_local int tlParseDepth = 0;

    /// How long the PTY's reader waits before it writes again what the PTY had no room for. @see
    /// readFromPty()
    constexpr auto PendingInputRetry = std::chrono::milliseconds { 10 };

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

    /// Runs @p step, and logs what it throws instead of letting it escape: for what the state lock's release
    /// sends and asks, from a lock guard's destructor.
    /// @param what What @p step is, for the log.
    /// @param step What to run.
    template <typename Step>
    void runContained(std::string_view what, Step const& step) noexcept
    {
        try
        {
            step();
        }
        catch (std::exception const& e)
        {
            errorLog()("Releasing the terminal lock: {} failed: {}", what, e.what());
        }
        catch (...)
        {
            errorLog()("Releasing the terminal lock: {} failed.", what);
        }
    }

    /// The UDK key number DECUDK programs a string for @p key under: the CSI ~ parameter it sends. F6 to F20
    /// have one; no other key does.
    /// @param key The key pressed.
    /// @return Its key number, or nullopt.
    [[nodiscard]] std::optional<int> userDefinedKeyNumberOf(Key key) noexcept
    {
        static constexpr auto KeyNumbers = std::array<std::pair<Key, int>, 15> { {
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

        auto const it = std::ranges::find(KeyNumbers, key, &std::pair<Key, int>::first);
        if (it == KeyNumbers.end())
            return std::nullopt;
        return it->second;
    }

    /// Whether @p path is a single word with no directory part: what prose is made of, too. A lone `~`
    /// or `~10` is one as well; only a leading `~/` names the home directory.
    [[nodiscard]] bool isBarePathName(std::string_view path) noexcept
    {
        return path.find_first_of("/\\") == std::string_view::npos;
    }

    constexpr size_t MaxColorPaletteSaveStackSize = 10;

    void trimSpaceRight(string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    }

#ifdef CONTOUR_PERF_STATS
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

    /// The primary screen's rows as a block action walks them: each attributed to the block
    /// Terminal::commandBlockAt() names for it, so the action and the gutter never disagree about a row.
    class AttributedBlockRows final: public CommandBlockRowSource
    {
      public:
        explicit AttributedBlockRows(Terminal const& terminal) noexcept:
            _terminal { terminal }, _grid { terminal.primaryScreen().grid() }
        {
        }

        [[nodiscard]] LineOffset topRow() const override { return _grid.addressableTop(); }

        [[nodiscard]] LineOffset bottomRow() const override
        {
            return boxed_cast<LineOffset>(_grid.pageSize().lines) - LineOffset(1);
        }

        [[nodiscard]] CommandBlockId blockIdAt(LineOffset row) const override
        {
            auto const* const record = _terminal.commandBlockAt(row);
            return record ? record->id : CommandBlockId {};
        }

        [[nodiscard]] LineFlags flagsAt(LineOffset row) const override { return _grid.lineAt(row).flags(); }

        [[nodiscard]] ColumnOffset commandEndColumnAt(LineOffset row) const override
        {
            return _grid.lineAt(row).commandEndOffset();
        }

      private:
        Terminal const& _terminal;
        Grid const& _grid;
    };

} // namespace
// }}}

Terminal::Terminal(Events& eventListener,
                   core::Environment const& env,
                   core::platform::FileSystem const& fileSystem,
                   std::unique_ptr<vtpty::Pty> pty,
                   Settings factorySettings,
                   optional<StatusLineCommandServices> commandServices,
                   TerminalClocks clocks,
                   SemanticBlockTokenSource sessionTokens,
                   std::optional<LinkDetectionServices> linkServices,
                   std::optional<StatusLineExtensionServices> extensionServices,
                   std::optional<GitHeadServices> gitServices):
    _eventListener { eventListener },
    // HOME, or Windows' USERPROFILE, when the composition root names none.
    _homeDirectory { !factorySettings.homeDirectory.empty()
                         ? factorySettings.homeDirectory
                         : core::platform::homeDirectory(env)
                               .transform(
                                   [](std::filesystem::path const& home) { return core::pathToUtf8(home); })
                               .value_or(std::string {}) },
    _fileSystem { fileSystem },
    _linkServices { std::move(linkServices) },
    // Read here rather than per reply: what it names is how this terminal was launched, and a
    // configuration value re-read on every DA/DSR/CPR is a lookup on a path that answers thousands
    // of times a second.
    _syncPtyOutput { env.get("CONTOUR_SYNC_PTY_OUTPUT")
                         .transform([](std::string const& value) { return !value.starts_with('0'); })
                         .value_or(false) },
    _factorySettings { std::move(factorySettings) },
    _settings { _factorySettings },
    _currentTime { clocks.steady.now() },
    _clocks { clocks },
    _sessionEpoch { chrono::floor<chrono::seconds>(clocks.wall.now()) },
    _ptyBufferPool { core::nextPowerOfTwo(_settings.ptyBufferObjectSize) },
    _currentPtyBuffer { _ptyBufferPool.allocateBufferObject() },
    _ptyReadBufferSize { core::nextPowerOfTwo(_settings.ptyReadBufferSize) },
    _pty { std::move(pty) },
    _clickCounter { clocks.steady, ClickCounter::DefaultInterval },
    _cursorBlink { CursorBlink { .since = _currentTime.load(), .phase = CursorBlinkPhase::Shown }.packed() },
    _hostWritableStatusLineScreen { *this,
                                    &_hostWritableScreenMargin,
                                    PageSize { StatusLineHeight, _settings.pageSize.columns },
                                    false,
                                    LineCount(0),
                                    "host-writable-status-line" },
    _indicatorStatusScreen { *this,
                             &_indicatorScreenMargin,
                             PageSize { StatusLineHeight, _settings.pageSize.columns },
                             false,
                             LineCount(0),
                             "indicator-status-line" },
    _currentScreen { &_hostWritableStatusLineScreen }, // temporary; reassigned after _pages is populated
    _viewport { *this, std::bind(&Terminal::onViewportChanged, this) },
    _indicatorStatusLineDefinition { parseStatusLineDefinition(_settings.indicatorStatusLine.left,
                                                               _settings.indicatorStatusLine.middle,
                                                               _settings.indicatorStatusLine.right) },
    _selectionHelper { this },
    _extendedSelectionHelper { this },
    _customSelectionHelper { this },
    _refreshInterval { RefreshInterval { _settings.refreshRate } },
    _traceHandler { *this },
    _cellPixelSize {},
    _defaultColorPalette { _settings.colorPalette },
    _colorPalette { _settings.colorPalette },
    _focused { _settings.focused },
    _terminalId { _settings.terminalId },
    _operatingLevel { _settings.terminalId },
    _mainDisplayMargin { makeDefaultMargin(_settings.pageSize) },
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
    _parser { std::ref(_sequenceBuilder), _settings.parserBulkScan },
    _viCommands { *this },
    _inputHandler { _viCommands, ViMode::Insert },
    _semanticBlockTracker { std::move(sessionTokens) },
    _extensions { extensionServices ? &extensionServices->extensions : nullptr },
    _publishSink { extensionServices ? &extensionServices->published : nullptr },
    _extensionEvents { _extensions },
    _maxRememberedUserVars { extensionServices ? extensionServices->maxUserVars : 0 },
    _takeOverName { _extensions ? _extensions->takeOverName() : std::nullopt },
    _statusLineCommands { commandServices ? std::make_unique<StatusLineCommandCache>(
                                                *commandServices,
                                                [this](StatusLineCommandOutput output) {
                                                    onStatusLineCommandCompleted(output);
                                                })
                                          : nullptr },
    // The same contract as onStatusLineCommandCompleted(): on the executor, without the state lock.
    _gitHead { gitServices
                   ? std::make_unique<GitHeadCache>(*gitServices,
                                                    [this] {
                                                        breakLoopAndRefreshRenderBuffer();
                                                        _eventListener.indicatorStatusLineItemChanged();
                                                    })
                   : nullptr },
    // Last: its redraw request reaches what every member above holds.
    _linkDetector { makeLinkDetector() }
{
    // Through setTerminalId, so the three pieces of state derived from the identity are derived in
    // ONE place. Seeding _terminalId/_operatingLevel in the member list and stopping there left the
    // conformance GATE and the sequence TABLE disagreeing for any terminal nobody reconfigures
    // afterwards — a daemon-hosted VT220 session refused the VT420 modes and answered DA1 as a
    // VT220, yet still executed DECFRA/DECCRA off the full VT525 table.
    setTerminalId(_settings.terminalId);

    // A surface has no size until it is told one, and renders nothing before.
    resizeStatusLineExtensions();

    // Populate the 16-page array: page 0 (primary) gets scrollback/reflow, pages 1-15 do not.
    _pages.reserve(MaxPageCount);
    _pages.push_back(std::make_unique<Screen>(*this,
                                              &_mainDisplayMargin,
                                              _settings.pageSize,
                                              _settings.primaryScreen.allowReflowOnResize,
                                              _settings.historyLimits,
                                              "page-1"));
    for (auto const i: std::views::iota(1, MaxPageCount))
        _pages.push_back(std::make_unique<Screen>(*this,
                                                  &_mainDisplayMargin,
                                                  _settings.pageSize,
                                                  false,
                                                  LineCount(0),
                                                  std::format("page-{}", i + 1)));
    _currentScreen = _pages[0].get();

    _savedColorPalettes.reserve(MaxColorPaletteSaveStackSize);

    changeInputEncoding([&](InputGenerator& input) {
        input.setMouseWheelScrollMultiplier(
            static_cast<unsigned>(unbox(_settings.mouseWheelScrollMultiplier)));
    });

    // TODO(should be this instead?): hardReset();

    // SRM, set: the terminal does *not* echo what it sends -- the host does. This is the default every
    // VT terminal ships with, and it has to be said out loud: the mode register starts at all zeroes,
    // and a *reset* SRM means local echo is on. @see flushInput().
    setMode(AnsiMode::SendReceive, true);

    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::AutoRepeat, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    setMode(DECMode::NoSixelScrolling, _settings.sixelScrolling == SixelScrolling::Disabled);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, _settings.graphemeClustering);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::PageCursorCoupling, true);
    setMode(DECMode::LeftRightMargin, false);
    // Alternate scroll on, as Windows Terminal and foot power up.
    setMode(DECMode::MouseAlternateScroll, true);

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);

    // A double and a triple click walk words by the delimiters the settings carry when they walk, so a
    // terminal selects words from the start, and setWordDelimiters() has nothing to rewire.
    _selectionHelper.wordDelimited = [this](CellLocation const& pos) {
        return wordDelimited(pos, _settings.wordDelimiters);
    };
    _extendedSelectionHelper.wordDelimited = [this](CellLocation const& pos) {
        return wordDelimited(pos, _settings.extendedWordDelimiters);
    };

    // Last, once nothing in this constructor can throw any more: a terminal that failed to construct runs
    // no destructor, so it must never have been attached, or the sink would keep a dangling target.
    if (_publishSink)
        _publishSink->attach(*this);
}

void Terminal::onViewportChanged()
{
    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = _viewport.clampCellLocation(_viCommands.cursorPosition);

    // The clamp puts the cursor on a row the viewport DRAWS; this puts it on a line no fold hides. Both
    // are needed and this order suffices: the clamp's own bounds are drawn rows, and a hidden line
    // between them belongs to a run that cannot reach past either of them -- they are visible, and no
    // run contains a visible line -- so snapping out of it stays inside the viewport.
    snapViCursorOutOfFolds();

    if (_hintModeHandler.isActive())
        refreshHints();

    extendSelectionAfterScroll();

    _eventListener.onScrollOffsetChanged(_viewport.scrollOffset());
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::extendSelectionAfterScroll()
{
    if (_isAutoScrolling || _leftButtonPress != LeftButtonPress::Selecting || !selectionAvailable()
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
    ZoneScoped;
    auto timeout =
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
        std::optional { passiveReadTimeout() };
#else
        std::optional<std::chrono::milliseconds> { std::nullopt };
#endif

    // The reader writes what other threads queue, so input the PTY had no room for -- the application reads
    // too slowly -- is written again after a short wait, rather than after the next output: an application
    // waiting for that input may write none.
    if (_settings.inputWriter == InputWriter::ReaderThread && hasInput())
        timeout = std::min(timeout.value_or(PendingInputRetry), PendingInputRetry);

    // Request a new Buffer Object if the current one cannot sufficiently
    // store a single text line. The width comes from the lock-free mirror: this runs before the parser
    // thread takes the state, while the GUI thread may be resizing the page under it.
    if (_currentPtyBuffer->bytesAvailable() < unbox<size_t>(totalPageSize().columns))
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

std::vector<vtpty::Pty::ReadResult> Terminal::readAhead(crispy::BufferObject<char>& buffer,
                                                        vtpty::Pty::ReadResult const& first)
{
    ZoneScoped;
    auto batch = std::vector { first };
    auto const* const batchBegin = first.data.data();
    auto const* claimedEnd = batchBegin + first.data.size();
    auto const claim = [&]() {
        auto const _ = std::scoped_lock { buffer };
        buffer.advanceHotEndUntil(claimedEnd);
    };

    // The batch's size while it is contiguous, which it is until a read is overtaken and ends the loop.
    auto const batchSize = [&]() {
        return static_cast<size_t>(claimedEnd - batchBegin);
    };

    claim();
    while (batchSize() < _ptyReadBufferSize && buffer.bytesAvailable() > 0)
    {
        auto const more =
            _pty->read(buffer, std::chrono::milliseconds::zero(), _ptyReadBufferSize - batchSize());
        if (!more || more->data.empty())
            break;
        // A read lands at the hot end, which is right after the batch unless another writer appended
        // there meanwhile and moved it on.
        auto const overtaken = more->data.data() != claimedEnd;
        if (auto& last = batch.back(); !overtaken && last.fromStdoutFastPipe == more->fromStdoutFastPipe)
            last.data = string_view { last.data.data(), last.data.size() + more->data.size() };
        else
            batch.push_back(*more);
        claimedEnd = more->data.data() + more->data.size();
        claim();
        if (overtaken)
            break;
    }
    return batch;
}

void Terminal::releaseReadAhead(crispy::BufferObject<char>& buffer,
                                std::span<vtpty::Pty::ReadResult const> batch) noexcept
{
    auto const* const batchBegin = batch.front().data.data();
    auto const _ = std::scoped_lock { buffer };
    buffer.rewindHotEndTo(batchBegin);
}

void Terminal::setExecutionMode(ExecutionMode mode)
{
    auto _ = std::unique_lock(_breakMutex);
    _executionMode = mode;
    _breakCondition.notify_one();
    _pty->wakeupReader();
}

uint8_t Terminal::bindingModes() const noexcept
{
    auto flags = _publishedBindingModes.load(std::memory_order_relaxed);

    if (_inputHandler.mode() == ViMode::Insert)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Insert);

    if (_executionMode.load(std::memory_order_relaxed) != ExecutionMode::Normal)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Trace);

    return flags;
}

void Terminal::publishBindingModes() noexcept
{
    using Flag = MatchModes::Flag;
    auto const flagIf = [](bool holds, Flag flag) {
        return holds ? static_cast<uint8_t>(flag) : uint8_t { 0 };
    };
    auto const flags = static_cast<uint8_t>(
        flagIf(displayedScreenType() == ScreenType::Alternate, Flag::AlternateScreen)
        | flagIf(applicationCursorKeys(), Flag::AppCursor) | flagIf(applicationKeypad(), Flag::AppKeypad)
        | flagIf(selectionAvailable(), Flag::Select) | flagIf(!_search.pattern.empty(), Flag::Search));
    _publishedBindingModes.store(flags, std::memory_order_relaxed);
}

template <typename Mutex, typename Parse>
void Terminal::runParseBatch(std::unique_lock<Mutex> lock, Parse const& parse)
{
    auto batchedRendering = false;
    {
        auto const heldLock = std::move(lock);
        auto const batchClock = sampleBatchClock();
        parse();

        // What the parse deferred runs now, inside this batch: no later batch is promised to drain it.
        processPendingLocalEcho(); // local echo (SRM) a reply deferred

        // Under the lock, as every other thread moves the view: the view follows what was just written.
        followOutput();

        // Sampled inside the lock: _modes is written by the GUI thread too (a Vi-mode toggle sets
        // DECMode::VisibleCursor under this lock), and screenUpdated() below must stay OUTSIDE it
        // because it calls back into the GUI.
        batchedRendering = isRenderingSuppressed();
    }

    if (!batchedRendering)
        screenUpdated();
}

bool Terminal::processInputOnce()
{
    ZoneScoped;
    _readerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    // clang-format off
    switch (_executionMode.load())
    {
        case ExecutionMode::BreakAtEmptyQueue:
            _executionMode = ExecutionMode::Waiting;
            [[fallthrough]];
        case ExecutionMode::Normal:
            if (!_traceHandler.pendingSequences().empty())
            {
                auto const _ = std::lock_guard { _ownStateLock };
                auto const batchClock = sampleBatchClock(); // the queued sequences take effect now
                _traceHandler.flushAllPending();
                return true;
            }
            break;
        case ExecutionMode::Waiting:
        {
            // Named, not deduced: std::condition_variable waits only on a std::unique_lock<std::mutex>.
            auto lock = std::unique_lock<std::mutex>(_breakMutex);
            _breakCondition.wait(lock, [this]() { return _executionMode != ExecutionMode::Waiting; });
            return true;
        }
        case ExecutionMode::SingleStep:
            if (!_traceHandler.pendingSequences().empty())
            {
                auto const _ = std::lock_guard { _ownStateLock };
                auto const batchClock = sampleBatchClock();
                _executionMode = ExecutionMode::Waiting;
                _traceHandler.flushOne();
                return true;
            }
            break;
    }
    // clang-format on

    // A backend that fails without setting errno must read as gone, not as whatever an earlier call on
    // this thread left behind: a stale EAGAIN would have this loop read again forever.
    errno = 0;
    auto const ptyReadResult = readFromPty();

    if (!ptyReadResult)
    {
        terminalLog()("PTY read failed. {}", std::generic_category().message(errno));
        if (errno == EINTR || errno == EAGAIN)
        {
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
            // Nothing arrived within the wait, which ends when the refresh interval lets a pending fill run.
            if (isPassiveFillPending())
                refreshPassively();
#endif
            return true;
        }

        _pty->close();
        return false;
    }

    if (ptyReadResult->readResult.data.empty())
    {
        terminalLog()("PTY read returned with zero bytes. Closing PTY.");
        _pty->close();
        return false;
    }

    auto readAheadBatch = std::vector<vtpty::Pty::ReadResult> {};
    auto batch = std::span { &ptyReadResult->readResult, 1 };
    auto lock = std::unique_lock { _ownStateLock, std::try_to_lock };
    if (!lock.owns_lock())
    {
        // Another thread -- the render thread, building a frame -- holds the state. Read what the
        // child writes meanwhile instead of waiting idle, so it is not left blocked on a full PTY and
        // the parse below covers all of it in this one hold of the lock.
        readAheadBatch = readAhead(*ptyReadResult->buffer, ptyReadResult->readResult);
        batch = readAheadBatch;
        lock.lock();
        releaseReadAhead(*ptyReadResult->buffer, batch);
    }
    TracyPlot("pty.read.bytes",
              static_cast<int64_t>(std::accumulate(
                  batch.begin(), batch.end(), size_t { 0 }, [](size_t total, auto const& piece) {
                      return total + piece.data.size();
                  })));

    runParseBatch(std::move(lock), [&] {
        // Parse against the buffer readFromPty() read into: Screen's bulk text path moves this buffer's hot
        // end past the text it parsed. writeToScreen() parses out of a buffer of its own.
        _parsingBuffer = ptyReadResult->buffer;
        for (auto const& piece: batch)
        {
            _usingStdoutFastPipe = piece.fromStdoutFastPipe;
            parseExpandingMacros(piece.data);
        }
        _parsingBuffer = nullptr;
    });

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    refreshPassively();
#endif

    return true;
}

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
bool Terminal::isPassiveFillPending() const noexcept
{
    return _renderBuffer.state != RenderBufferState::WaitingForRefresh || _screenDirty;
}

std::chrono::milliseconds Terminal::passiveReadTimeout() const noexcept
{
    auto const interval = _refreshInterval.get().value;

    // A frame that waits for the display to let go of the front buffer is swapped in when the display wakes
    // the reader (@see onFrontBufferReleased); new output ends the wait too. Polling meanwhile would only
    // fail the swap again, for as long as a paint lasts.
    if (!isPassiveFillPending() || _renderBuffer.state == RenderBufferState::TrySwapBuffers)
        return interval;

    // Measured at the time refreshPassively() measures the throttle at, and rounded up: a wait that ended
    // short of the interval would find the fill still held back.
    auto const now = std::max(currentTime(), _clocks.steady.now());
    auto const elapsed = now - _renderBuffer.lastUpdate.load(std::memory_order_relaxed);
    return std::max(chrono::milliseconds(0), chrono::ceil<chrono::milliseconds>(interval - elapsed));
}

void Terminal::refreshPassively()
{
    // Forward only: the display's tick() may have set it later meanwhile.
    auto const now = _clocks.steady.now();
    auto clock = _currentTime.load(std::memory_order_relaxed);
    while (clock < now && !_currentTime.compare_exchange_weak(clock, now, std::memory_order_relaxed))
    {
    }
    ensureFreshRenderBuffer();
}

void Terminal::onFrontBufferReleased()
{
    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
        _pty->wakeupReader();
}
#endif

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

void Terminal::onStatusLineCommandCompleted(StatusLineCommandOutput output)
{
    // Runs on a core::async executor thread with the cache's mutex held and Terminal::_stateMutex NOT
    // held, so it must stay off terminal state:
    //  - Changed: breakLoopAndRefreshRenderBuffer(). _changes and _renderBuffer.state are atomics, and
    //    Pty::wakeupReader() is the cross-thread wake-up (the GUI thread calls it too).
    //  - Unchanged: renderBufferUpdated() only, so the display re-arms its wakeup from nextRender()
    //    without dirtying the buffer. This is only as thread-safe as the Events implementation:
    //    see Events::renderBufferUpdated().
    switch (output)
    {
        case StatusLineCommandOutput::Unchanged: _eventListener.renderBufferUpdated(); break;
        case StatusLineCommandOutput::Changed: breakLoopAndRefreshRenderBuffer(); break;
    }
}

std::string Terminal::statusLineCommandOutput(StatusLineDefinitions::Command const& item) const
{
    // The cache is internally synchronised, so the const accessor may call it through the pointer.
    return _statusLineCommands ? _statusLineCommands->output(item.command, item.interval, currentTime())
                               : std::string {};
}

Terminal::~Terminal()
{
    // Before any member goes: a delivery in flight reaches _eventListener and _pty.
    if (_publishSink)
        _publishSink->revoke();
}

void Terminal::statusLineExtensionsPublished() noexcept
{
    // On an extension worker, without the state lock: the same contract as onStatusLineCommandCompleted().
    // _extensions is fixed at construction, and subscriptions() is a lock-free read on the host side.
    if (!_extensions) [[unlikely]]
        return;
    // What they newly follow is replayed by the next frame, under the lock: this worker reads no state.
    if (auto const newly = _extensionEvents.refresh(); newly.any())
        _extensionReplay.fetch_or(newly.value(), std::memory_order_release);
    onStatusLineCommandCompleted(StatusLineCommandOutput::Changed);
    _eventListener.extensionFramePublished();
}

void Terminal::refreshIndicatorStatusLine()
{
    drainStatusLineReplay();
    // Whether this terminal shows it or not: a daemon's terminals show no indicator, and their clients do.
    if (_activeStatusDisplay != ActiveStatusDisplay::IndicatorStatusLine
        && indicatorStatusLineDrivenByExtensions())
        updateIndicatorStatusLine();
}

bool Terminal::indicatorStatusLineDrivenByExtensions() const noexcept
{
    if (!_extensions)
        return false;
    if (_takeOverName)
        return true;
    return containsItem<StatusLineDefinitions::Extension>(_indicatorStatusLineDefinition);
}

void Terminal::setMirroredIndicatorStatusLine(std::optional<Line> row)
{
    _mirroredIndicatorRow = std::move(row);
}

void Terminal::statusLineExtensionsReplaced()
{
    if (!_extensions)
        return;
    _takeOverName = _extensions->takeOverName();
    _takeOverSource.reset();
    _takeOverRow = Line {};
    // Told again even at an unchanged size: the new extensions were never told it.
    _extensionSurfaceSize = {};
    resizeStatusLineExtensions();
    // Nor the font, which every one of them takes its surface's cell size from, followed or not: told here,
    // so not replayed below as well.
    if (_statusLineFont != StatusLineFontMetrics {})
        _extensions->post(StatusLineEvents::FontChanged { .metrics = _statusLineFont });
    // Nor any state: everything they follow already is replayed, and what they follow once loaded later.
    _extensionEvents.refresh();
    auto replay = _extensionEvents.subscriptions()
                  | StatusLineEventKinds::fromValue(_extensionReplay.exchange(0, std::memory_order_acquire));
    replay.disable(StatusLineEventKind::Font);
    replayStatusLineState(replay);
    breakLoopAndRefreshRenderBuffer();
}

std::vector<StatusLineEvent> Terminal::currentStatusLineEvents(StatusLineEventKind kind) const
{
    using namespace StatusLineEvents;
    auto events = std::vector<StatusLineEvent> {};
    switch (kind)
    {
        case StatusLineEventKind::Title:
            if (!_windowTitle.empty())
                events.emplace_back(TitleChanged { .title = _windowTitle });
            break;
        case StatusLineEventKind::Cwd:
            if (!_currentWorkingDirectory.path.empty())
                events.emplace_back(CwdChanged { .cwd = _currentWorkingDirectory.path });
            break;
        case StatusLineEventKind::InputMode:
            events.emplace_back(InputModeChanged { .mode = _inputHandler.mode() });
            break;
        case StatusLineEventKind::Tabs: {
            auto const lock = std::lock_guard { _guiTabInfoMutex };
            if (!_guiTabInfoForStatusLine.tabs.empty())
                events.emplace_back(statusLineTabsOf(_guiTabInfoForStatusLine));
            break;
        }
        case StatusLineEventKind::Focus:
            events.emplace_back(
                FocusChanged { .state = _focused ? StatusLineFocus::Focused : StatusLineFocus::Unfocused });
            break;
        case StatusLineEventKind::Progress:
            if (_progress.state != ProgressState::Inactive)
                events.emplace_back(ProgressChanged { .progress = _progress });
            break;
        case StatusLineEventKind::ForegroundProcess:
            if (!_foregroundProcessName.empty())
                events.emplace_back(ForegroundProcessChanged { .name = _foregroundProcessName });
            break;
        case StatusLineEventKind::Font:
            if (_statusLineFont != StatusLineFontMetrics {})
                events.emplace_back(FontChanged { .metrics = _statusLineFont });
            break;
        case StatusLineEventKind::UserVar:
            // Decoded here, as the live hook decodes; the adapter makes both valid UTF-8 alike.
            for (auto const& [name, base64Value]: _userVars)
                events.emplace_back(
                    UserVarChanged { .name = name, .value = core::base64::decode(base64Value) });
            break;
    }
    return events;
}

void Terminal::replayStatusLineState(StatusLineEventKinds kinds)
{
    if (!_extensions || kinds.none())
        return;
    for (auto const kind: StatusLineEventKindList)
        if (kinds.test(kind))
            for (auto& event: currentStatusLineEvents(kind))
                _extensions->post(std::move(event));
}

void Terminal::drainStatusLineReplay()
{
    // One relaxed load per frame while nothing is pending.
    if (!_extensions || _extensionReplay.load(std::memory_order_relaxed) == 0) [[likely]]
        return;
    replayStatusLineState(
        StatusLineEventKinds::fromValue(_extensionReplay.exchange(0, std::memory_order_acquire)));
}

Line Terminal::takeOverRowOf(StatusLineExtensionSnapshot const* snapshot)
{
    if (!snapshot || snapshot->lines.empty())
        return Line {};
    auto row = snapshot->lines.front();
    // Its own ids and its own remover: the frame's images belong to no terminal, and an id another pool
    // handed out may be one this pool has given a different image.
    rehomeImageFragments(row, [this](Image const& image) {
        return _imagePool.create(image.format(), image.size(), Image::Data(image.data()));
    });
    clearHyperlinks(row);
    return row;
}

std::string Terminal::statusLineExtensionText(StatusLineDefinitions::Extension const& item) const
{
    if (!_extensions)
        return {};

    // Outside a repaint, the latest frame; within one, the frame the repaint first read for this name.
    auto snapshot = std::shared_ptr<StatusLineExtensionSnapshot const> {};
    if (!_extensionFrame)
        snapshot = _extensions->snapshot(item.name);
    else if (auto const read = std::ranges::find(_extensionFrame->snapshots,
                                                 std::string_view { item.name },
                                                 [](auto const& entry) { return entry.first; });
             read != _extensionFrame->snapshots.end())
        snapshot = read->second;
    else
        snapshot =
            _extensionFrame->snapshots.emplace_back(item.name, _extensions->snapshot(item.name)).second;

    return snapshot ? snapshot->sgrText : std::string {};
}

void Terminal::resizeStatusLineExtensions()
{
    if (!_extensions)
        return;
    if (auto const size = indicatorStatusLinePageSize(); size != _extensionSurfaceSize)
    {
        _extensionSurfaceSize = size;
        _extensions->resize(size);
    }
}

void Terminal::setStatusLineFontMetrics(StatusLineFontMetrics metrics)
{
    if (!_extensions || metrics == _statusLineFont)
        return;
    _statusLineFont = metrics;
    // Not through the subscription gate: every extension's surface takes its cell size from it.
    _extensions->post(StatusLineEvents::FontChanged { .metrics = metrics });
}

void Terminal::foregroundProcessChanged(std::string_view name)
{
    // Remembered for extensions that follow it later: its host tells a name only once.
    if (_extensions) [[unlikely]]
        _foregroundProcessName = name;
    _extensionEvents.post(
        [&] { return StatusLineEvents::ForegroundProcessChanged { .name = std::string { name } }; });
}

void Terminal::userVarChanged(std::string_view name, std::string_view base64Value)
{
    if (_extensions) [[unlikely]]
        rememberUserVar(name, base64Value);
    // Decoded inside the event's builder, which runs only if an extension subscribed.
    _extensionEvents.post([&] {
        return StatusLineEvents::UserVarChanged { .name = std::string { name },
                                                  .value = core::base64::decode(base64Value) };
    });
}

void Terminal::rememberUserVar(std::string_view name, std::string_view base64Value)
{
    if (auto const known = std::ranges::find(_userVars, name, &RememberedUserVar::first);
        known != _userVars.end())
        known->second = base64Value;
    else if (_userVars.size() < _maxRememberedUserVars)
        _userVars.emplace_back(std::string { name }, std::string { base64Value });
}

void Terminal::inputModeChanged(ViMode mode)
{
    _eventListener.inputModeChanged(mode);
    _extensionEvents.post([&] { return StatusLineEvents::InputModeChanged { .mode = mode }; });
}

void Terminal::setGuiTabInfoForStatusLine(TabsInfo&& info)
{
    auto const l = std::lock_guard { _guiTabInfoMutex };
    // The host refreshes the tabs far more often than they change; an unchanged set is no event.
    if (_extensionEvents.isSubscribedTo(StatusLineEventKind::Tabs) && info != _guiTabInfoForStatusLine)
        [[unlikely]]
        _extensions->post(statusLineTabsOf(info));
    _guiTabInfoForStatusLine = std::move(info);
}

bool Terminal::refreshRenderBuffer(bool locked)
{
    _renderBuffer.state = RenderBufferState::RefreshBuffersAndTrySwap;
    ensureFreshRenderBuffer(locked);
    return _renderBuffer.state == RenderBufferState::WaitingForRefresh;
}

template <std::invocable<RenderBuffer&> Fill>
bool Terminal::refreshRenderBufferWith(Fill const& fill)
{
    if (isRenderingSuppressed())
    {
        // _renderBuffer.state = RenderBufferState::WaitingForRefresh;
        return false;
    }

    auto const now = currentTime();
    auto const elapsed = now - _renderBuffer.lastUpdate.load(std::memory_order_relaxed);
    auto const avoidRefresh = elapsed < _refreshInterval.get().value;

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
            fill(backBuffer);
            auto const cursorChanged =
                lastCursorPos.has_value() != backBuffer.cursor.has_value()
                || (backBuffer.cursor.has_value() && backBuffer.cursor->position != lastCursorPos->position);
            if (cursorChanged)
                // Fired with the state mutex held on the locked path; see the
                // Events::cursorPositionChanged contract.
                _eventListener.cursorPositionChanged();
            _renderBuffer.state = RenderBufferState::TrySwapBuffers;
            [[fallthrough]];
        }
        case RenderBufferState::TrySwapBuffers: {
            [[maybe_unused]] auto const success = _renderBuffer.swapBuffers(now);

#ifdef CONTOUR_PERF_STATS
            logRenderBufferSwap(success, _lastFrameID);
#endif

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
            // Passively invoked by the terminal thread -> do inform render thread about updates.
            if (success)
                _eventListener.renderBufferUpdated();
#endif
        }
        break;
    }
    return true;
}

bool Terminal::ensureFreshRenderBuffer(bool locked)
{
    // The display's frames: link detection sees them.
    if (!locked)
        return refreshRenderBufferWith([this](RenderBuffer& output) { fillRenderBuffer(output, true); });
    return refreshRenderBufferWith([this](RenderBuffer& output) { fillRenderFrame(output, true); });
}

PageSize Terminal::TheSelectionHelper::pageSize() const noexcept
{
    return terminal->pageSize();
}

bool Terminal::TheSelectionHelper::wrappedLine(LineOffset line) const noexcept
{
    // Asked by the word walks before they step into the row above, which must therefore still exist. Of the
    // page on display, which the walks read: only the primary page records its wrapped rows.
    return terminal->displayedScreenType() == ScreenType::Primary
           && terminal->primaryScreen().grid().continuesRowAbove(line);
}

bool Terminal::TheSelectionHelper::cellEmpty(CellLocation pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->displayedPage().isCellEmpty(pos);
}

int Terminal::TheSelectionHelper::cellWidth(CellLocation pos) const noexcept
{
    // Word selection may be off by one
    pos.column = std::min(pos.column, boxed_cast<ColumnOffset>(terminal->pageSize().columns - 1));

    return terminal->displayedPage().cellWidthAt(pos);
}

bool Terminal::TheSelectionHelper::cellHoldsText(CellLocation pos) const noexcept
{
    return !terminal->displayedPage().at(pos).flags().any(NoTextCellFlags);
}

void Terminal::updateInputMethodPreeditString(std::string preeditString)
{
    auto const composing = !preeditString.empty();
    {
        auto const _ = std::scoped_lock { _inputMethodMutex };
        if (_inputMethodData.preeditString == preeditString)
            return;
        _inputMethodData.preeditString = std::move(preeditString);
    }

    // Composing is typing, and typing reveals the cursor, which the text being composed is drawn at.
    if (composing)
        scrollToBottomOnInput();

    screenUpdated();
}

InputMethodData Terminal::inputMethodData() const
{
    auto const _ = std::scoped_lock { _inputMethodMutex };
    return _inputMethodData;
}

void Terminal::fillRenderBuffer(RenderBuffer& output, bool includeSelection)
{
    auto const _ = std::lock_guard { _ownStateLock };
    fillRenderFrame(output, includeSelection);
}

void Terminal::fillRenderFrame(RenderBuffer& output, bool includeSelection)
{
    drainStatusLineReplay();
    // The fill reads _screenDirty anyway; link detection takes its signal of new output from here, so that
    // the parser's path gains no instruction. Before the build, so that its hover query sees what detection
    // adopts.
    auto const change = _screenDirty.exchange(false) ? FrameChange::ScreenChanged : FrameChange::Unchanged;
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    // Every fill runs on the parser thread here, and detection's collect stays off the parser's path.
    std::ignore = change;
#else
    if (_linkDetector)
        detectLinks(change);
#endif
    fillRenderBufferInternal(output, includeSelection);
}

void Terminal::detectLinks(FrameChange change)
{
    auto const now = currentTime();
    _linkSource.beginFrame();
    // A link may have appeared under a pointer that has not moved. The frame draws it anyway, from a query of
    // its own; the tooltip and the pointer shape read this.
    if (_linkDetector->onFrame(_linkSource, now, change) == LinkIndexChange::Changed)
        refreshHoveredLink(currentMouseGridPosition());
    // Published after every frame: a scan the frame left due -- a match it dropped as stale -- is known only
    // from here, and nextRender() reads this without the lock.
    auto const due = _linkDetector->nextDue(now);
    _linkScanDue.store(due ? now + *due : chrono::steady_clock::time_point::max(), std::memory_order_relaxed);
}

void Terminal::fillRenderBufferInternal(RenderBuffer& output, bool includeSelection)
{
    ZoneScoped;
    verifyState();

    output.clear();

    _changes.store(0);
    ++_lastFrameID;

#ifdef CONTOUR_PERF_STATS
    if (terminalLog)
        terminalLog()("{}: Refreshing render buffer.\n", _lastFrameID.load());
#endif

    auto baseLine = LineOffset(0);

    if (_settings.statusDisplayPosition == StatusDisplayPosition::Top)
        baseLine += fillRenderBufferStatusLine(output, includeSelection, baseLine).as<LineOffset>();

    // The two must agree -- mainPageTopRow() is what every hit-test maps a pixel back through, and a
    // grid drawn somewhere else would answer for a row the user did not point at.
    assert(baseLine == mainPageTopRow());

    auto const mainDisplayReverseVideo = isModeEnabled(vtbackend::DECMode::ReverseVideo);

    // The links under the pointer are asked for once, here, as this frame's own: the pointer's cell holds
    // another link after a scroll, a screen switch or new output even while the pointer stands still, and
    // no pointer event says so. Pure queries -- unlike updateHoveringHyperlinkState() they tell nobody. There
    // may be no index: a crossfade snapshot builds with no detection before it.
    auto const pointer = currentMouseGridPosition();
    auto const links = FrameLinks { .hoveredHyperlink = hyperlinkIdUnderPointer(pointer),
                                    .detectedLinks = detectedLinks(),
                                    .hoveredDetectedLink = detectedLinkIdUnderPointer(pointer) };

    // A non-empty search pattern forces every visible line onto the per-cell render path (the
    // uniform-line fast path emits one RenderLine and no per-cell data). Hint mode needs the same:
    // its overlay dims unmatched cells and stamps label glyphs by rewriting per-cell data, so while
    // a hint session is active every line must take the per-cell path, or a label on a plain line —
    // the common case — would never be drawn. Grid::render reuses its search-highlight flag as that
    // per-cell switch.
    auto const renderLinesPerCell = (!_search.pattern.empty() || isHintModeActive())
                                        ? HighlightSearchMatches::Yes
                                        : HighlightSearchMatches::No;

    auto const theCursorPosition = [&]() -> std::optional<CellLocation> {
        if (inputHandler().mode() == ViMode::Insert)
        {
            // Not while DECSASD selects the status line: its cursor is none of the main display's cells.
            if (isModeEnabled(DECMode::VisibleCursor) && _activeStatusDisplay == ActiveStatusDisplay::Main)
                return optional<CellLocation> { currentScreen().cursor().position };
            return std::nullopt;
        }
        return _viCommands.cursorPosition;
    }();

    // When smooth scrolling is active with a non-zero pixel offset,
    // render one extra line at the bottom to fill the gap caused by the sub-cell shift.
    auto const smoothScrollExtra = smoothScrollExtraLines();

    // When DECPCCM is reset and the cursor's page is not on display, its cursor is not drawn (DEC STD 070:
    // "The cursor is not shown in this case"). The vi cursor is on the page on display, and always is.
    auto const effectiveCursorPosition =
        inputHandler().mode() == ViMode::Insert && _cursorPage != _displayedPage
            ? std::optional<CellLocation> {}
            : theCursorPosition;

    auto& displayedScreen = pageAt(_displayedPage);

    // One builder for either branch: Grid::render takes it by forwarding reference.
    auto builder = RenderBufferBuilder { *this,
                                         displayedScreen,
                                         output,
                                         baseLine,
                                         mainDisplayReverseVideo,
                                         _colorPalette.colorLookupTable,
                                         HighlightSearchMatches::Yes,
                                         inputMethodData(),
                                         effectiveCursorPosition,
                                         includeSelection,
                                         links,
                                         RenderedDisplay::Main };
    if (foldingAppliesToDisplayedPage())
        _lastRenderPassHints = displayedScreen.render(
            builder, _viewport.scrollOffset(), renderLinesPerCell, smoothScrollExtra, foldProjection());
    else
        _lastRenderPassHints = displayedScreen.render(builder, _viewport.scrollOffset(), renderLinesPerCell);

    // After both branches: the gutter and the fold labels belong to the main display whichever page it is
    // showing, and each is a no-op on a page folding does not apply to.
    fillGutter(output, baseLine);
    fillFoldLabels(output, baseLine);

    // After the gutter, with the same baseLine: the header covers the main page's first row, wherever a top
    // status line put it.
    fillStickyHeader(output, baseLine);
    fillLineFlash(output, baseLine);

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
    publishAnimationsEnd();

    // Instrumentation: how many RenderCells this frame carries, against the page it was built from.
    // The two should be close; they are not, and finding out why is an open backlog item.
    ZoneValue(static_cast<int64_t>(output.cells.size()));
}

void Terminal::updateCursorMotionAnimation(RenderBuffer& output)
{
    auto const now = currentTime();

    // A finished motion ends here, under the lock the fill holds, as everything else that changes it does.
    if (_cursorMotion.active && _cursorMotion.isComplete(now))
        _cursorMotion.active = false;

    if (!output.cursor.has_value())
        return;

    // The status line's cursor is not animated: its rows are none of the viewport's, and a motion between the
    // two displays would sweep across the page.
    if (output.cursor->display == RenderedDisplay::StatusLine)
        return;

    // Effective animation duration: at least 3 frames at the current refresh rate
    // to guarantee visible interpolation.
    constexpr auto MinFrames = 3;
    auto const effectiveDuration =
        std::max(_settings.cursorMotionAnimationDuration, _refreshInterval.get().value * MinFrames);

    // Use grid coordinates (without scroll offset or baseLine) for animation state.
    // This prevents viewport scrolling from triggering spurious cursor animations,
    // since the grid position doesn't change when the user scrolls.
    auto const gridCursorPos = gridCursorPosition();

    // Grid -> screen is a projection LOOKUP, not an addition: with folds collapsed the rows on screen
    // are a non-contiguous selection of grid rows. Taking the difference at the cursor's own line and
    // adding it to the animation's start line -- a different grid line -- put the interpolation's
    // origin on the wrong row, and on a different wrong row each frame as scrolling reshaped the
    // projection, so the cursor jittered along its path instead of sliding down it.
    //
    // The status-line shift IS a constant, so it is recovered by difference against the position
    // renderCursor() already computed through the very same translation.
    auto const baseLine =
        output.cursor->position.line - _viewport.translateGridToScreenCoordinate(gridCursorPos.line);
    // A position between two lines is translated by the line it starts on, and keeps its fraction past it.
    auto const toScreen = [&](FractionalCellLocation gridPosition) {
        auto const line = std::floor(gridPosition.line);
        auto const screenLine =
            baseLine + _viewport.translateGridToScreenCoordinate(LineOffset::cast_from(line));
        return FractionalCellLocation { .line = static_cast<float>(*screenLine) + (gridPosition.line - line),
                                        .column = gridPosition.column };
    };

    if (_cursorMotion.active && !_cursorMotion.isComplete(now))
    {
        // Animation in progress — check if target changed (chaining)
        if (gridCursorPos != _cursorMotion.toPosition)
        {
            // Chain: start new animation from current interpolated position, where the cursor is drawn -- a
            // whole cell short of it would snap the drawn cursor back toward where it came from. The two
            // agree where grid lines map one to one onto screen rows between the ends; a collapsed fold
            // between them puts the interpolated grid line on a row the screen does not show, as whole cells
            // did before.
            auto const currentProgress = _cursorMotion.progress(now);
            _cursorMotion.fromPosition =
                lerpCellLocation(_cursorMotion.fromPosition, _cursorMotion.toPosition, currentProgress);
            _cursorMotion.fromColor =
                mixColor(_cursorMotion.fromColor, _cursorMotion.toColor, currentProgress);
            _cursorMotion.toPosition = gridCursorPos;
            _cursorMotion.toColor = output.cursor->cursorColor;
            _cursorMotion.startTime = now;
            _cursorMotion.duration = effectiveDuration;
        }
        // Inject animation data (convert grid -> screen coordinates)
        output.cursor->animateFrom = toScreen(_cursorMotion.fromPosition);
        output.cursor->animationProgress = _cursorMotion.progress(now);
        output.cursor->animateFromColor = _cursorMotion.fromColor;
    }
    else if (gridCursorPos != _cursorMotion.toPosition && _settings.cursorMotionAnimationDuration.count() > 0)
    {
        // Start new animation (store grid positions)
        _cursorMotion.active = true;
        _cursorMotion.fromPosition = FractionalCellLocation::of(_cursorMotion.toPosition); // previous target
        _cursorMotion.fromColor = _cursorMotion.toColor; // previous target's color
        _cursorMotion.toPosition = gridCursorPos;
        _cursorMotion.toColor = output.cursor->cursorColor;
        _cursorMotion.startTime = now;
        _cursorMotion.duration = effectiveDuration;

        output.cursor->animateFrom = toScreen(_cursorMotion.fromPosition);
        output.cursor->animationProgress = _cursorMotion.progress(now);
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

    auto const progress = _screenTransition.progress(currentTime());
    if (progress >= 1.0f)
    {
        finalizeScreenTransition();
        return;
    }

    auto const defaultBg = _colorPalette.defaultBackground;

    // Determine the line offset range that covers the main display (excluding the status line).
    auto const mainLineBegin = mainPageTopRow();
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

    switch (_statusDisplayType.get())
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
                                                                includeSelection,
                                                                FrameLinks {},
                                                                RenderedDisplay::StatusLine },
                                          ScrollOffset(0));
            // The terminal's own furniture, so its bar may span the window's whole width (#2139), and its
            // text the gutter's columns as well as the page's.
            output.statusBar = RenderStatusBar {
                .lineOffset = base,
                .lines = _indicatorStatusScreen.pageSize().lines,
                .background = indicatorStatusLineColors().background,
                .leadingColumns = _indicatorStatusScreen.pageSize().columns - _settings.pageSize.columns,
            };
            return _indicatorStatusScreen.pageSize().lines;
        case StatusDisplayType::HostWritable: {
            // While DECSASD selects it, the status line's own active position is the cursor, drawn on its
            // row: DEC STD 070 ch. 14 gives the status display an active position and a cursor of its own,
            // and xterm and mlterm draw it there. The one DECTCEM enables it, the main display's too: see the
            // DECTCEM case of setMode() for why there is one.
            auto const statusLineCursor =
                _activeStatusDisplay == ActiveStatusDisplay::StatusLine
                        && inputHandler().mode() == ViMode::Insert && isModeEnabled(DECMode::VisibleCursor)
                    ? optional<CellLocation> { _hostWritableStatusLineScreen.cursor().position }
                    : nullopt;
            _hostWritableStatusLineScreen.render(RenderBufferBuilder { *this,
                                                                       _hostWritableStatusLineScreen,
                                                                       output,
                                                                       base,
                                                                       !mainDisplayReverseVideo,
                                                                       StatusLineColorLookupTable,
                                                                       HighlightSearchMatches::No,
                                                                       InputMethodData {},
                                                                       statusLineCursor,
                                                                       includeSelection,
                                                                       FrameLinks {},
                                                                       RenderedDisplay::StatusLine },
                                                 ScrollOffset(0));
            return _hostWritableStatusLineScreen.pageSize().lines;
        }
    }
    core::unreachable();
}
// }}}

PageSize Terminal::indicatorStatusLinePageSize() const
{
    auto const gutterColumns = ColumnCount::cast_from(gutterLayoutFor(_settings.gutter).totalColumns);
    return PageSize { StatusLineHeight, _settings.pageSize.columns + gutterColumns };
}

RGBColorPair Terminal::indicatorStatusLineColors() const
{
    if (!_focused)
        return colorPalette().indicatorStatusLineInactive;
    if (showsModeChip(_indicatorStatusLineDefinition))
        return colorPalette().indicatorStatusLineInsertMode;
    return indicatorStatusLineColorsFor(_inputHandler.mode());
}

RGBColorPair Terminal::indicatorStatusLineColorsFor(ViMode mode) const noexcept
{
    switch (mode)
    {
        case ViMode::Insert: return colorPalette().indicatorStatusLineInsertMode;
        case ViMode::Normal:
        case ViMode::Hint: return colorPalette().indicatorStatusLineNormalMode;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock: return colorPalette().indicatorStatusLineVisualMode;
    }
    core::unreachable();
}

void Terminal::updateIndicatorStatusLine()
{
    Require(_activeStatusDisplay != ActiveStatusDisplay::IndicatorStatusLine);

    // Refitted here as well as on a resize: the gutter's width follows its settings, which a configuration
    // reload changes without the page necessarily changing size.
    if (auto const wanted = indicatorStatusLinePageSize(); _indicatorStatusScreen.pageSize() != wanted)
        _indicatorStatusScreen.applyPageSizeToMainDisplay(wanted);
    resizeStatusLineExtensions();

    auto const colors = indicatorStatusLineColors();

    auto const backupForeground = _colorPalette.defaultForeground;
    auto const backupBackground = _colorPalette.defaultBackground;
    _colorPalette.defaultForeground = colors.foreground;
    _colorPalette.defaultBackground = colors.background;

    auto const _ = core::Finally { [&]() {
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

    if (_mirroredIndicatorRow) [[unlikely]]
    {
        // A mirror shows what its host drew, extensions and all, and runs neither the template nor them.
        auto& row = _indicatorStatusScreen.grid().lineAt(LineOffset(0));
        row = *_mirroredIndicatorRow;
        row.resize(_indicatorStatusScreen.pageSize().columns);
        row.resolveDefaultColors(colors);
        return;
    }

    if (_takeOverName) [[unlikely]]
    {
        // A take-over extension draws the whole row, graphics and all, in place of the template; until its
        // first frame the row stays empty.
        if (auto snapshot = _extensions->snapshot(*_takeOverName); snapshot != _takeOverSource)
        {
            _takeOverRow = takeOverRowOf(snapshot.get());
            _takeOverSource = std::move(snapshot);
        }
        if (_takeOverSource)
        {
            auto& row = _indicatorStatusScreen.grid().lineAt(LineOffset(0));
            row = _takeOverRow;
            row.resize(_indicatorStatusScreen.pageSize().columns);
            row.resolveDefaultColors(colors);
        }
        return;
    }

    refreshGitHead();

    // Every {Extension} item of this repaint reads one frame per name, so that two slots, and the measuring
    // of a slot, cannot see two generations.
    auto extensionFrame = StatusLineExtensionFrame {};
    _extensionFrame = &extensionFrame;
    auto const closeExtensionFrame = core::Finally { [&]() { _extensionFrame = nullptr; } };

    using Styling = StatusLineStyling;
    auto const& definitions = _indicatorStatusLineDefinition;

    auto const columns = _indicatorStatusScreen.pageSize().columns;
    auto const policy = _indicatorStatusScreen.clusterWidthPolicy();
    auto const sides = std::array {
        std::pair { StatusLineSide::Left, &definitions.left },
        std::pair { StatusLineSide::Middle, &definitions.middle },
        std::pair { StatusLineSide::Right, &definitions.right },
    };

    // Each item is visited once to measure it, its unstyled text kept: a segment's text is its items' texts
    // joined, so a segment is measured as the grid lays it out -- by columns, not bytes, and with a cluster
    // that joins across two items as one -- without visiting its items again.
    auto texts = std::vector<std::string> {};
    auto extents = std::vector<StatusLineItemExtent> {};
    for (auto const& [side, segment]: sides)
        for (auto const& item: *segment)
        {
            auto& text = texts.emplace_back(serializeToVT(*this, std::span { &item, 1 }, Styling::Disabled));
            extents.push_back(StatusLineItemExtent { .side = side,
                                                     .width = columnsOnTheGrid(text, policy),
                                                     .priority = StatusLineDefinitions::priorityOf(item),
                                                     .kind = StatusLineDefinitions::kindOf(item) });
        }
    auto const measureSegment = [&](StatusLineSide side, std::span<StatusLineItemFate const> fates) {
        auto joined = std::string {};
        for (auto const [index, extent]: core::views::enumerate(extents))
            if (extent.side == side && fates[index] == StatusLineItemFate::Shown)
                joined += texts[index];
        return columnsOnTheGrid(joined, policy);
    };

    // Too narrow for every item, the line drops whole ones by priority until each segment is placed.
    auto const fates = collapseStatusLine(extents, columns, measureSegment);
    auto const placement = placeStatusLineSegments(
        StatusLineSegmentWidths { .left = measureSegment(StatusLineSide::Left, fates),
                                  .middle = measureSegment(StatusLineSide::Middle, fates),
                                  .right = measureSegment(StatusLineSide::Right, fates) },
        columns);

    // Draws the shown items of the segment on @p side, styled; an item's text does not depend on its
    // neighbours, so they are drawn one by one, and nothing is copied for a line that dropped some.
    auto const draw = [&](StatusLineSide side) {
        auto text = std::string {};
        auto index = std::size_t { 0 };
        for (auto const& [segmentSide, segment]: sides)
            for (auto const& item: *segment)
                if (fates[index++] == StatusLineItemFate::Shown && segmentSide == side)
                    text += serializeToVT(*this, std::span { &item, 1 }, Styling::Enabled);
        writeToScreenInternal(_indicatorStatusScreen, text);
    };

    // The left segment starts in the first column, and one wider than the line is cut at its last.
    draw(StatusLineSide::Left);
    if (placement.middleStart)
    {
        _indicatorStatusScreen.moveCursorToColumn(*placement.middleStart);
        draw(StatusLineSide::Middle);
    }
    if (placement.rightStart)
    {
        _indicatorStatusScreen.moveCursorToColumn(*placement.rightStart);
        draw(StatusLineSide::Right);
    }

    // What the template printed may reset its rendition -- a {Command}'s output and an {Extension}'s text are
    // written as printed, SGR and all -- which leaves the default colours on the cells after it. Those are
    // the indicator's, not the main page's.
    _indicatorStatusScreen.grid().lineAt(LineOffset(0)).resolveDefaultColors(colors);
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
    //
    // Asked first without the state lock, which a key must not wait for: a view being typed into is at the
    // bottom all but always. Only a view away from it takes the lock, to move it.
    if (!_viewport.isAwayFromBottom())
        return;

    auto const _ = std::scoped_lock { *this };
    _viewport.scrollToBottom();
}

template <typename Dispatch>
Handled Terminal::offerToViInput(Dispatch const& dispatch)
{
    // Hint mode is a Vi mode of its own, so in Insert mode neither wants the key.
    if (_inputHandler.mode() == ViMode::Insert)
        return Handled { false };

    // Taken once, here, rather than by each command. The frontend hooks a command raises under it --
    // screenUpdated(), the viewport, mode, selection and search notifications -- take no lock and wait on no
    // other thread: they post, or read what takes no lock. What would is held back until the lock is
    // released: a flush, whose local echo parses, a clipboard paste, which pastes through a flush, and a
    // document to open.
    return runUnderStateLock(dispatch);
}

void Terminal::acquireState(HoldBack holdBack) const
{
    _stateMutex.lock();
    _stateLockOwner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    _stateHoldBack = holdBack;
}

bool Terminal::tryAcquireState(HoldBack holdBack) const
{
    if (!_stateMutex.try_lock())
        return false;
    _stateLockOwner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    _stateHoldBack = holdBack;
    return true;
}

void Terminal::releaseState() const
{
    _stateLockOwner.store(std::thread::id {}, std::memory_order_relaxed);
    _stateMutex.unlock();
}

void Terminal::OwnStateLock::lock() const
{
    _terminal->acquireState(HoldBack::No);
}

void Terminal::OwnStateLock::unlock() const
{
    _terminal->releaseOwnState();
}

void Terminal::releaseOwnState()
{
    // Taken while the lock is still held, as it is written under it.
    auto const owed = std::exchange(_owedFlush, OwedFlush::None);
    releaseState();
    if (owed == OwedFlush::Owed)
        runContained("sending the replies raised under it", [this] { flushInput(); });
}

bool Terminal::OwnStateLock::try_lock() const // NOLINT(readability-identifier-naming)
{
    return _terminal->tryAcquireState(HoldBack::No);
}

void Terminal::unlock() noexcept
{
    // Taken while the lock is still held, as it is written under it.
    auto const heldBack = std::exchange(_outsideStateLock, {});
    releaseState();

    // Written here, or by the PTY's reader, woken for it, as Settings::inputWriter says. @see flushInput()
    runContained("sending the input queued under it", [this] { flushInput(); });
    for (auto const& action: heldBack)
        runContained("asking for what it held back", action);
}

void Terminal::runOutsideStateLock(std::function<void()> action)
{
    if (holdsBack())
    {
        _outsideStateLock.emplace_back(std::move(action));
        return;
    }

    action();
}

Handled Terminal::sendKeyEvent(Key key,
                               KeyboardModifiers modifiers,
                               KeyboardEventType eventType,
                               Timestamp now)
{
    restartCursorBlink(now);

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

    if (offerToViInput([&] {
            // Route Escape to hint mode handler if active (ignore key release events).
            if (_hintModeHandler.isActive() && key == Key::Escape && eventType != KeyboardEventType::Release)
            {
                _hintModeHandler.processInput(U'\x1B');
                return Handled { true };
            }
            return _inputHandler.sendKeyPressEvent(key, chord, eventType);
        }))
        return Handled { true };

    // Check for User-Defined Keys (DECUDK) — override function keys F6-F20
    // when no modifiers are pressed and the key is being pressed (not released). Looked up under the input
    // lock the parser thread programs them under, not the state lock, so that typing waits for no parse.
    if (chord.none() && eventType == KeyboardEventType::Press)
    {
        if (auto const udkStr = udkStringForKey(key); udkStr.has_value())
        {
            core::locked(_inputMutex, [&] { _inputGenerator.generateRaw(*udkStr); });
            flushInput();
            scrollToBottomOnInput();
            return Handled { true };
        }
    }

    bool const success =
        core::locked(_inputMutex, [&] { return _inputGenerator.generate(key, modifiers, eventType); });
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
                                KeyIdentity keyIdentity,
                                KeyboardModifiers modifiers,
                                KeyboardEventType eventType,
                                Timestamp now)
{
    restartCursorBlink(now);

    // Early exit while the host's KAM or the user's lock holds the keyboard.
    if (!allowInput())
        return Handled { true };

    // Suppress character repeat events when DECARM (auto-repeat mode) is disabled.
    if (eventType == KeyboardEventType::Repeat && !isModeEnabled(DECMode::AutoRepeat))
        return Handled { true };

    // See sendKeyEvent(): the hint-mode handler and the Vi input handler match on the chord, the
    // input generator reports the latched lock state to the application.
    auto const chord = modifiers.chord;

    if (offerToViInput([&] {
            // Route input to hint mode handler if active (no modifiers — just label chars).
            // Ignore key release events to prevent the activating key's release from being
            // processed as a label character (e.g. 'h' release after 'gh' triggered hint mode).
            if (_hintModeHandler.isActive() && chord.none() && eventType != KeyboardEventType::Release
                && _hintModeHandler.processInput(ch))
                return Handled { true };
            return _inputHandler.sendCharPressEvent(ch, chord, eventType);
        }))
        return Handled { true };

    auto const success = core::locked(
        _inputMutex, [&] { return _inputGenerator.generate(ch, keyIdentity, modifiers, eventType); });
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
                                      CellLocation newPosition,
                                      PixelCoordinate pixelPosition,
                                      bool uiHandledHint,
                                      MousePressClaimant const& claimant)
{
    // The event carries the chord held at its moment: the one a frontend learns of without a key event.
    (void) updateHeldModifiers(modifiers);
    _currentMousePixelPosition = pixelPosition;

    // A press is not necessarily preceded by a MouseMove landing exactly here: the windowing system
    // usually delivers one, which is why a slow, deliberate drag never showed this, but a fast click
    // can arrive as a bare press while _currentMousePosition is still wherever the pointer last idled
    // (a previous selection, a different pane, ...). Without this, handleMouseSelection() below
    // anchors the drag on that stale position instead of where the button actually went down, and the
    // whole selection appears to jump the moment the drag starts. Mirrors the same update
    // sendMouseMoveEvent already does for exactly this reason. A pointer back from off the grid lands
    // somewhere new as well, even on the cell it left from.
    if (newPosition != _currentMousePosition || _pointerPlacement == PointerPlacement::OffGrid)
    {
        _currentMousePosition = newPosition;
        _pointerPlacement = PointerPlacement::OnGrid;
        updateHoveringHyperlinkState();
    }

    // Counted once, for every button, before anyone looks at it: the count is both what a `clicks:`
    // binding matches on and what picks drag, word or line below. A press on another cell starts a new
    // sequence by the counter's own rule -- which is what this function used to reset by hand -- and
    // each button keeps its own, so a right-click for the context menu does not end a double-click.
    auto const clickCount = _clickCounter.press(_currentMousePosition, button);

    // The terminal's own selection -- and so a binding standing in for it -- sees a press only when no
    // application asked for the mouse, or the bypass modifier overrides that.
    auto const reachesSelection =
        !allowPassMouseEventToApp(modifiers) || isModeEnabled(DECMode::MousePassiveTracking);
    auto const generationBeforeClaim = _selectionGeneration;
    auto const claim = reachesSelection && claimant ? claimant(clickCount) : MousePressClaim::Unclaimed;

    if (button == MouseButton::Left && claim == MousePressClaim::Claimed)
    {
        // The action the binding runs may select, now or before the release; the release reports it then.
        _leftButtonPress = LeftButtonPress::Claimed;
        _claimedPressGeneration = generationBeforeClaim;
    }
    else if (button == MouseButton::Left)
    {
        _leftButtonPress = LeftButtonPress::Selecting;
        _lastMousePixelPositionOnLeftClick = pixelPosition;
        if (reachesSelection)
            uiHandledHint = handleMouseSelection(modifiers, clickCount) || uiHandledHint;
    }

    verifyState();

    // A press the DEC locator reports is not the application's.
    if (handleLocatorMouseEvent(LocatorEvent::ButtonDown, button))
        return Handled { true };

    // Hand the event to the input generator for app tracking or wheel->cursor-key translation;
    // generateMousePress() self-rejects the cases where neither applies.
    auto const eventHandledByApp =
        (allowPassMouseEventToApp(modifiers) || allowWheelTranslationToApp(button, modifiers))
        && core::locked(_inputMutex, [&] {
               return _inputGenerator.generateMousePress(modifiers,
                                                         button,
                                                         _currentMousePosition + _reportedCellOrigin,
                                                         pixelPosition,
                                                         uiHandledHint);
           });

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

    // Visualizing "other occurrences" only makes sense for a WORD -- the case a double/triple-click
    // selects. An ordinary linear (or rectangular) drag selection is not a word: it is arbitrary text
    // the user is actively choosing, and serializing every intermediate extension of it into the
    // search pattern overwrote whatever the user had actually typed into the find bar/search prompt on
    // every single mouse-move of a plain click-and-drag selection.
    if (!selectionAvailable() || dynamic_cast<WordWiseSelection const*>(selector()) == nullptr)
        return;

    auto const text = extractSelectionText();

    // Also stops being a word the moment it is DRAGGED past its own logical line: RenderBufferBuilder's
    // search-match scanner matches the pattern's bytes -- newlines included -- against the flat,
    // single-line stream of rendered cells, so a multi-line pattern can (and did) never really
    // match anything stable, and its highlight visibly jumped around as the pattern kept growing
    // with every further mouse-move of the drag.
    //
    // The test is the extracted TEXT, not `from().line != to().line`: a double-clicked word that
    // straddles a soft wrap spans two physical rows, and SelectionRenderer deliberately joins those
    // without a break (it flushes a line only where `!isLineWrapped`). Keying on physical rows
    // therefore refused a perfectly good single-line pattern and left the previous search term
    // standing, costing the wrapped word its matching-word highlights.
    if (text.contains('\n'))
        return;

    auto const text32 = unicode::convert_to<char32_t>(string_view(text.data(), text.size()));
    setNewSearchTerm(text32, SearchOrigin::DoubleClick);
}

void Terminal::setStatusLineDefinition(StatusLineDefinition&& definition)
{
    _indicatorStatusLineDefinition = std::move(definition);
    if (_statusLineCommands)
        _statusLineCommands->forgetRequests();
    updateIndicatorStatusLine();
}

void Terminal::resetStatusLineDefinition()
{
    _indicatorStatusLineDefinition = parseStatusLineDefinition(_settings.indicatorStatusLine.left,
                                                               _settings.indicatorStatusLine.middle,
                                                               _settings.indicatorStatusLine.right);
    if (_statusLineCommands)
        _statusLineCommands->forgetRequests();
    updateIndicatorStatusLine();
}

bool Terminal::handleMouseSelection(Modifiers modifiers, uint8_t clickCount)
{
    verifyState();

    // Through the viewport, not by subtracting the scroll offset: with folds collapsed the rows on
    // screen are a non-contiguous selection of grid rows, and anchoring a selection by the old
    // arithmetic while extending it by the new one would start the drag on a different line than it
    // highlights.
    auto const startPos = _viewport.translateScreenToGridCoordinate(_currentMousePosition);

    // Shift+Click extends a completed selection to the new click position.
    // Re-create as LinearSelection to avoid breaking WordWise/FullLine invariants.
    if (modifiers.contains(Modifier::Shift) && selectionAvailable()
        && selector()->state() == Selection::State::Complete)
    {
        _clickCounter.reset(); // Don't count Shift+Click in the multi-click sequence.

        // Anchor at the farthest endpoint so the selection grows in either direction.
        // NB: Store values before std::minmax — minmax returns references, and
        //     selector()->from()/to() return temporaries that would dangle.
        auto const fromPos = selector()->from();
        auto const toPos = selector()->to();
        auto const [selStart, selEnd] = std::minmax(fromPos, toPos);
        auto const anchor = (startPos < selStart) ? selEnd : selStart;

        setSelector(std::make_unique<LinearSelection>(_selectionHelper, anchor, selectionUpdatedHelper()));
        (void) selector()->extend(startPos);

        // NOT updateSelectionMatches(): what this builds is a LinearSelection, which that function
        // rightly refuses to serialize into a search pattern. But a word selection extended this way
        // leaves the double-click's pattern behind it, still painting its matches for a selection
        // that is no longer that word -- so the extension must retire it, exactly as an ordinary
        // single click does below.
        if (_search.origin == SearchOrigin::DoubleClick)
            clearSearch();

        breakLoopAndRefreshRenderBuffer();
        return true;
    }

    if (_inputHandler.mode() != ViMode::Insert)
        _viCommands.cursorPosition = startPos;

    switch (clickCount)
    {
        case 1:
            if (_search.origin == SearchOrigin::DoubleClick)
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
    // Through the viewport, for the reason given in handleMouseSelection above.
    auto const startPos = _viewport.translateScreenToGridCoordinate(_currentMousePosition);
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
    ++_selectionGeneration;
    publishBindingModes();
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
        if (_leftButtonPress != LeftButtonPress::Selecting)
            // Don't clear if in visual mode and mouse wasn't used.
            return;
        _inputHandler.setMode(ViMode::Normal);
    }

    if (!_selection)
        return;

    inputLog()("Clearing selection.");
    _selection.reset();
    ++_selectionGeneration;
    publishBindingModes();

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
    // - the multi-click counter (_clickCounter, for tracking rapid multi click) is reset
    // - grid text selection is extended
    verifyState();

    // avoid applying event for sctatus line or indicator status line
    if (!(isPrimaryScreen() || isAlternateScreen()))
        return;

    (void) updateHeldModifiers(modifiers);
    _currentMousePixelPosition = pixelPosition;

    // Back from off the grid is a move too, even onto the cell it left from.
    if (newPosition != _currentMousePosition || _pointerPlacement == PointerPlacement::OffGrid)
    {
        // Multi-clicks are only counted when not moving the mouse in between, so reset on mouse move here.
        _clickCounter.reset();

        _currentMousePosition = newPosition;
        _pointerPlacement = PointerPlacement::OnGrid;
        updateHoveringHyperlinkState();
    }

    auto const shouldExtendSelection = shouldExtendSelectionByMouse(newPosition, pixelPosition);
    auto relativePos = _viewport.translateScreenToGridCoordinate(newPosition);
    if (shouldExtendSelection && _leftButtonPress == LeftButtonPress::Selecting)
    {
        _viCommands.cursorPosition = relativePos;
        _viewport.makeVisible(_viCommands.cursorPosition.line);
    }

    // Do not handle mouse-move events in sub-cell dimensions.
    if (allowPassMouseEventToApp(modifiers))
    {
        if (core::locked(_inputMutex, [&] {
                return _inputGenerator.generateMouseMove(modifiers,
                                                         relativePos + _reportedCellOrigin,
                                                         pixelPosition,
                                                         uiHandledHint || !selectionAvailable());
            }))
            flushInput();
        if (!isModeEnabled(DECMode::MousePassiveTracking))
            return;
    }

    if (_leftButtonPress == LeftButtonPress::Selecting)
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

void Terminal::sendMouseLeaveEvent()
{
    _pointerPlacement = PointerPlacement::OffGrid;
    updateHoveringHyperlinkState();
}

Handled Terminal::sendMouseReleaseEvent(Modifiers modifiers,
                                        MouseButton button,
                                        PixelCoordinate pixelPosition,
                                        bool uiHandledHint)
{
    verifyState();

    if (button == MouseButton::Left)
    {
        // A claimed press's action completes what it selects itself, so no drag is left to finish: report it,
        // as a release that finishes a drag does, to whoever copies a finished selection.
        if (std::exchange(_leftButtonPress, LeftButtonPress::Released) == LeftButtonPress::Claimed
            && _selectionGeneration != _claimedPressGeneration && selectionAvailable()
            && selector()->state() == Selection::State::Complete)
            _eventListener.onSelectionCompleted();
        if (selectionAvailable())
        {
            switch (selector()->state())
            {
                case Selection::State::InProgress:
                    if (_inputHandler.mode() == ViMode::Insert)
                        selector()->complete();
                    _eventListener.onSelectionCompleted();
                    break;
                case Selection::State::Waiting:
                    _selection.reset();
                    publishBindingModes();
                    break;
                case Selection::State::Complete: break;
            }
        }
    }

    // A release the DEC locator reports is not the application's.
    if (handleLocatorMouseEvent(LocatorEvent::ButtonUp, button))
        return Handled { true };

    if (allowPassMouseEventToApp(modifiers) && core::locked(_inputMutex, [&] {
            return _inputGenerator.generateMouseRelease(
                modifiers, button, _currentMousePosition + _reportedCellOrigin, pixelPosition, uiHandledHint);
        }))
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
    _extensionEvents.post(
        [] { return StatusLineEvents::FocusChanged { .state = StatusLineFocus::Focused }; });

    if (core::locked(_inputMutex, [&] { return _inputGenerator.generateFocusInEvent(); }))
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
    _extensionEvents.post(
        [] { return StatusLineEvents::FocusChanged { .state = StatusLineFocus::Unfocused }; });

    if (core::locked(_inputMutex, [&] { return _inputGenerator.generateFocusOutEvent(); }))
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

    // No branch for "is the user typing a search term": the find bar owns a real text field, which
    // handles its own clipboard. Appending here is what used to corrupt the pattern, since it wrote
    // past the edit buffer the prompt was actually rendering from.
    core::locked(_inputMutex, [&] { _inputGenerator.generatePaste(text); });
    flushInput();
}

bool Terminal::pastesUnchanged(string_view text) const
{
    auto const purpose =
        isModeEnabled(DECMode::BracketedPaste) ? SanitizePurpose::Insert : SanitizePurpose::Type;
    return isUnchangedBySanitizing(text, purpose);
}

void Terminal::sendPasteFromClipboard(unsigned count, bool strip)
{
    runOutsideStateLock([this, count, strip] { _eventListener.pasteFromClipboard(count, strip); });
}

void Terminal::sendRawInput(string_view text)
{
    if (!allowInput())
        return;

    inputLog()("Sending raw input to stdin: {}", core::escape(text));
    core::locked(_inputMutex, [&] { _inputGenerator.generateRaw(text); });
    flushInput();
}

bool Terminal::hasInput() const
{
    return core::locked(_inputMutex, [&] { return !_inputGenerator.peek().empty(); });
}

std::string Terminal::peekInput() const
{
    return core::locked(_inputMutex, [&] { return std::string(_inputGenerator.peek()); });
}

void Terminal::flushInput()
{
    // Never with the state lock held: the write parks once the application leaves its input unread, and
    // every thread waiting for the lock would park with it -- the parser thread, a frontend's, a daemon's
    // loop building a client's delta; and with SRM reset the echo parses, which takes the lock again. Under a
    // frontend's hold it is sent as that hold is released (@see unlock()); under the terminal's own -- a
    // parse batch, some of whose replies ask for it on the spot -- by the same thread once it has let go of
    // the lock (@see releaseOwnState()). The queue keeps the order either way.
    if (isStateLockHeldByThisThread())
    {
        if (_stateHoldBack == HoldBack::No)
            _owedFlush = OwedFlush::Owed;
        return;
    }

    // Under InputWriter::ReaderThread only the PTY's reader writes, once it reads: any other thread -- a GUI
    // thread typing or pasting -- wakes it instead, and never parks on a write the application leaves
    // unread, nor waits behind one the reader is making. Woken whether or not anything is queued, as at the
    // release of a frontend's hold.
    if (_settings.inputWriter == InputWriter::ReaderThread)
    {
        auto const reader = _readerThread.load(std::memory_order_relaxed);
        if (reader != std::thread::id {} && reader != std::this_thread::get_id())
        {
            _pty->wakeupReader();
            return;
        }
    }

    std::string input;
    int rv = 0;
    {
        // Take, write and consume are one step: this is called from the parser, the GUI and a daemon's pump
        // thread, and if two callers could interleave here both would send the same bytes and both would
        // consume them. @see _flushMutex.
        auto const flushing = std::scoped_lock { _flushMutex };

        // Own the bytes: peek() returns a view into InputGenerator::_pendingSequence, which another thread
        // appends to -- and reallocates -- as soon as _inputMutex is free, and the local echo parses these
        // bytes, where a query among them replies and appends to it too. @see echoLocally().
        input = core::locked(_inputMutex, [&] { return std::string(_inputGenerator.peek()); });
        if (input.empty())
            return;

        // XXX Should be the only location that does write to the PTY's stdin to avoid race conditions.
        rv = _pty->write(input);
        auto const error = errno;
        if (rv <= 0)
        {
            // EAGAIN/EINTR is backpressure: keep the bytes pending so the caller's deferred retry sends
            // them. Any other error is fatal for this device -- these bytes will never be delivered, and
            // leaving them pending keeps hasInput() true, which turns TerminalSession::flushInput()'s
            // self-repost into an unbounded loop that logs one error per iteration. That is the
            // "Failed to write to SSH channel" flood a broken SSH session used to produce.
            if (rv < 0 && error != EAGAIN && error != EINTR)
                core::locked(_inputMutex, [&] { _inputGenerator.consume(static_cast<int>(input.size())); });
            return;
        }

        // Only this flush consumes, and others only append, so the head it wrote is still the head.
        core::locked(_inputMutex, [&] { _inputGenerator.consume(rv); });
    }

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

void Terminal::parseExpandingMacros(std::string_view input)
{
    auto const parseGuard = ParseDepthGuard {};
    do
    {
        input.remove_prefix(_parser.parseFragment(input));
        if (auto const invocation = std::exchange(_pendingMacroInvocation, std::nullopt))
            runMacro(*invocation);
    } while (!input.empty() && !(isMacroRunning() && _areRunningMacrosVoid));
}

void Terminal::parseFragmentChunked(string_view vtStream)
{
    ZoneScoped;
    auto const parseGuard = ParseDepthGuard {};

    while (!vtStream.empty())
    {
        if (!_screenWriteBuffer
            || (_screenWriteBuffer->bytesAvailable() < 64
                && _screenWriteBuffer->bytesAvailable() < vtStream.size()))
            _screenWriteBuffer = _ptyBufferPool.allocateBufferObject();
        auto const chunk =
            vtStream.substr(0, std::min(vtStream.size(), _screenWriteBuffer->bytesAvailable()));
        vtStream.remove_prefix(chunk.size());
        // Set parsingBuffer to ensure BufferFragment holds the correct buffer reference.
        _parsingBuffer = _screenWriteBuffer;
        auto const written = _screenWriteBuffer->writeAtEnd(chunk);
        parseExpandingMacros(std::string_view { written.data(), written.size() });
        _parsingBuffer = nullptr;
    }
}

void Terminal::writeToScreen(string_view vtStream)
{
    ZoneScoped;
    TracyPlot("pty.parse.bytes", static_cast<int64_t>(vtStream.size()));
    runParseBatch(std::unique_lock { _ownStateLock }, [&] { parseFragmentChunked(vtStream); });
}

Terminal::BatchClock::BatchClock(Screen& primary, uint32_t stamp) noexcept: _primary { primary }
{
    _primary.setLineBirthStamp(stamp);
}

Terminal::BatchClock::~BatchClock()
{
    _primary.setLineBirthStamp(0);
}

Terminal::BatchClock Terminal::sampleBatchClock() noexcept
{
    // Floored, then clamped below at the session's first second: a wall clock stepped back before the
    // session began must not wrap a row into the far future.
    auto const elapsed = chrono::floor<chrono::seconds>(_clocks.wall.now() - _sessionEpoch);
    auto const stamp = std::clamp(elapsed, chrono::seconds::zero(), chrono::seconds { Line::MaxBornAt - 1 });
    return BatchClock { primaryScreen(), static_cast<uint32_t>(stamp.count()) + 1U };
}

std::optional<chrono::system_clock::time_point> Terminal::lineBirthTime(Line const& line) const noexcept
{
    auto const stamp = line.bornAt();
    if (stamp == 0 || stamp >= Line::MaxBornAt)
        return std::nullopt;
    // A replica's rows count from its host's epoch, by the host's clock: read on this one, as the records'
    // instants are, so a row and its block name one time.
    return onThisClock(_sessionEpoch + chrono::seconds { stamp - 1U });
}

size_t Terminal::maxBulkTextSequenceWidth() const noexcept
{
    // Measured on the display the text lands on: a status line selected with DECSASD has margins and a
    // line of its own.
    return activeDisplay().bulkWritableColumns();
}

// {{{ SimpleSequenceHandler
// This simple sequence handler is used to write to the screen
// without any optimizations (and no parser hooking).
// We use this for rendering the status line.
namespace
{
    /// Whether the row a status line segment is written into has room for more of it.
    enum class RowRoom : uint8_t
    {
        Left = 0, ///< The next character is written if it fits.
        Filled,   ///< The screen holds the cursor at the row's end: only what joins the last character goes.
        Closed,   ///< A character did not fit, or one followed a filled row: nothing more is written.
    };

    struct SimpleSequenceHandler
    {
        Screen& targetScreen;

        /// The indicator status line ends at the page's last column, so a segment is cut there and shows its
        /// head. Autowrap would carry the overflow onto the next row, which on the one-row line is a scroll
        /// that leaves the tail; without autowrap the last column would take every character past it.
        RowRoom rowRoom = RowRoom::Left;

        void executeControlCode(char controlCode) { targetScreen.executeControlCode(controlCode); }

        void processSequence(Sequence const& seq)
        {
            // NB: We might want to check for some VT sequences that should not be processed here.
            // We might make use of Terminal::activeSequences() here somehow.
            targetScreen.processSequence(seq);
        }

        void processAPC(std::string_view body) { targetScreen.processAPC(body); }

        /// Writes @p codepoint where it fits on the row, and nothing once one has not.
        /// @param codepoint The character.
        void writeText(char32_t codepoint)
        {
            if (rowRoom == RowRoom::Closed)
                return;

            auto& cursor = targetScreen.cursor();
            auto const joins = targetScreen.joinsLastCluster(codepoint);
            if (rowRoom == RowRoom::Filled)
            {
                if (!joins)
                {
                    rowRoom = RowRoom::Closed;
                    return;
                }
                // The screen takes a pending wrap before it places a codepoint: held back, what joins the
                // row's last character stays on it.
                auto const wrapPending = std::exchange(cursor.wrapPending, false);
                targetScreen.writeText(codepoint);
                cursor.wrapPending = wrapPending;
                return;
            }

            // A wide character with too few columns left would wrap, or, without autowrap, be dropped.
            auto const width = static_cast<int>(unicode::width(codepoint));
            auto const lastColumn = boxed_cast<ColumnOffset>(targetScreen.pageSize().columns) - 1;
            if (!joins && width > 1 && cursor.position.column + ColumnOffset(width - 1) > lastColumn)
            {
                rowRoom = RowRoom::Closed;
                return;
            }

            auto const column = cursor.position.column;
            targetScreen.writeText(codepoint);

            // The screen says when the row is full. Autowrap holds the cursor on the last column with a wrap
            // pending; without autowrap the cursor stays on what was written last -- on the cell a character
            // started, or inside a cluster a variation selector widened -- and the next character would
            // overwrite it.
            if (cursor.wrapPending || (!joins && cursor.position.column == column)
                || targetScreen.at(cursor.position).isFlagEnabled(CellFlag::WideCharContinuation))
                rowRoom = RowRoom::Filled;
        }

        /// Writes a run of UTF-8 text a character at a time, decoded as the parser decodes text.
        /// @param chars The run.
        void writeText(std::string_view chars, size_t /*cellCount*/)
        {
            auto utf8 = unicode::utf8_decoder_state {};
            for (auto const byte: chars)
                if (auto const codepoint = vtparser::decodeUtf8Byte(utf8, static_cast<uint8_t>(byte)))
                    writeText(*codepoint);
        }

        void writeTextEnd() { targetScreen.writeTextEnd(); }

        [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept
        {
            // 0: the parser hands every character to writeText(char32_t), which alone decides what fits on
            // the row.
            return 0;
        }
    };
} // namespace
// }}}

void Terminal::writeToScreenInternal(Screen& screen, std::string_view vtStream)
{
    auto sequenceHandler = SimpleSequenceHandler { screen };
    auto sequenceBuilder = SequenceBuilder { sequenceHandler, NoOpInstructionCounter() };
    // auto sequenceBuilder = SequenceBuilder { *this, NoOpInstructionCounter() };
    auto parser = vtparser::Parser { sequenceBuilder };

    parser.parseFragment(vtStream);
}

void Terminal::updateCursorVisibilityState() const noexcept
{
    if (_settings.cursorDisplay == CursorDisplay::Steady)
        return;

    auto const now = currentTime();
    auto const interval = cursorBlinkInterval();
    auto word = _cursorBlink.load(std::memory_order_relaxed);
    while (true)
    {
        auto const blink = CursorBlink::unpacked(word);
        if (now - blink.since < interval)
            return;

        auto const next =
            CursorBlink { .since = now,
                          .phase = blink.phase == CursorBlinkPhase::Shown ? CursorBlinkPhase::Hidden
                                                                          : CursorBlinkPhase::Shown };
        // Only over the word it read: a key that restarted the blink meanwhile changed it, and wins. The
        // failed exchange reloads the word, which then shows the phase too young to end.
        if (_cursorBlink.compare_exchange_weak(word, next.packed(), std::memory_order_relaxed))
            return;
    }
}

HyperlinkId Terminal::hyperlinkIdUnderPointer() const
{
    return hyperlinkIdUnderPointer(currentMouseGridPosition());
}

HyperlinkId Terminal::hyperlinkIdUnderPointer(std::optional<CellLocation> pointer) const
{
    return pointer ? displayedPage().hyperlinkIdAt(*pointer) : HyperlinkId {};
}

DetectedLink const* Terminal::detectedLinkAt(CellLocation gridPosition) const
{
    // The page on display, which is what the index describes and what the fill draws -- not the cursor's.
    auto const* index = detectedLinks();
    if (!index)
        return nullptr;
    auto const spans = currentSpansOn(*index, displayedPage().grid(), gridPosition.line);
    auto const span =
        std::ranges::find_if(spans, [column = gridPosition.column](LinkRowSpan const& candidate) {
            return candidate.first <= column && column <= candidate.last;
        });
    return span != spans.end() ? index->find(span->link) : nullptr;
}

LinkId Terminal::detectedLinkIdUnderPointer(std::optional<CellLocation> pointer) const
{
    auto const* link = pointer ? detectedLinkAt(*pointer) : nullptr;
    return link ? link->id : LinkId {};
}

void Terminal::refreshHoveredLink(std::optional<CellLocation> pointer)
{
    _hoveringLinkId = detectedLinkIdUnderPointer(pointer);
}

namespace
{
    /// The rows of the page on display from a vi link motion's cursor on, in its direction of travel, as one
    /// key press has seen them: those the index answers for, and those the key press scanned itself.
    class LinkMotionRows
    {
      public:
        /// @param grid The page on display's grid.
        /// @param from The row the motion starts on.
        /// @param direction Which way it goes.
        LinkMotionRows(Grid const& grid, LineOffset from, LinkDirection direction):
            _grid { grid },
            _direction { direction },
            _top { -boxed_cast<LineOffset>(grid.historyLineCount()) },
            _bottom { boxed_cast<LineOffset>(grid.pageSize().lines) - 1 },
            _walked { from },
            _readEnd { from }
        {
        }

        /// @return The last row in the direction of travel.
        [[nodiscard]] LineOffset edge() const noexcept { return isForward() ? _bottom : _top; }

        /// The first row up to @p end that the index does not answer for and this key press has not scanned:
        /// what lies there may be a link the index does not know. Resumes where the last call stopped: a scan
        /// only adds rows answered for or scanned within one key press, under the lock.
        /// @param index The links detected on the page on display; null for none.
        /// @param end The last row to look at, in the direction of travel.
        /// @return The row; nullopt when every row up to @p end is answered for.
        [[nodiscard]] std::optional<LineOffset> firstUnread(LinkIndex const* index, LineOffset end)
        {
            auto const rows = unbox(isForward() ? end - _walked : _walked - end) + 1;
            for (auto const offset: std::views::iota(0, std::max(rows, 0)))
            {
                auto const line = isForward() ? _walked + LineOffset(offset) : _walked - LineOffset(offset);
                if (!isRead(line) && (index == nullptr || !isScannedAsItIs(*index, _grid, line)))
                {
                    _walked = line;
                    return line;
                }
            }
            if (rows > 0)
                _walked = isForward() ? end + LineOffset(1) : end - LineOffset(1);
            return std::nullopt;
        }

        /// @return The page to scan from @p hole on, in the direction of travel, within the grid: first,
        /// last.
        [[nodiscard]] std::pair<LineOffset, LineOffset> pageFrom(LineOffset hole) const noexcept
        {
            auto const span = boxed_cast<LineOffset>(_grid.pageSize().lines) - 1;
            return isForward() ? std::pair { hole, std::min(_bottom, hole + span) }
                               : std::pair { std::max(_top, hole - span), hole };
        }

        /// Notes rows [@p first, @p last] as scanned by this key press: a row the index still does not hold
        /// as it is after its scan (a scan dropped as stale) is not scanned twice.
        void markRead(LineOffset first, LineOffset last) noexcept
        {
            _readEnd = isForward() ? std::max(_readEnd, last + LineOffset(1))
                                   : std::min(_readEnd, first - LineOffset(1));
        }

      private:
        [[nodiscard]] bool isForward() const noexcept { return _direction == LinkDirection::Forward; }

        [[nodiscard]] bool isRead(LineOffset line) const noexcept
        {
            return isForward() ? line < _readEnd : line > _readEnd;
        }

        Grid const& _grid;
        LinkDirection _direction;
        LineOffset _top;
        LineOffset _bottom;
        LineOffset _walked;  ///< The rows from the cursor's up to here are answered for or scanned.
        LineOffset _readEnd; ///< The rows from the cursor's that this key press scanned end before here.
    };
} // namespace

LinkScanBudget Terminal::linkScanBudget() const noexcept
{
    return LinkScanBudget { .pages = _linkDetector ? _linkDetector->services().tuning.motionSearchPages : 0 };
}

std::optional<CellLocation> Terminal::linkStartFrom(CellLocation from,
                                                    LinkDirection direction,
                                                    LinkScanBudget& budget)
{
    if (!_linkDetector)
        return std::nullopt;

    // The page on display, which is what the index describes, as detectedLinkAt() reads it.
    auto& grid = displayedPage().grid();
    auto const step = [direction](LinkIndex const& index, int64_t row, ColumnOffset column) {
        return direction == LinkDirection::Forward ? index.next(row, column) : index.previous(row, column);
    };

    // A path confirmed, or a background scan finished, since the last frame counts at once.
    auto indexChange = _linkDetector->adoptFinished(_linkSource);
    _linkSource.beginFrame();

    auto rows = LinkMotionRows { grid, from.line, direction };
    auto found = std::optional<CellLocation> {};
    auto searchFrom = from; // where the index walk resumes, past a link this key press could not confirm
    while (true)
    {
        auto const* index = detectedLinks();
        auto const* link =
            index ? step(*index, grid.stableLineIdOf(searchFrom.line), searchFrom.column) : nullptr;
        auto const head = link ? grid.lineOffsetOf(link->spans.front().row) : std::nullopt;
        // Every row up to the index's answer must be answered for, and without one, every row to the edge.
        if (auto const hole = rows.firstUnread(index, head.value_or(rows.edge())))
        {
            if (budget.pages <= 0)
                break;
            auto const [first, last] = rows.pageFrom(*hole);
            auto const scan = _linkDetector->scanNow(_linkSource, first, last);
            if (scan.index == LinkIndexChange::Changed)
                indexChange = LinkIndexChange::Changed;
            if (scan.linesRead > 0)
                --budget.pages;
            rows.markRead(first, last);
            continue;
        }
        if (!head)
            break; // no link to the edge
        auto const start = CellLocation { .line = *head, .column = link->spans.front().first };
        if (detectedLinkAt(start) == link)
        {
            found = start;
            break;
        }
        // Scanned by this key press, and still not indexed as it is now: passed over.
        searchFrom = start;
    }
    // A link may have appeared under a pointer that has not moved, as after a frame that adopts one.
    if (indexChange == LinkIndexChange::Changed)
        refreshHoveredLink(currentMouseGridPosition());
    return found;
}

void Terminal::followLinkAt(CellLocation gridPosition)
{
    runOutsideStateLock([this, gridPosition] { _eventListener.followLinkAt(gridPosition); });
}

void Terminal::updateHoveringHyperlinkState()
{
    // The cell the hover tooltip reads too (@see tryGetHoveringHyperlink), so the two agree. Translated once,
    // for both kinds of link.
    auto const pointer = currentMouseGridPosition();
    auto const newState = hyperlinkIdUnderPointer(pointer);

    auto const oldState = _hoveringHyperlinkId.exchange(newState);
    auto const oldLink = _hoveringLinkId.load();
    refreshHoveredLink(pointer);

    if (newState != oldState || _hoveringLinkId.load() != oldLink)
        renderBufferUpdated();
}

optional<chrono::milliseconds> Terminal::nextRender() const
{
    auto const now = currentTime();
    auto nextWakeup = chrono::milliseconds::max();

    // Cursor blink scheduling
    if (isModeEnabled(DECMode::VisibleCursor) && _settings.cursorDisplay == CursorDisplay::Blink)
    {
        // Read once: the frontend may set it meanwhile.
        auto const interval = cursorBlinkInterval();
        auto const passedCursor = chrono::duration_cast<chrono::milliseconds>(now - lastCursorBlink());
        if (passedCursor <= interval)
            nextWakeup = std::min(nextWakeup, interval - passedCursor);
    }

    // Cell blink scheduling
    if (isBlinkOnScreen())
    {
        if (_settings.blinkStyle == BlinkStyle::Classic)
        {
            // Classic mode only needs redraws at toggle transitions.
            nextWakeup = std::min(nextWakeup, _slowBlinker.nextToggleIn(now));
            nextWakeup = std::min(nextWakeup, _rapidBlinker.nextToggleIn(now));
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
            chrono::time_point_cast<chrono::seconds>(_clocks.wall.now()).time_since_epoch().count() % 60;
        auto const millisUntilNextMinute =
            chrono::duration_cast<chrono::milliseconds>(chrono::seconds(60 - currentSecond));
        nextWakeup = std::min(nextWakeup, millisUntilNextMinute);

        // While synchronized output suppresses rendering the status line is not rebuilt, so an overdue
        // command would keep reporting 0 ms and spin the display; the end of the sync triggers a refresh.
        if (_statusLineCommands && !isRenderingSuppressed())
            if (auto const due = _statusLineCommands->nextDue(now))
                nextWakeup = std::min(nextWakeup, *due);
    }

    // Screen transition and cursor motion animation scheduling at display refresh rate, from what the fill
    // published: their state is the fill's, under the state lock this does not take.
    if (now < _animationsEnd.load(std::memory_order_relaxed))
        nextWakeup = std::min(nextWakeup, _refreshInterval.get().value);

    // Momentum scrolling animation scheduling at display refresh rate.
    if (_isGliding.load(std::memory_order_relaxed))
        nextWakeup = std::min(nextWakeup, _refreshInterval.get().value);

    // Link detection's next scan, as the last frame published it: the detector is the fill's, under the state
    // lock this does not take. Not while synchronized output suppresses the fill, for the reason the status
    // line's commands give.
    if (auto const due = _linkScanDue.load(std::memory_order_relaxed);
        due != chrono::steady_clock::time_point::max() && !isRenderingSuppressed())
        nextWakeup = std::min(
            nextWakeup,
            chrono::ceil<chrono::milliseconds>(std::max(due - now, chrono::steady_clock::duration::zero())));

    if (nextWakeup == chrono::milliseconds::max())
        return nullopt;

    return nextWakeup;
}

void Terminal::tick(chrono::steady_clock::time_point now) noexcept
{
    auto const changes = _changes.exchange(0);
    (void) changes;
    // TODO: update render buffer if  (changes != 0)

    _currentTime.store(now, std::memory_order_relaxed);
    updateCursorVisibilityState();

    // Apply momentum scrolling each frame. The glide moves the view, which the parser thread and the GUI
    // thread move under the state lock, so it takes that lock too: only while one is armed, as a frame
    // without one should not wait for a parse. A frame with one waits for that lock anyway, to build the
    // moved view.
    if (_isGliding.load(std::memory_order_relaxed))
    {
        auto const _ = std::scoped_lock { _ownStateLock };
        advanceMomentumGlide(now);
    }
}

void Terminal::advanceMomentumGlide(chrono::steady_clock::time_point now)
{
    // Ended meanwhile by another thread, between the frame seeing the glide armed and taking the lock.
    if (!_scrollMomentum.active)
        return;

    auto const dt = chrono::duration<float>(now - _scrollMomentum.lastUpdate).count();
    if (dt <= 0.0f)
        return;

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
    _isGliding.store(false, std::memory_order_relaxed);
}

bool Terminal::isMomentumScrollActive() const noexcept
{
    return _isGliding.load(std::memory_order_relaxed);
}

SmoothScrollResult Terminal::injectWheelMomentum(float pixelDelta,
                                                 chrono::steady_clock::time_point now) noexcept
{
    // Gated on smoothScrolling only (independent of momentumScrolling, which governs touchpad
    // inertia). A page on display without history keeps the legacy line-based wheel path.
    if (!_settings.smoothScrolling || _viewport.scrollingDisabled())
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
    _isGliding.store(true, std::memory_order_relaxed);

    // Kick the render loop so tick() advances the glide even when the display is otherwise idle;
    // without this the display idles after the last input and momentum would only advance on the
    // next unrelated event (e.g. a mouse move).
    breakLoopAndRefreshRenderBuffer();
}

// }}} Momentum scrolling

SmoothScrollResult Terminal::applySmoothScrollPixelDelta(float pixelDelta)
{
    if (!_settings.smoothScrolling || _viewport.scrollingDisabled())
        return SmoothScrollResult::Disabled;

    auto const cellHeight = static_cast<float>(_cellPixelSize.height.as<int>());
    if (cellHeight <= 0.0f)
        return SmoothScrollResult::InvalidCellSize;

    // Compute the total pixel position and decompose into whole-line scroll offset + sub-line remainder.
    // This avoids calling scrollUp/scrollDown in a loop (which triggers intermediate viewport change
    // notifications and causes visual glitches).
    auto const totalPixels = _viewport.pixelOffset() + pixelDelta;
    auto const linesDelta = static_cast<int>(std::floor(totalPixels / cellHeight));
    auto const remainder = totalPixels - (static_cast<float>(linesDelta) * cellHeight);

    // The bound the viewport itself enforces, NOT the raw history depth: a collapsed fold takes its
    // rows out of the scrollable range, and clamping to the larger number would compute offsets
    // scrollTo() then rejects -- leaving the scroll offset frozen while the sub-cell remainder below
    // went on cycling through a whole cell height every frame, which is visible as a flicker.
    auto const maxOffset = boxed_cast<ScrollOffset>(_viewport.scrollableLineCount());
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
    ZoneScoped;
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
    publishAnimationsEnd();

    // Reset smooth scroll state on resize.
    resetSmoothScroll();

    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.
    auto const mainDisplayPageSize = totalPageSize - statusLineHeight();

    // What the grid had, rather than what _settings.pageSize less the status line says: a status line just
    // shown or hidden has already changed statusLineHeight() by the time it resizes through here.
    auto const gridPageSizeBefore = currentScreen().grid().pageSize();

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

    // Reset the margins to their defaults on resize.
    _mainDisplayMargin = makeDefaultMargin(mainDisplayPageSize);

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

    // clampToScreen keeps the cursor on the page but knows nothing about folds, so the page-delta
    // shift above can land it inside a collapsed block, where it renders as nothing at all.
    snapViCursorOutOfFolds();

    // Not for a call that only brings new pixel sizes: no row moved.
    if (currentScreen().grid().pageSize() != gridPageSizeBefore)
        _eventListener.pageResized();

    reportInBandWindowResize();
    resizeStatusLineExtensions();

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
#ifdef CONTOUR_VERIFY_STATE
    auto const thePageSize = _settings.pageSize;
    Require(*_currentMousePosition.column < *thePageSize.columns);
    Require(*_currentMousePosition.line < *thePageSize.lines);

    Require(_hostWritableStatusLineScreen.pageSize().lines == _indicatorStatusScreen.pageSize().lines);
    Require(_hostWritableStatusLineScreen.pageSize().lines == StatusLineHeight);
    Require(_hostWritableStatusLineScreen.pageSize().columns == _settings.pageSize.columns);

    // TODO: the current main display's page size PLUS visible status line count must match total page size.

    Require(_hostWritableStatusLineScreen.grid().pageSize().columns == _settings.pageSize.columns);
    // At least the page's: the gutter's columns too, as the gutter's settings stood when it was last laid
    // out.
    Require(_indicatorStatusScreen.grid().pageSize().columns >= _settings.pageSize.columns);

    Require(_tabs.empty() || _tabs.back() < unbox<ColumnOffset>(_settings.pageSize.columns));

    _currentScreen->verifyState();

    // Margins are terminal state, so verify them against the terminal's OWN main-display page size —
    // the exact basis resizeScreen() fills them with. pageSize() would compare against the PTY's page
    // size instead, which freezes once the PTY closes (UnixPty::resizeScreen early-returns without a
    // master fd), so any grid change after the shell exited (DPR settlement, window growth, pane
    // teardown reflows) made the two legitimately diverge and aborted debug builds.
    auto const mainDisplayPageSize = _settings.pageSize - statusLineHeight();
    Require(0 <= *mainDisplayMargin().horizontal.from
            && *mainDisplayMargin().horizontal.to < *mainDisplayPageSize.columns);
    Require(0 <= *mainDisplayMargin().vertical.from
            && *mainDisplayMargin().vertical.to < *mainDisplayPageSize.lines);
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

void Terminal::setExtendedWordDelimiters(string const& wordDelimiters)
{
    _settings.extendedWordDelimiters = unicode::from_utf8(wordDelimiters);
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
            // codepoint is 0, or vertically, where a tall block reaches down into the row -- and nor does
            // the padding a wide character left at the end of a row. Without this they fall into the
            // empty-cell branch below and copy as spaces: 中a would yield "中 a", a six-column text-sizing
            // block would trail five spaces, and a word a wide character wrapped early would be split.
            if (cell.flags().any(NoTextCellFlags))
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

    auto const& screen = displayedPage();
    auto se = SelectionRenderer { *this, pageSize().columns.as<ColumnOffset>() - 1 };
    vtbackend::renderSelection(*_selection, [&](CellLocation pos) { se(pos, screen.at(pos)); });
    return se.finish();
}

void Terminal::selectAll()
{
    selectLines(-boxed_cast<LineOffset>(displayedPage().grid().historyLineCount()),
                boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1));
}

void Terminal::selectLines(LineOffset top, LineOffset bottom)
{
    auto const from = CellLocation { .line = top, .column = ColumnOffset(0) };
    auto const to = CellLocation { .line = bottom,
                                   .column = boxed_cast<ColumnOffset>(pageSize().columns) - ColumnOffset(1) };

    // FullLineSelection rather than LinearSelection: it normalizes the columns to whole lines and follows
    // wrapped ones, which is what "all" and "this block" mean (and what ViMode's VisualLine already does).
    selectThrough(std::make_unique<FullLineSelection>(_selectionHelper, from, selectionUpdatedHelper()), to);
}

void Terminal::selectThrough(std::unique_ptr<Selection> selector, CellLocation to)
{
    setSelector(std::move(selector));
    (void) _selection->extend(to);

    // Completing a selection is Insert mode's business — exactly the gate sendMouseReleaseEvent() applies
    // to a finished drag. In a Visual mode the Vi layer owns the selection and every motion extends it
    // (ViCommands::moveCursorTo → Selection::extend, whose first statement is an assert that the state is
    // not Complete), so handing it a completed one aborts on the next keystroke.
    if (_inputHandler.mode() == ViMode::Insert)
        _selection->complete();
    else
    {
        // The next motion moves the vi cursor and extends the selection to it, so the cursor stands where the
        // selection ends, as it would had the motions made it.
        _viCommands.cursorPosition = to;
        _viewport.makeVisible(to.line);
    }

    // Deliberately NOT updateSelectionMatches(): that serializes the selection into a search pattern to
    // highlight the other occurrences of a selected WORD. For a selection that spans many lines -- the whole
    // scrollback, or a block's long output -- the pattern is megabytes built under the terminal lock, for a
    // search whose only match is what is already selected, and which then disables the trivial-line render
    // fast path for every frame that follows. The quadruple-click full-line selection skips it for the same
    // reason.
    onSelectionUpdated();
}

// {{{ Output folding
void Terminal::refreshFoldState() const
{
    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    auto const floor = grid.stableRangeFloor();

    // The answer on all but the calls that follow a reflow or an eviction, and this fronts the
    // per-cell coordinate translation -- so what runs here is the guard, not the reconciliation.
    if (generation == _foldStateGeneration && _foldStateFloor == floor)
        return;

    // A bump means a reflow destroyed row identity wholesale, so every id held names a different row,
    // or none. There is nothing to salvage and guessing would collapse the wrong block.
    //
    // stableIdGeneration(), NOT generation(): the latter also bumps for a pure line-count resize, which
    // every row survives with the id it had -- so keying on it dropped every collapsed fold the moment
    // a status line appeared or the window grew a row taller.
    if (generation != _foldStateGeneration)
    {
        _foldState.clear();
        _foldStateGeneration = generation;
    }

    // Heads that scrolled out of the scrollback: without pruning the set grows for the life of the
    // session, and an id the grid later hands out again would come back collapsed.
    _foldState.prune(floor);
    _foldStateFloor = floor;
}

std::optional<FoldRange> Terminal::foldContaining(LineOffset gridLine) const
{
    // A query about the page on display -- the gutter draws from it, and a click or a context menu
    // acts on the row the user pointed at. The same predicate the projection and the render pass use,
    // so a page whose rows were folded can be asked which fold folded them.
    if (!foldingAppliesToDisplayedPage())
        return std::nullopt;

    return foldCovering(foldRanges(), primaryScreen().grid().stableLineIdOf(gridLine));
}

std::optional<FoldRange> Terminal::foldCovering(std::span<FoldRange const> ranges, int64_t id) noexcept
{
    // Ranges come back most recent first, which is DESCENDING by head id -- the walk that produced them
    // climbs the scrollback. So the search runs against that order rather than sorting a copy per row.
    //
    // A range spans [headStableId, lastStableId], the head being the smallest of the three, so the first
    // range whose head is at or below @p id is the only one that can cover it: ranges do not nest.
    auto const it = std::ranges::lower_bound(ranges, id, std::greater {}, &FoldRange::headStableId);
    if (it == ranges.end() || id > it->lastStableId)
        return std::nullopt;

    return *it;
}

FoldMarker Terminal::markerIn(FoldRange const& range, int64_t id) const noexcept
{
    auto const collapsed = _foldState.isCollapsed(range.headStableId);

    if (id == range.headStableId)
        return collapsed ? FoldMarker::Collapsed : FoldMarker::Expanded;

    // A collapsed block draws nothing below its head, because there IS nothing below it -- those rows
    // are the ones it hides.
    if (collapsed)
        return FoldMarker::None;

    return id == range.lastStableId ? FoldMarker::BodyEnd : FoldMarker::Body;
}

FoldMarker Terminal::foldMarkerAt(LineOffset gridLine) const
{
    auto const range = foldContaining(gridLine);
    if (!range)
        return FoldMarker::None;

    return markerIn(*range, primaryScreen().grid().stableLineIdOf(gridLine));
}

void Terminal::snapViCursorOutOfFolds()
{
    // Motions cannot put the Vi cursor on a hidden line -- they count VISIBLE lines. Collapsing a
    // block the cursor is standing in can, and a cursor on a hidden line renders as nothing at all.
    if (_inputHandler.mode() == ViMode::Insert)
        return;

    _viCommands.cursorPosition.line =
        snapToVisibleLine(_viCommands.cursorPosition.line, VerticalDirection::Down);
}

void Terminal::onFoldStateChanged()
{
    // The scrollable range is the history less what collapsed folds hide, so collapsing one can leave
    // the viewport parked above the new top -- where scrollTo() rejects every request and the viewport
    // can no longer be scrolled away from. Expanding only ever grows the range, which makes this a
    // no-op, so both directions share the one funnel.
    //
    // Not called from onViewportChanged(): _modified() lands there, and a clamp issued from inside it
    // would re-enter.
    _viewport.clampScrollOffset();
    snapViCursorOutOfFolds();
}

bool Terminal::toggleFoldContaining(LineOffset gridLine)
{
    if (!isFoldingAvailable())
        return false;

    auto const range = foldContaining(gridLine);
    if (!range)
        return false;

    _foldState.toggle(range->headStableId);
    onFoldStateChanged();
    return true;
}

BlockColumnPress Terminal::blockColumnPressAt(LineOffset gridLine) const
{
    if (!foldingAppliesToDisplayedPage())
        return BlockColumnPress::Swallow;

    // The fold looked up once, for the toggle and the glyph alike: foldContaining() without its page check,
    // already made above.
    auto const id = primaryScreen().grid().stableLineIdOf(gridLine);
    auto const range = foldCovering(foldRanges(), id);
    auto const& gutter = _settings.gutter;
    auto const fold = isFoldingAvailable() && gutter.foldMarkers && range.has_value()
                          ? FoldToggle::Available
                          : FoldToggle::Unavailable;
    return blockColumnPressFor(blockColumnGlyph(blockColumnFactsAt(gridLine, id, range), gutter), fold);
}

bool Terminal::shellIsPrompting() const noexcept
{
    auto const* const current = commandBlocks().current();
    return isPrimaryScreen() && current != nullptr && current->state == CommandBlockState::Prompting;
}

BlockColumnFacts Terminal::blockColumnFactsAt(LineOffset gridLine) const
{
    auto const id = primaryScreen().grid().stableLineIdOf(gridLine);
    return blockColumnFactsAt(gridLine, id, foldCovering(foldRanges(), id));
}

BlockColumnFacts Terminal::blockColumnFactsAt(LineOffset gridLine,
                                              int64_t id,
                                              std::optional<FoldRange> const& range) const
{
    auto const& grid = primaryScreen().grid();
    auto const* const record = commandBlockAt(gridLine);
    return BlockColumnFacts {
        .fold = range ? markerIn(*range, id) : FoldMarker::None,
        .head = blockHeadStateOf(record, id, grid.stableIdGeneration()),
        .mark = grid.lineAt(gridLine).isFlagEnabled(LineFlag::UserMark) ? UserMarkPresence::Present
                                                                        : UserMarkPresence::Absent,
        .outcome = record != nullptr && _settings.gutter.exitStatus ? std::optional { outcomeOf(*record) }
                                                                    : std::nullopt,
    };
}

bool Terminal::expandFoldContaining(LineOffset gridLine)
{
    auto const range = foldContaining(gridLine);
    if (!range || !_foldState.isCollapsed(range->headStableId))
        return false;

    _foldState.expand(range->headStableId);
    onFoldStateChanged();
    return true;
}

void Terminal::collapseFoldContaining(LineOffset gridLine)
{
    if (!isFoldingAvailable())
        return;

    auto const range = foldContaining(gridLine);
    if (!range || _foldState.isCollapsed(range->headStableId))
        return;

    _foldState.collapse(range->headStableId);
    onFoldStateChanged();
}

bool Terminal::toggleLastFold()
{
    if (!isFoldingAvailable() || !foldingAppliesToDisplayedPage())
        return false;

    auto const ranges = foldRanges();
    if (ranges.empty())
        return false;

    // Most recent first, so the newest finished command is the front one.
    _foldState.toggle(ranges.front().headStableId);
    onFoldStateChanged();
    return true;
}

bool Terminal::collapseLastFold()
{
    // Like every fold verb a user reaches, a verb about the page on display: in vim, the primary screen
    // and its folds are out of sight, and folding them away there would hide the output the user returns
    // to.
    return isFoldingAvailable() && foldingAppliesToDisplayedPage() && collapseNewestFold();
}

bool Terminal::collapseNewestFold()
{
    auto const ranges = foldRanges();
    if (ranges.empty() || _foldState.isCollapsed(ranges.front().headStableId))
        return false;

    _foldState.collapse(ranges.front().headStableId);
    onFoldStateChanged();
    return true;
}

bool Terminal::collapseAllFolds()
{
    if (!isFoldingAvailable() || !foldingAppliesToDisplayedPage())
        return false;

    auto const revision = _foldState.revision();
    _foldState.collapseAll(foldRanges());
    if (_foldState.revision() == revision)
        return false;

    onFoldStateChanged();
    return true;
}

bool Terminal::expandAllFolds()
{
    if (!foldingAppliesToDisplayedPage())
        return false;

    auto const revision = _foldState.revision();
    _foldState.expandAll();
    if (_foldState.revision() == revision)
        return false;

    onFoldStateChanged();
    return true;
}

LineOffset Terminal::drawnRowLimit() const
{
    // How far down the page the projection actually reaches, which is NOT always a page-full: collapsed
    // folds hiding more rows than the history can backfill leave it short, and the rest of the page is
    // blank. Mapping a blank row anyway extrapolates it onto the last visible line -- every one of them
    // onto the SAME line -- so each would be handed that line's gutter, a column of duplicates hanging
    // below content Grid::render draws nothing on.
    //
    // A count rather than the span it comes from, because the callers' loops re-enter the cache that span
    // points into. An empty projection means nothing is collapsed and the whole page is drawn.
    //
    // The MAIN page, not the total: _settings.pageSize includes the status line, and nothing drawn here
    // belongs beside it.
    auto const pageLines = boxed_cast<LineOffset>(pageSize().lines);
    auto const projectedRows = foldProjection().size();
    return projectedRows == 0
               ? pageLines
               : std::min(pageLines, foldProjectionTopRow() + LineOffset::cast_from(projectedRows));
}

CellLocation Terminal::gridCursorPosition() const noexcept
{
    if (_inputHandler.mode() == ViMode::Insert)
        return currentScreen().cursor().position;
    return _viCommands.cursorPosition;
}

void Terminal::fillGutter(RenderBuffer& output, LineOffset baseLine) const
{
    // The same predicate the projection and the render pass use. On every other page -- the alternate
    // screen of vim, less or htop among them -- the strip stays reserved, its width being the
    // configuration's and never the content's, and stays empty: those rows have no history to number, no
    // birth time worth showing and no command blocks. Asked first, so those frames build no layout at all.
    if (!foldingAppliesToDisplayedPage())
        return;

    // Built per frame rather than cached: Settings is mutable through settings(), and the layout formats a
    // sample only when timestamps are on.
    auto const layout = gutterLayoutFor(_settings.gutter);
    if (layout.totalColumns == 0)
        return;

    auto const& gutter = _settings.gutter;
    auto const& grid = primaryScreen().grid();
    auto const textColors = _colorPalette.gutterTextColors();

    // Absolute numbers count every row the session ever had, the evicted ones included; the distance counts
    // VISIBLE rows, so a collapsed fold is one row -- what `j` and `k` count.
    //
    // The drawn rows are consecutive visible rows, so row y is the top row's distance plus y: measured once
    // per frame, which with folds collapsed is a walk of the hidden runs, and not at all in absolute mode,
    // which never reads it.
    auto const numbers = layout.segment(GutterSegmentKind::LineNumber);
    auto const topRowDistance =
        numbers && gutter.lineNumbers != LineNumberMode::Absolute
            ? visibleDistance(gridCursorPosition().line, _viewport.translateScreenToGridLine(LineOffset(0)))
            : 0;
    auto const lineNumberOf = [&](LineOffset gridLine, int screenRow) {
        if (!numbers)
            return LineNumberText {};
        auto const absolute = grid.evictedRowCount()
                              + static_cast<uint64_t>(unbox<int64_t>(gridLine - grid.addressableTop())) + 1;
        return lineNumberText(lineNumberFor(gutter.lineNumbers, absolute, topRowDistance + screenRow),
                              numbers->columns);
    };

    // Only with the segment present, in the format the layout already resolved. A run spells the rows top
    // to bottom, so it is asked once per drawn row, in order.
    auto timestamps = layout.segment(GutterSegmentKind::Timestamp)
                          ? std::optional { TimestampRun { layout.timestampFormat, _clocks.localTime } }
                          : std::nullopt;
    auto const timestampOf = [&](Line const& line) {
        if (!timestamps)
            return std::string {};
        return timestamps->next(lineBirthTime(line));
    };

    // The whole run of the hovered fold lights up, not the one row under the pointer: that run is what a
    // click acts on, so it is what the highlight has to describe.
    auto const hoveredHead =
        _gutterHoverLine ? foldCovering(foldRanges(), grid.stableLineIdOf(*_gutterHoverLine)) : std::nullopt;

    // Shifted by baseLine for the same reason every cell is: a status line at the top moves the main page
    // down, and a gutter left where it was would point at the wrong row.
    for (auto const y: std::views::iota(0, unbox<int>(drawnRowLimit())))
    {
        auto const screenRow = LineOffset(y);
        auto const gridLine = _viewport.translateScreenToGridLine(screenRow);
        auto const& line = grid.lineAt(gridLine);

        // Asked for per row rather than held across the loop: foldRanges() hands back a span over a
        // CACHE, and the translation above re-enters that cache once per row. Holding it survives only as
        // long as no key input can move inside a frame -- one that could, and the remaining rows would be
        // reading freed memory. A cache hit here is three integer comparisons. Asked once, for the hover
        // and the glyph alike.
        auto const id = grid.stableLineIdOf(gridLine);
        auto const range = foldCovering(foldRanges(), id);

        auto const facts = blockColumnFactsAt(gridLine, id, range);
        auto const glyph = blockColumnGlyph(facts, gutter);
        auto const& outcome = facts.outcome;

        auto const hover = hoveredHead && range && hoveredHead->headStableId == range->headStableId
                               ? GutterHover::Hovered
                               : GutterHover::Rest;

        appendGutterCells(
            layout,
            GutterRow { .screenLine = baseLine + screenRow,
                        .timestamp = timestampOf(line),
                        .lineNumber = lineNumberOf(gridLine, y),
                        .glyph = glyph,
                        // Mixed only for a glyph there is: most rows have none.
                        .glyphColors = glyph == BlockColumnGlyph::None
                                           ? RGBColorPair {}
                                           : blockColumnColors(_colorPalette, glyph, outcome, hover),
                        .weight = foldWeightOf(facts, gutter, hover) },
            textColors,
            output.gutter);
    }
}

void Terminal::fillFoldLabels(RenderBuffer& output, LineOffset baseLine) const
{
    auto const attributes = gutterCellAttributes(_colorPalette.gutterTextColors());
    for (auto const& placement: foldLabelPlacements())
        layoutFoldLabel(output.annotations,
                        baseLine + _viewport.translateGridToScreenCoordinate(placement.gridLine),
                        placement.textColumns,
                        pageSize().columns,
                        placement.label,
                        attributes);
}

std::optional<LineOffset> Terminal::collapsedFoldLabelAt(CellLocation position) const
{
    for (auto const& placement: foldLabelPlacements())
    {
        auto const extent = foldLabelExtent(_viewport.translateGridToScreenCoordinate(placement.gridLine),
                                            placement.textColumns,
                                            pageSize().columns,
                                            placement.label.size());
        if (extent && extent->contains(position))
            return primaryScreen().grid().lineOffsetOf(placement.range.headStableId);
    }
    return std::nullopt;
}

std::vector<Terminal::FoldLabelPlacement> Terminal::foldLabelPlacements() const
{
    // Only a COLLAPSED fold is labelled, and nothing is collapsed in the common case -- which then costs
    // one comparison. Independent of the gutter: with the fold column hidden, the label is the only sign
    // left that output was folded away.
    if (!foldingAppliesToDisplayedPage() || _foldState.empty())
        return {};

    // The few collapsed folds, rather than every drawn row: the rows are what is expensive to translate while
    // anything is collapsed. Copied out of foldRanges(), whose span points into a cache the translations
    // below re-enter.
    auto const collapsed =
        std::ranges::to<std::vector>(foldRanges() | std::views::filter([this](FoldRange const& range) {
                                         return _foldState.isCollapsed(range.headStableId);
                                     }));

    auto const& grid = primaryScreen().grid();
    auto const pageLines = unbox<int>(pageSize().lines);
    auto const drawnRows = drawnRowLimit();

    auto placements = std::vector<FoldLabelPlacement> {};
    for (auto const& range: collapsed)
    {
        auto const head = grid.lineOffsetOf(range.headStableId);
        if (!head)
            continue;

        // The label follows the text where the head's LOGICAL line ends. A command line wider than the page
        // wraps, and its head row is then full to the edge, so the label belongs on the last row of that line
        // which is drawn: the last row of all, unless the viewport cuts the line short.
        auto const continuations = std::ranges::distance(
            std::views::iota(unbox<int>(*head) + 1, pageLines)
            | std::views::take_while([&](int row) { return grid.lineAt(LineOffset(row)).wrapped(); }));
        auto const bottomUp =
            std::views::iota(unbox<int>(*head), unbox<int>(*head) + static_cast<int>(continuations) + 1)
            | std::views::reverse;
        auto const isDrawn = [&](int row) {
            auto const screenRow = _viewport.translateGridToScreenCoordinate(LineOffset(row));
            return screenRow >= LineOffset(0) && screenRow < drawnRows
                   && _viewport.isLineVisible(LineOffset(row));
        };
        auto const lastDrawn = std::ranges::find_if(bottomUp, isDrawn);
        if (lastDrawn == std::ranges::end(bottomUp))
            continue;

        auto const gridLine = LineOffset(*lastDrawn);
        auto const hidden = static_cast<uint64_t>(range.lastStableId - range.firstStableId + 1);
        placements.push_back(FoldLabelPlacement { .range = range,
                                                  .gridLine = gridLine,
                                                  .textColumns = grid.lineAt(gridLine).trimmedColumns(),
                                                  .label = collapsedFoldLabel(hidden) });
    }
    return placements;
}

namespace
{
    /// The primary screen's rows with the block each line was stamped with (Line::blockId()): unlike
    /// AttributedBlockRows, no row is attributed through the store, so a row costs one read and never a
    /// scan -- what the sticky header's per-frame walk down a prompt may spend.
    class StampedBlockRows final: public CommandBlockRowSource
    {
      public:
        explicit StampedBlockRows(Grid const& grid) noexcept: _grid { grid } {}

        [[nodiscard]] LineOffset topRow() const override { return _grid.addressableTop(); }

        [[nodiscard]] LineOffset bottomRow() const override
        {
            return boxed_cast<LineOffset>(_grid.pageSize().lines) - LineOffset(1);
        }

        [[nodiscard]] CommandBlockId blockIdAt(LineOffset row) const override
        {
            return _grid.lineAt(row).blockId();
        }

        [[nodiscard]] LineFlags flagsAt(LineOffset row) const override { return _grid.lineAt(row).flags(); }

        [[nodiscard]] ColumnOffset commandEndColumnAt(LineOffset row) const override
        {
            return _grid.lineAt(row).commandEndOffset();
        }

      private:
        Grid const& _grid;
    };

    /// Lays @p text out on @p line so that it ends just short of @p end, appended to @p output -- what
    /// fits of it, when the row from column 0 to @p end is too narrow for all of it.
    ///
    /// Laid out from column 0 first, then moved right by the room left over: the width is read off the cells
    /// as laid out, so the rule that a zero-width codepoint joins its base is spelled once, in
    /// layoutRenderText().
    /// @param output Where the cells go.
    /// @param text The text.
    /// @param attributes Its colours.
    /// @param line The screen row.
    /// @param end The first column the text may not reach.
    /// @return The column its first cell starts at; @p end when nothing fits.
    ColumnOffset layoutRightAligned(std::vector<RenderCell>& output,
                                    std::u32string_view text,
                                    RenderAttributes const& attributes,
                                    LineOffset line,
                                    ColumnOffset end)
    {
        auto const first = output.size();
        auto const width = layoutRenderText(
            output, text, attributes, CellLocation { .line = line, .column = ColumnOffset(0) }, end);
        auto const shift = end - width;
        for (auto& cell: output | std::views::drop(static_cast<std::ptrdiff_t>(first)))
            cell.position.column += shift;
        return shift;
    }

    /// Where @p record's head is in @p grid, for the sticky header: the test the reveal a jump to the head
    /// performs (Terminal::revealStableLineAtTop()), split the same way.
    /// @param grid The primary screen's grid.
    /// @param record The block.
    /// @return The head row; HeadEvicted when the record was headed in this grid's generation and its row
    ///         has left the history since; HeadNotPlaced when it was headed in another generation -- a
    ///         daemon mirror's record before the rows that head it arrive -- which is not known here, and
    ///         not the same as gone.
    [[nodiscard]] std::expected<LineOffset, CommandBlockJumpError> locateHead(
        Grid const& grid, CommandBlockRecord const& record)
    {
        if (auto const head = headOffsetOf(grid, record))
            return *head;
        if (record.headIdGeneration == grid.stableIdGeneration())
            return std::unexpected { CommandBlockJumpError::HeadEvicted };
        return std::unexpected { CommandBlockJumpError::HeadNotPlaced };
    }

    /// Why a jump to a block's head failed, for a reveal of that head that was refused: the same reason, in
    /// the jump's words.
    /// @param error Why the reveal was refused.
    /// @return The jump's error.
    [[nodiscard]] CommandBlockJumpError jumpErrorOf(RevealError error) noexcept
    {
        switch (error)
        {
            case RevealError::Evicted: return CommandBlockJumpError::HeadEvicted;
            case RevealError::OtherGeneration: return CommandBlockJumpError::HeadNotPlaced;
            case RevealError::NotScrollable: return CommandBlockJumpError::NotScrollable;
        }
        core::unreachable();
    }
} // namespace

Terminal::GatheredStickyHeader Terminal::stickyHeaderInput() const
{
    auto gathered = GatheredStickyHeader {
        .input =
            StickyHeaderInput {
                .settings = _settings.stickyHeader,
                // The DISPLAYED page, as folding and the gutter use: with DECPCCM reset the cursor's may
                // differ.
                .screen = screenTypeFromPage(_displayedPage),
                .viewport = _viewport.scrolled() ? ViewportScroll::Scrolled : ViewportScroll::Live,
                .topRowAge = TopRowAge::Newer,
                .cursor = TopRowCursor::Elsewhere,
                .block = std::nullopt,
            },
        .record = nullptr,
        .inputLine = std::nullopt,
    };
    auto& input = gathered.input;

    // Nothing below describes a page that carries no blocks, and the viewport's translation would speak
    // for the wrong grid there.
    if (input.screen != ScreenType::Primary)
        return gathered;

    auto const& grid = primaryScreen().grid();
    auto const topLine = _viewport.topLine();
    // A row of the scrollback, and the oldest it keeps. The live page's first row is no top of a scrollback
    // even where nothing older survives -- with none kept -- so there the mode decides, as for any row.
    input.topRowAge =
        topLine < LineOffset(0) && topLine <= grid.addressableTop() ? TopRowAge::Oldest : TopRowAge::Newer;
    // Outside insert mode the cursor drawn is the vi-mode one, in grid coordinates; in it, the terminal's
    // own, on the live page -- where the page it is on is the one displayed.
    if (_inputHandler.mode() != ViMode::Insert)
        input.cursor =
            _viCommands.cursorPosition.line == topLine ? TopRowCursor::ViCursor : TopRowCursor::Elsewhere;
    else
        input.cursor = _cursorPage == _displayedPage && primaryScreen().cursor().position.line == topLine
                           ? TopRowCursor::TextCursor
                           : TopRowCursor::Elsewhere;

    // Where no block could get a header -- the commonest frame, live output under the default mode -- the
    // block is not worth looking up.
    if (!stickyHeaderPossible(input.settings, input.screen, input.viewport, input.topRowAge))
        return gathered;

    // The row's own block, resolved through the store -- not a walk up to its prompt.
    auto const* const record = commandBlockAt(topLine);
    if (record == nullptr)
        return gathered;

    // A block a ;C opened with no ;A before it never had a command row: none to copy, and none to have lost,
    // so neither the header nor the placeholder -- which would claim an eviction that never happened.
    if (!record->promptStartedAt)
        return gathered;

    auto facts = StickyHeaderBlockFacts {
        .id = record->id,
        .state = record->state,
        .head = StickyHeadPosition::Visible,
        .topRow = TopRowPart::Output,
    };
    auto const head = locateHead(grid, *record);
    if (!head)
    {
        // A head not placed yet is unknown here, not gone: no header, and no placeholder to call it evicted.
        if (head.error() == CommandBlockJumpError::HeadNotPlaced)
            return gathered;
        // The head scrolled out of the history, while the top row shows the block's rows outlived it.
        facts.head = StickyHeadPosition::Evicted;
    }
    else if (*head < topLine)
    {
        if (auto const inputLine = inputRowOf(StampedBlockRows { grid }, *head, record->id))
        {
            facts.head = StickyHeadPosition::Above;
            auto const lastRow = boxed_cast<LineOffset>(grid.pageSize().lines) - LineOffset(1);
            auto const inputEnd = grid.logicalLineEnd(
                *inputLine, std::min(*inputLine + LineOffset::cast_from(MaxPromptScanLines), lastRow));
            facts.topRow = topLine > inputEnd ? TopRowPart::Output : TopRowPart::Input;
            gathered.inputLine = inputLine;
        }
        else
        {
            // Headed at its output: the command row has left as surely as an evicted head has, and a copy of
            // the row that heads the block now would pass off output as the command.
            facts.head = StickyHeadPosition::Evicted;
        }
    }
    input.block = facts;
    gathered.record = record;
    return gathered;
}

std::expected<void, CommandBlockJumpError> Terminal::scrollToCommandBlockHead(CommandBlockId id)
{
    auto const* const record = commandBlocks().find(id);
    if (record == nullptr)
        return std::unexpected { CommandBlockJumpError::UnknownBlock };

    // The reveal a click on a scrollbar tick performs: it refuses a head placed in another generation, then
    // one the scrollback dropped, and opens the fold hanging off the head first.
    return revealStableLineAtTop(record->headStableId, record->headIdGeneration)
        .transform([&] {
            // The row the header copied -- the command's input -- rather than the head, which a prompt
            // opening with a blank line or a status row puts above it: the command is what the eye goes
            // looking for.
            auto const& grid = primaryScreen().grid();
            auto const head = headOffsetOf(grid, *record);
            auto const input = head ? inputRowOf(StampedBlockRows { grid }, *head, record->id) : std::nullopt;

            // Timed by the injected clock rather than currentTime(), which stands still between paints: a
            // flash stamped with the last paint's time could be over before the frame that shows it.
            _lineFlash.active = true;
            _lineFlash.startTime = _clocks.steady.now();
            _lineFlash.duration = LineFlashDuration;
            _lineFlash.stableId = input ? grid.stableLineIdOf(*input) : record->headStableId;
            _lineFlash.stableIdGeneration = record->headIdGeneration;
            _lineFlash.color = blockHighlightColor(
                _colorPalette,
                _settings.gutter.exitStatus ? std::optional { outcomeOf(*record) } : std::nullopt);
            publishAnimationsEnd();
            breakLoopAndRefreshRenderBuffer();
        })
        .transform_error(jumpErrorOf);
}

void Terminal::fillLineFlash(RenderBuffer& output, LineOffset baseLine)
{
    if (!_lineFlash.active)
        return;
    auto const progress = _lineFlash.progress(currentTime());
    if (progress >= 1.0f)
    {
        _lineFlash.active = false;
        return;
    }

    // On the primary page alone, where the row is, and only where it is drawn: scrolled out of view, folded
    // away or on another page, the flash fades unseen.
    auto const& grid = primaryScreen().grid();
    if (!foldingAppliesToDisplayedPage() || _lineFlash.stableIdGeneration != grid.stableIdGeneration())
        return;
    auto const line = grid.lineOffsetOf(_lineFlash.stableId);
    if (!line || !_viewport.isLineVisible(*line))
        return;
    auto const screenRow = _viewport.translateGridToScreenCoordinate(*line);
    if (screenRow < LineOffset(0) || screenRow >= drawnRowLimit())
        return;

    output.lineFlash = RenderLineFlash { .lineOffset = baseLine + screenRow,
                                         .color = _lineFlash.color,
                                         .alpha = LineFlashPeakAlpha * (1.0f - progress) };
}

void Terminal::fillStickyHeader(RenderBuffer& output, LineOffset baseLine) const
{
    auto const gathered = stickyHeaderInput();
    auto const decision = stickyHeaderFor(gathered.input);
    if (!decision)
        return;

    // A decision names the block it was handed, whose record the gathering kept.
    auto const* const record = gathered.record;

    auto& header = output.showStickyHeader();
    header.kind = decision->kind;
    header.block = decision->block;
    header.background = _colorPalette.stickyHeaderBackgroundColor();
    header.separator = _colorPalette.stickyHeaderSeparatorColor();

    // The page's own background, which the renderer leaves unpainted: the header's text sits on its opaque
    // band without a rectangle per cell repainting the band over the band.
    auto const background = _colorPalette.defaultBackground;

    // The chip first, as it decides how much room the text gets: right-aligned, StickyHeaderChipInset columns
    // short of the page's end (clear of the overlay scrollbar), and never pushed left of column 0 on a page
    // too narrow for it. The injected wall clock, so a test drives the running time.
    auto const statusColor = _colorPalette.blockStatusColor(outcomeOf(*record));
    auto const chipStart = layoutRightAligned(
        header.chip,
        stickyHeaderChip(*record, _clocks.wall.now()),
        RenderAttributes { .foregroundColor = statusColor,
                           .backgroundColor = background,
                           .decorationColor = statusColor,
                           .flags = {},
                           .lineFlags = LineFlag::None },
        baseLine,
        boxed_cast<ColumnOffset>(std::max(ColumnCount(0), pageSize().columns - StickyHeaderChipInset)));

    // One blank column between the text and the chip, so a long command never runs into it.
    auto const textColumns = std::max(ColumnCount(0), boxed_cast<ColumnCount>(chipStart) - ColumnCount(1));

    switch (decision->kind)
    {
        case StickyHeaderKind::Command: {
            // Decided only for a head above the viewport, whose input row the gathering found -- on the
            // primary screen, which is on display then, so the detected links are its own.
            auto const& grid = primaryScreen().grid();
            auto const* const index = detectedLinks();
            RenderBufferBuilder::renderDetachedLine(header.cells,
                                                    *this,
                                                    grid.lineAt(*gathered.inputLine),
                                                    index ? currentSpansOn(*index, grid, *gathered.inputLine)
                                                          : std::span<LinkRowSpan const> {},
                                                    baseLine,
                                                    textColumns);
            break;
        }
        case StickyHeaderKind::Evicted: {
            // No row of the block's prompt survives to be copied, so the record speaks for it: the command
            // line it kept, sanitised, in the gutter's own quieter text colour.
            auto const textColor = _colorPalette.gutterTextColor();
            layoutRenderText(header.cells,
                             evictedPlaceholderText(record->commandLine, textColumns),
                             RenderAttributes { .foregroundColor = textColor,
                                                .backgroundColor = background,
                                                .decorationColor = textColor,
                                                .flags = {},
                                                .lineFlags = LineFlag::None },
                             CellLocation { .line = baseLine, .column = ColumnOffset(0) },
                             boxed_cast<ColumnOffset>(textColumns));
            break;
        }
    }
}

void Terminal::setGutterHoverLine(std::optional<LineOffset> gridLine)
{
    if (_gutterHoverLine == gridLine)
        return;

    // What is DRAWN is the highlighted fold, not the hovered row, so the repaint is gated on the fold
    // changing rather than on the pointer crossing a row boundary. Dragging down a page of gutter
    // would otherwise force one full render-buffer rebuild -- and one reader wakeup -- per row.
    auto const headOf = [this](std::optional<LineOffset> line) -> std::optional<int64_t> {
        if (!line)
            return std::nullopt;
        auto const range = foldContaining(*line);
        return range ? std::optional { range->headStableId } : std::nullopt;
    };

    auto const before = headOf(_gutterHoverLine);
    _gutterHoverLine = gridLine;
    if (before == headOf(gridLine))
        return;

    // The highlight lives only in the render buffer, so nothing else would notice it changed: without
    // this the tint appears on the next frame something else happens to dirty, which for an idle
    // terminal is never.
    breakLoopAndRefreshRenderBuffer();
}

void Terminal::autoCollapseOnNewPrompt()
{
    if (!_settings.autoCollapseFoldOnNewCommand || !isFoldingAvailable())
        return;

    // The scan is driven off the marks, and the ;A that brought us here has already been stamped -- so
    // the block that just finished is the front one, exactly as it is for CollapseLastFold. Whichever page
    // is on display: the prompt belongs to the primary screen, and so does the block it finished.
    collapseNewestFold();
}

std::span<FoldRange const> Terminal::foldRanges() const
{
    // Before any range is handed out: every fold MUTATION starts by asking for the ranges, so an id
    // minted by a previous grid generation is dropped here rather than toggled -- which would have
    // collapsed whichever block the reused id now names.
    refreshFoldState();

    // Always the PRIMARY screen: that is where the history and the shell's marks live, and an alt-screen
    // application (vim, less) owns the page without leaving any.
    auto const& grid = primaryScreen().grid();

    // stableBase advances on every scroll of the page, which is exactly when a mark may have moved into or
    // out of reach; a generation bump means row identity is gone. Between those, marks change position only
    // by a scroll inside margins, which drops the scan through the mark revision (foldRowsMoved()), so
    // otherwise the previous scan still stands.
    auto const key = FoldRangesKey { .generation = grid.generation(),
                                     .stableBase = grid.stableLineIdOf(LineOffset(0)),
                                     .stableFloor = grid.stableRangeFloor(),
                                     .markRevision = _semanticMarkRevision };
    if (_foldRangesKey == key)
        return _foldRanges;

    _foldRangesKey = key;

    // No mark has ever been stamped, so there is nothing for the scan to find -- every range hangs off a
    // Marked line, and stamping one goes through invalidateFoldRanges(). A shell with no OSC 133
    // integration would otherwise climb the whole scrollback on every scroll to be told so again.
    if (_semanticMarkRevision != 0)
        _foldRanges = primaryScreen().foldRanges(MaxFoldScanLines);
    else
        _foldRanges.clear();

    return _foldRanges;
}

void Terminal::ensureFoldProjection() const
{
    // Before the key is assembled, because reconciliation can empty the state and therefore move the
    // revision the key is stamped with. Everything that reads the projection, the hidden runs or the
    // hidden count arrives through here, so this is where a stale generation stops.
    refreshFoldState();

    auto const& grid = primaryScreen().grid();
    auto const pageLines = unbox<int>(pageSize().lines);
    auto const key = FoldProjectionKey { .generation = grid.generation(),
                                         .stableBase = grid.stableLineIdOf(LineOffset(0)),
                                         .stableFloor = grid.stableRangeFloor(),
                                         .scrollOffset = unbox<int>(_viewport.scrollOffset()),
                                         .pageLines = pageLines,
                                         .foldRevision = _foldState.revision(),
                                         .markRevision = _semanticMarkRevision,
                                         .extraLines = unbox<int>(smoothScrollExtraLines()),
                                         .foldingApplies = foldingAppliesToDisplayedPage() };
    if (_foldProjectionKey == key)
        return;

    _foldProjectionKey = key;
    _foldProjection.clear();
    _hiddenIntervals.clear();
    _hiddenLineCount = LineCount(0);

    // The fast path, and the common one: with nothing collapsed the contiguous page IS the projection,
    // so no ranges are scanned for and no rows are built. Callers read empty as "unfolded".
    if (_foldState.empty() || !key.foldingApplies)
        return;

    // Derived ONCE and handed to everything that needs it: the projection walk binary-searches these
    // runs per row, the count below measures them, and the fold-aware Vi motions step over them.
    _hiddenIntervals = vtbackend::hiddenIntervals(foldRanges(), _foldState);

    // What the grid still holds, and therefore what a fold can hide: a range whose rows have been partly
    // evicted hides only the part that is still there, and one entirely evicted hides nothing. Measured
    // with visibleDistance() rather than by summing clamped runs, so the arithmetic that decides which
    // rows are reachable is stated once (@see Folding.hpp) and costs the runs CROSSED, not all of them.
    auto const floor = key.stableBase + unbox<int64_t>(grid.addressableTop());
    auto const ceiling = key.stableBase + pageLines - 1;
    auto const addressable = ceiling + 1 - floor;
    auto const visible = vtbackend::visibleDistance(_hiddenIntervals, floor, ceiling + 1);
    _hiddenLineCount = LineCount::cast_from(addressable - visible);

    // The rows the viewport shows: a page-full, plus the smooth-scrolling rows, which belong ABOVE the
    // page -- Grid::render places anything beyond the page count there. Without them a smooth scroll over
    // collapsed folds would leave the strip at the top of the viewport undrawn while the content slid
    // down through it.
    auto const wanted = pageLines + key.extraLines;

    // How far below the top row of the viewport the true bottom lies, in VISIBLE rows -- which is how
    // far up the grid the walk must START, since what the scroll offset skips depends on what is hidden
    // along the way. Bounded by what the grid can still supply, so a viewport scrolled further than the
    // remaining history reaches keeps showing the topmost rows rather than a single one.
    //
    // Bounded by the PAGE, not by `wanted`: `visible - pageLines` is precisely the largest offset
    // Viewport::scrollableLineCount() admits, so bounding by the extra smooth-scroll row too would
    // refuse the topmost offset and draw the whole page one row further down -- snapping back the moment
    // the sub-cell offset reached zero. The extra row is dropped instead, which is what happens anyway:
    // the walk below runs out of grid and returns a page-length list, exactly as the unfolded path drops
    // it (@see Grid.hpp's extraOffset).
    //
    // Found by stepping over the hidden runs rather than by collecting the rows and throwing them away:
    // scrolled fifty thousand lines into a folded history, the discarded rows alone were a per-frame
    // allocation and fifty thousand pushes.
    auto const skipped = std::min<int64_t>(key.scrollOffset, std::max<int64_t>(0, visible - pageLines));
    auto const bottom =
        advanceVisibleId(_hiddenIntervals, ceiling, skipped, VerticalDirection::Up, floor, ceiling);

    _foldProjection = projectRows(_hiddenIntervals,
                                  key.stableBase,
                                  LineOffset::cast_from(bottom - key.stableBase),
                                  LineCount(wanted),
                                  grid.addressableTop());
}

std::span<LineOffset const> Terminal::foldProjection() const
{
    // Checked BEFORE anything is computed, and that placement is load-bearing: Viewport's coordinate
    // translation calls this once PER CELL of every frame, where it used to be a constexpr addition.
    // With nothing collapsed -- which is every session that never folds anything, and every moment of
    // the ones that do until they first fold -- the whole feature costs one load and one branch, rather
    // than assembling and comparing a cache key two thousand times a frame.
    if (_foldState.empty())
        return {};

    ensureFoldProjection();
    return _foldProjection;
}

LineOffset Terminal::foldProjectionTopRow() const
{
    if (_foldState.empty())
        return LineOffset(0);

    ensureFoldProjection();
    if (_foldProjection.empty())
        return LineOffset(0);

    // The same rule Grid::render places the row list by -- literally, both go through the one function
    // that states it, which is what keeps Viewport's coordinate translation from drifting away from
    // where the rows are actually drawn.
    return foldedRowsTopRow(pageSize().lines, _foldProjection.size());
}

LineCount Terminal::hiddenLineCount() const
{
    if (_foldState.empty())
        return LineCount(0);

    ensureFoldProjection();
    return _hiddenLineCount;
}

std::span<HiddenInterval const> Terminal::hiddenIntervals() const
{
    if (_foldState.empty())
        return {};

    ensureFoldProjection();
    return _hiddenIntervals;
}

bool Terminal::isLineHiddenByFold(LineOffset gridLine) const
{
    // The fast path, and the common one: nothing collapsed hides nothing.
    if (_foldState.empty())
        return false;

    auto const base = currentStableBase();
    return hidingInterval(hiddenIntervals(), base + unbox<int64_t>(gridLine)).has_value();
}

int Terminal::visibleDistance(LineOffset from, LineOffset to) const
{
    // With nothing collapsed every grid line is a visible line, so the distance is the difference it
    // has always been.
    if (_foldState.empty())
        return unbox<int>(to - from);

    // Ids and LineOffsets both increase downwards, so the base cancels out of a DIFFERENCE entirely --
    // it is carried only because hiddenIntervals() speaks in ids.
    auto const base = currentStableBase();
    return static_cast<int>(vtbackend::visibleDistance(
        hiddenIntervals(), base + unbox<int64_t>(from), base + unbox<int64_t>(to)));
}

LineOffset Terminal::advanceVisibleLines(LineOffset line, int count, VerticalDirection direction) const
{
    // The page on display, the grid the vi cursor and the viewport move over -- @see currentStableBase(),
    // which the walk below uses and which states why.
    auto const& grid = displayedPage().grid();
    auto const step = direction == VerticalDirection::Down ? count : -count;
    auto const top = grid.addressableTop();
    auto const bottom = boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1);

    // The fast path, and the common one: with nothing collapsed every grid line is a visible line, so
    // the motion is the addition it has always been.
    if (_foldState.empty())
        return std::clamp(line + LineOffset(step), top, bottom);

    // Ids and LineOffsets both increase downwards -- the base cancels -- so the walk is done in id space
    // and the result shifted back, rather than mapping every intermediate step.
    auto const base = currentStableBase();
    auto const id = advanceVisibleId(hiddenIntervals(),
                                     base + unbox<int64_t>(line),
                                     count,
                                     direction,
                                     base + unbox<int64_t>(top),
                                     base + unbox<int64_t>(bottom));
    return LineOffset::cast_from(id - base);
}

LineOffset Terminal::snapToVisibleLine(LineOffset line, VerticalDirection direction) const
{
    // The page on display, for the reason currentStableBase() gives.
    auto const& grid = displayedPage().grid();

    // Clamped on BOTH paths, so the documented contract ("a line the caller can put a cursor on")
    // does not quietly depend on whether anything happens to be collapsed right now.
    auto const clampToGrid = [&](LineOffset value) {
        return std::clamp(
            value, grid.addressableTop(), boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1));
    };

    if (_foldState.empty())
        return clampToGrid(line);

    auto const base = currentStableBase();
    auto const id = snapToVisibleId(hiddenIntervals(), base + unbox<int64_t>(line), direction);

    // Snapping out of a run at the very edge of the grid can name a row just past it.
    return clampToGrid(LineOffset::cast_from(id - base));
}
// }}}

// {{{ Scrollbar marks
std::vector<ScrollbarMarkInput> Terminal::scrollbarMarkInputs() const
{
    // The alternate screen keeps no history and no marks: the scrollbar over it has nothing to point into.
    // Nor is a tick offered while another page is on display, wherever the cursor is: the gate is the
    // reveal's, so no tick is drawn that a click would refuse.
    if (!canRevealScrollback())
        return {};

    // Both reconcile a cache of their own first, so both are read before the intervals below, which view the
    // fold projection's: nothing that could rebuild it then runs while they are in use.
    auto const& blocks = commandBlocks();
    auto const& marks = userMarks().ids();

    // Row 0 is the oldest HISTORY row: where the viewport measures its own range from
    // (Viewport::scrollableLineCount), so a tick agrees with the handle about which row a fraction of the
    // track names.
    auto const& grid = primaryScreen().grid();

    // Empty while nothing is collapsed, and then every distance below is plain subtraction. Every mark is
    // measured from the one row 0, so the runs' lengths are summed once and a mark costs a binary search
    // rather than a step over every run above it: `zM` over a long session is a run per command.
    auto const hidden = hiddenIntervals();
    auto const fromTop = VisibleDistanceFrom { hidden, grid.stableRangeFloor() };
    auto const visibleRowOf = [&](int64_t id) {
        // A mark a collapsed fold hides is drawn nowhere, so its tick stands on the head that hides it.
        return fromTop.to(snapToVisibleId(hidden, id, VerticalDirection::Up));
    };

    auto inputs = std::vector<ScrollbarMarkInput> {};
    inputs.reserve(blocks.size() + marks.size());

    // O(records), never O(scrollback): each record caches its head's stable id.
    blocks.forEachRecord([&](CommandBlockRecord const& record) {
        // A prompt nobody has run anything from is not a command yet; a head another generation recorded
        // names some other row now, or none of this grid's, and one below the floor names none at all.
        if (record.state == CommandBlockState::Prompting || !headOffsetOf(grid, record))
            return;

        inputs.push_back(ScrollbarMarkInput { .visibleRow = visibleRowOf(record.headStableId),
                                              .kind = scrollbarMarkKindOf(outcomeOf(record)),
                                              .targetStableId = record.headStableId });
    });

    // Every id userMarks() holds names a marked line of the grid.
    for (auto const id: marks)
        inputs.push_back(ScrollbarMarkInput {
            .visibleRow = visibleRowOf(id), .kind = ScrollbarMarkKind::UserMark, .targetStableId = id });

    return inputs;
}

uint64_t Terminal::scrollbarMarksRevision() const
{
    // Each store is reconciled with the grid before its revision is read: a reconciliation that moves one
    // moves this once, and the next read finds it where it was.
    auto const blockRevision = commandBlocks().revision();
    auto const userMarkRevision = userMarks().revision();
    refreshFoldState();
    auto const& grid = primaryScreen().grid();
    auto const inputs = ScrollbarMarksInputs {
        .blockRevision = blockRevision,
        .userMarkRevision = userMarkRevision,
        .foldRevision = _foldState.revision(),
        .stableIdGeneration = grid.stableIdGeneration(),
        .stableFloor = grid.stableRangeFloor(),
        .displayedPage = _displayedPage,
    };
    if (inputs != _scrollbarMarksInputs)
    {
        _scrollbarMarksInputs = inputs;
        ++_scrollbarMarksRevision;
    }
    return _scrollbarMarksRevision;
}

UserMarkIndex const& Terminal::userMarks() const
{
    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();

    // A generation bump renamed every row, so every id held names some other line now. The marks
    // themselves survived -- UserMark is head-only and rides a reflow on its logical line's head -- so one
    // scan finds them again. Reflow has just walked the whole scrollback itself, so this adds no new order
    // of cost, and it runs once per bump rather than once per read.
    if (!_userMarks.isCurrentFor(generation))
    {
        // No mark has ever been placed -- every one is heard of in userMarkChanged() -- so there is nothing
        // for the scan to find, and a session that never used `mm` does not walk its scrollback per reflow.
        auto ids = std::vector<int64_t> {};
        if (_userMarkChanges != 0)
            grid.forEachValidLine([&](LineOffset offset, Line const& line) {
                if (line.isFlagEnabled(LineFlag::UserMark))
                    ids.push_back(grid.stableLineIdOf(offset));
            });
        _userMarks.rebuild(ids, generation);
    }

    // Marks whose rows left the grid: scrolled out of the scrollback -- without this the set grows for the
    // life of the session -- or cut off below a page that shrank under them, whose ids the rows scrolled in
    // next come to name. Every other way a mark goes is reported as it happens, so nothing here looks at a
    // line.
    auto const bottom = boxed_cast<LineOffset>(grid.pageSize().lines) - 1;
    _userMarks.prune(StableIdSpan { .first = grid.stableRangeFloor(), .last = grid.stableLineIdOf(bottom) });
    return _userMarks;
}

void Terminal::toggleUserMark(LineOffset line)
{
    auto& screen = displayedPage();
    auto const change = screen.isLogicalLineFlagEnabled(line, LineFlag::UserMark) ? UserMarkChange::Removed
                                                                                  : UserMarkChange::Added;
    // Only the primary screen's marks are held anywhere (the index, the scrollbar, the host): an alternate
    // screen's stays as local as its rows.
    if (_replicaRole.load(std::memory_order_acquire) != ReplicaRole::None && &screen == &primaryScreen())
    {
        auto const& grid = screen.grid();
        _eventListener.userMarkRequested(UserMarkRequest { .stableId = grid.stableLineIdOf(line),
                                                           .generation = grid.stableIdGeneration(),
                                                           .change = change });
        return;
    }
    screen.setLogicalLineFlags(line, LineFlag::UserMark, change == UserMarkChange::Added);
}

void Terminal::placeUserMark(UserMarkRequest const& request)
{
    auto& screen = primaryScreen();
    auto const& grid = screen.grid();
    if (grid.stableIdGeneration() != request.generation)
        return;
    if (auto const line = grid.lineOffsetOf(request.stableId))
        screen.setLogicalLineFlags(*line, LineFlag::UserMark, request.change == UserMarkChange::Added);
}

void Terminal::userMarkChanged(int64_t headStableId, UserMarkChange change)
{
    // Counted even where the index drops the change for its generation: the line carries the mark either
    // way, and the rescan that follows has to look for it.
    ++_userMarkChanges;
    _userMarks.record(headStableId, primaryScreen().grid().stableIdGeneration(), change);
}

void Terminal::userMarkRowsMoved(RowMove const& move)
{
    _userMarks.follow(move);
}

std::expected<void, RevealError> Terminal::revealStableLineAtTop(int64_t stableId,
                                                                 uint64_t stableIdGeneration)
{
    auto const& grid = primaryScreen().grid();
    if (stableIdGeneration != grid.stableIdGeneration())
        return std::unexpected { RevealError::OtherGeneration };

    auto const line = grid.lineOffsetOf(stableId);
    if (!line)
        return std::unexpected { RevealError::Evicted };

    // Asked before the fold is touched: a reveal that cannot land changes nothing.
    if (!canRevealScrollback())
        return std::unexpected { RevealError::NotScrollable };

    // Expanded before scrolling: a collapsed fold changes which offset puts the line on top. A block's head
    // stays drawn while collapsed, and a user who asked to see a command asked to see what it printed as
    // well; a line inside the fold is drawn nowhere until it opens.
    expandFoldContaining(*line);

    // A part-row smooth-scroll offset would draw the line part-way off the top, and a glide in flight would
    // carry it off again. Both show in the frame, so dropping them asks for one even where the line is on
    // top already and the scroll offset stays: the viewport asks only when it moves.
    auto const wasSmoothScrolling = _viewport.pixelOffset() != 0.0f || isMomentumScrollActive();
    resetSmoothScroll();
    if (!_viewport.scrollLineToTop(*line) && wasSmoothScrolling)
        breakLoopAndRefreshRenderBuffer();
    return {};
}
// }}}

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

void Terminal::rowsDropped(LineCount pageShift)
{
    _viCommands.cursorPosition.line -= boxed_cast<LineOffset>(pageShift);
    clampToHistory();

    // A scrollback session's labels name rows for the whole session, and some of them are gone. A visible
    // one re-scans the viewport whenever it changes.
    if (_hintModeHandler.isActive() && _hintScope == HintScope::Scrollback)
        _hintModeHandler.deactivate();
    _highlightRange = std::nullopt;
    _gutterHoverLine = std::nullopt;

    // The fold caches need nothing: both are keyed on the stable floor and base, which a drop moves.
    scrollbackBufferCleared();
}

void Terminal::screenUpdated()
{
    if (markScreenChanged() == ScreenNotification::Due)
        _eventListener.screenUpdated();
}

void Terminal::screenReplayed()
{
    if (markScreenChanged() == ScreenNotification::Due)
        _eventListener.screenReplayed();
}

Terminal::ScreenNotification Terminal::markScreenChanged()
{
    ZoneScoped;
    // The screen changed whatever is drawn next, and the display's next frame tells link detection so: set
    // ahead of the returns, which skip only the notification. The same one store as after them.
    _screenDirty = true;

    if (isRenderingSuppressed())
        return ScreenNotification::Skipped;

    // A frame still waiting for its swap is the display's to show: retry it, and stay quiet while a paint
    // holds the front buffer still. One that now succeeds is announced as any change is -- the display that
    // built it may since have gone (a tab switched from), and this output is then the only news a background
    // tab gets. A spurious announcement to a display that is still there costs nothing; it coalesces.
    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers
        && !_renderBuffer.swapBuffers(_renderBuffer.lastUpdate.load(std::memory_order_relaxed)))
        return ScreenNotification::Skipped;

    return ScreenNotification::Due;
}

void Terminal::followOutput()
{
    // Taken whatever is decided below, so that what scrolled while the view sat at the bottom is not
    // followed later, by a batch that added nothing. A shell's line editor that repaints its prompt on a
    // timer (endo, once a second) adds nothing, and used to bring a view taken back to read the history down
    // with every repaint: xterm's scrollTtyOutput follows any byte read, ghostty only a bottom row that
    // moved. Only what the page on display scrolled counts: output to another page is no output to follow,
    // wherever the cursor ends the batch.
    auto const scrolled = displayedPage().linesScrolledIntoHistory();
    if (std::exchange(_displayedPageLinesFollowed, scrolled) == scrolled)
        return;

    if (!_settings.autoScrollOnUpdate || !_viewport.scrolled() || _inputHandler.mode() != ViMode::Insert)
        return;

    if (_eventListener.outputFollowing() == OutputFollowing::Follow)
        _viewport.scrollToBottom();
}

void Terminal::runWithOwnStateLockReleased(std::function<void()> const& pause)
{
    _ownStateLock.unlock();
    auto const _ = core::Finally([this] { _ownStateLock.lock(); });
    pause();
}

void Terminal::renderBufferUpdated()
{
    // As in screenUpdated(): ahead of the returns, which skip only the notification.
    _screenDirty = true;

    if (isRenderingSuppressed())
        return;

    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
    {
        _renderBuffer.swapBuffers(_renderBuffer.lastUpdate.load(std::memory_order_relaxed));
        return;
    }

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
    reply("\033]52;{};{}\033\\", selection, core::base64::encode(content.begin(), content.end()));
}

void Terminal::openDocument(string_view data)
{
    runOutsideStateLock([this, document = string(data)] { _eventListener.openDocument(document); });
}

void Terminal::openLink(LinkTarget const& link)
{
    runOutsideStateLock([this, link] { _eventListener.openLink(link); });
}

std::optional<Terminal::LogicalLineScan> Terminal::scanLogicalLineAt(CellLocation gridPosition) const
{
    auto const& screen = displayedPage();
    auto const historyTop = -screen.historyLineCount().as<LineOffset>();
    auto const gridBottom = pageSize().lines.as<LineOffset>() - LineOffset(1);
    if (gridPosition.line < historyTop || gridPosition.line > gridBottom)
        return std::nullopt;

    // The logical line holding the cell, no further than one scan budget either way.
    auto const extent = scanExtentOf(screen.grid(), gridPosition.line, gridPosition.line);
    auto const rows = scanRowsOf(screen.grid(), extent.first, extent.last);
    for (auto& text: joinLogicalLines(rows, extent.continuesBelow))
        if (text.lineOf(0) <= gridPosition.line && gridPosition.line <= text.lineOf(text.rowCount() - 1))
        {
            auto const row = unbox<std::size_t>(gridPosition.line - text.lineOf(0));
            return LogicalLineScan { .text = std::move(text),
                                     .position = ScanPosition { .row = row, .column = gridPosition.column } };
        }
    return std::nullopt;
}

LinkLookup Terminal::lookUpLinkAt(CellLocation gridPosition) const
{
    auto lookup = LinkLookup {};

    // The page on display, as the pointer and the index read it -- not the cursor's.
    if (auto hyperlink = displayedPage().hyperlinkAt(gridPosition))
    {
        lookup.hyperlink = std::move(hyperlink);
        return lookup;
    }
    if (auto const* const link = detectedLinkAt(gridPosition))
    {
        lookup.target = link->target;
        return lookup;
    }
    // The index holds the row as it is: what it found there is all there is (§7.2), and no pattern runs on
    // this thread -- but a path found there and not confirmed yet, its probe pending or dropped by the cap on
    // what may wait, is looked for: the click is the user asking, and it opens once found.
    auto const& grid = displayedPage().grid();
    if (auto const* const index = detectedLinks(); index && isScannedAsItIs(*index, grid, gridPosition.line))
    {
        if (auto const* const pending =
                _linkDetector->unconfirmedPathAt(grid.stableLineIdOf(gridPosition.line), gridPosition.column))
        {
            lookup.printedPath = pending->link.target;
            lookup.pathToProbe = *pending->query;
            lookup.pathDirectories = pending->directories;
        }
        return lookup;
    }

    // Not scanned yet, or written since: this one logical line, now, with the grammar detection uses, merged
    // so that a URL's own path is no second link.
    auto const scan = scanLogicalLineAt(gridPosition);
    if (!scan)
        return lookup;
    auto const candidates =
        findLinks(scan->text, *_settings.linkPatterns, PatternSelection::ClickableOnly, LinkOverlap::Merge);
    // What detection would not find on this page is no link here either.
    auto const screen = _settings.linkDetection.forPage(_displayedPage);
    auto const candidate = std::ranges::find_if(candidates, [&](LinkCandidate const& found) {
        return found.first <= scan->position && scan->position <= found.last
               && screen.detects(linkCategoryOf(found.kind, found.target));
    });
    if (candidate == candidates.end())
        return lookup;

    // Classified as detection classifies it: a path against the directories of the command that printed the
    // logical line's first row, and from the probe's cache where it knows.
    if (candidate->kind == LinkKind::Path)
        lookup.printedPath = printedLinkTarget(*candidate);
    auto const* const probe = _linkDetector ? _linkDetector->pathProbe().get() : nullptr;
    auto classified = classifyPrintedLink(
        printedLinkTarget(*candidate), localPathBaseAt(scan->text.lineOf(0)), linkMachine(), probe);
    std::visit(core::Overloaded {
                   [&](LinkTarget& target) { lookup.target = std::move(target); },
                   [&](PathToProbe& toProbe) {
                       lookup.printedPath = std::move(toProbe.printed);
                       lookup.pathToProbe = std::move(toProbe.query);
                       lookup.pathDirectories = std::move(toProbe.directories);
                   },
                   [&](RefusedPath& refused) {
                       lookup.printedPath = std::move(refused.printed);
                       lookup.refusal = refused.reason;
                   },
                   [&](MissingPath& missing) { lookup.printedPath = std::move(missing.printed); },
                   [](NotALink) {},
               },
               classified);
    return lookup;
}

std::expected<PathBases, PathRefusal> Terminal::localPathBaseAt(LineOffset row) const
{
    return terminalDirectory().and_then(
        [&](TerminalDirectory const& here) { return localPathBaseFor(pathBlockAt(row), here); });
}

CommandBlockRecord const* Terminal::pathBlockAt(LineOffset row) const
{
    // Blocks live on the primary screen; a row of any other page on display belongs to none.
    return displayedScreenType() == ScreenType::Primary ? commandBlockAt(row) : nullptr;
}

std::expected<TerminalDirectory, PathRefusal> Terminal::terminalDirectory() const
{
    // The role fileUrlLocalHost() judges a file:// URL by: on a replica of another machine's terminal,
    // every path on screen names a file there -- an absolute one included, which no working directory is
    // asked about -- and one that exists here too is another file.
    if (_replicaRole.load(std::memory_order_acquire) == ReplicaRole::OtherMachine)
        return std::unexpected(PathRefusal::ReplicaOfAnotherMachine);

    // Only a directory provably on this machine: a relative path in another host's is not one here.
    auto openLocally =
        resolveWorkingDirectory(CwdPurpose::OpenLocally)
            .transform([](ResolvedWorkingDirectory resolved) { return std::move(resolved.path); })
            .value_or(std::string {});
    // Judged as a block's own directory is (workingDirectorySnapshot()), so the two compare.
    auto const shown = resolveWorkingDirectory(CwdPurpose::Display);
    return TerminalDirectory { .openLocally = std::move(openLocally),
                               .locality = shown ? shown->locality : ContextLocality::Unknown };
}

void Terminal::recordForeignDirectory()
{
    if (_replicaRole.load(std::memory_order_acquire) != ReplicaRole::None)
        return;
    // Judged as terminalDirectory() judges it, so the record keeps what a path base saw while the block ran.
    if (auto const shown = resolveWorkingDirectory(CwdPurpose::Display);
        shown && shown->locality == ContextLocality::Foreign)
        _commandBlocks.foreignDirectoryReported();
}

core::platform::FileSystem const& Terminal::pathFileSystem() const noexcept
{
    return _linkServices ? _linkServices->fileSystem : _fileSystem;
}

LinkMachine Terminal::linkMachine() const
{
    return LinkMachine { .home = _homeDirectory, .hostname = std::string { localIdentity().hostname } };
}

std::expected<PathBases, PathRefusal> Terminal::TerminalLinkSource::pathBasesAt(LineOffset row) const
{
    if (_memo.frame != _frame)
        _memo = Memo { .frame = _frame, .directory = std::nullopt, .bases = {} };
    if (!_memo.directory)
        _memo.directory = _terminal.terminalDirectory();
    if (!*_memo.directory)
        return std::unexpected(_memo.directory->error());

    auto const* const block = _terminal.pathBlockAt(row);
    auto const known = std::ranges::find(_memo.bases, block, [](auto const& entry) { return entry.first; });
    if (known != _memo.bases.end())
        return known->second;
    return _memo.bases.emplace_back(block, localPathBaseFor(block, **_memo.directory)).second;
}

// {{{ Hint mode

HintScanArea Terminal::collectHintScanArea(HintScope scope, LineCount scrollbackLimit) const
{
    // The page on display, which the labels are drawn over.
    auto const& screen = displayedPage();
    auto const gridBottom = pageSize().lines.as<LineOffset>() - LineOffset(1);
    auto const historyTop = -screen.historyLineCount().as<LineOffset>();

    // A scrollback limit is a row count; a negative value (an out-of-range config) would push
    // labelableRows.first past last and underflow the row count below, so clamp it to zero.
    scrollbackLimit = std::max(scrollbackLimit, LineCount(0));

    // A label can only be typed once it has been drawn. Under Visible that is the viewport; under
    // Scrollback the labels are fixed for the session, so every scanned row can carry one and the
    // user scrolls to reach it.
    auto const labelableRows = [&] {
        if (scope == HintScope::Scrollback)
            return HintRowRange { .first = std::max(historyTop, -scrollbackLimit.as<LineOffset>()),
                                  .last = gridBottom };
        // Through the projection rather than by negating the scroll offset: that offset counts VISIBLE
        // rows, so with folds collapsed it names a grid line far down the page instead of the one the
        // viewport starts at -- and the scan would then cover rows the viewport does not draw while
        // missing ones it does.
        return HintRowRange { .first = _viewport.topLine(),
                              .last = _viewport.translateScreenToGridLine(gridBottom) };
    }();

    // Whole logical lines, so a pattern wrapped across the labelable range's edge is still matched in full --
    // but no further than one scan budget either way (MaxScanCells): beyond it the text is cut anyway.
    auto const extent = scanExtentOf(screen.grid(), labelableRows.first, labelableRows.last);
    return HintScanArea {
        .rows = scanRowsOf(screen.grid(), extent.first, extent.last),
        .continuesBelow = extent.continuesBelow,
        .labelableRows = labelableRows,
    };
}

void Terminal::refreshHints()
{
    // Scrollback-scope labels are assigned once and must not move: scrolling reveals other rows'
    // labels rather than renaming them, so a label the user already read stays valid. Only the
    // visible scope, whose scan region *is* the viewport, re-scans.
    if (_hintScope == HintScope::Scrollback)
        return;

    _hintModeHandler.refresh(collectHintScanArea(HintScope::Visible, LineCount(0)));
}

void Terminal::activateHintMode(HintModeRequest request)
{
    _hintScope = request.scope;
    auto const area = collectHintScanArea(request.scope, request.scrollbackLimit);

    // A path resolves against the directory of the command that printed it, and outside any block against the
    // working directory for opening locally (localPathBaseAt()), decided per row as the mouse decides it, and
    // is classified as detection and a click classify it (classifyPrintedLink()). A row whose paths name
    // files on another machine offers none. A row with no directory to resolve against still resolves an
    // absolute or `~/` path, and offers what is printed as written when that names nothing -- except a bare
    // word, which only a lookup could tell from prose. What the probe's cache does not know is looked up at
    // once, as hint mode is user-initiated (§12), through the probe where detection runs, which fills its
    // cache, and in the terminal's filesystem otherwise. What a block's rows resolve against is worked out
    // once per activation.
    auto mutablePatterns = std::move(request.patterns);
    mutablePatterns.resolve =
        [this,
         here = terminalDirectory(),
         blocks = std::make_shared<std::map<CommandBlockId, std::expected<PathBases, PathRefusal>>>()](
            LinkCandidate const& candidate, LineOffset row) -> std::optional<LinkTarget> {
        auto printed = printedLinkTarget(candidate);
        // A URL or a custom link is as it opens; a file:// URL is the path it names (§7.6), and goes on as
        // one.
        if (candidate.kind != LinkKind::Path && !isFileUrl(printed.target))
            return printed;
        if (!here)
            return std::nullopt;
        auto const* const block = pathBlockAt(row);
        auto const id = block ? block->id : CommandBlockId {};
        auto known = blocks->find(id);
        if (known == blocks->end())
            known = blocks->emplace(id, localPathBaseFor(block, *here)).first;
        auto const& bases = known->second;
        // Cannot resolve ~/ when HOME is unset: offered as written.
        if (bases && printed.target.starts_with("~/") && _homeDirectory.empty())
            return printed;
        auto const asWritten = [&bases](LinkTarget written) -> std::optional<LinkTarget> {
            if (bases->empty() && !isBarePathName(written.target))
                return written;
            return std::nullopt;
        };
        auto* const probe = _linkDetector ? _linkDetector->pathProbe().get() : nullptr;
        auto classified = classifyPrintedLink(std::move(printed), bases, linkMachine(), probe);
        return std::visit(
            core::Overloaded {
                [](LinkTarget& target) -> std::optional<LinkTarget> { return std::move(target); },
                [&](PathToProbe& toProbe) -> std::optional<LinkTarget> {
                    auto const answer =
                        probe ? probe->probe(toProbe.query)
                              : resolveLocalPath(pathFileSystem(), toProbe.query.candidates)
                                    .value_or(ResolvedPath {
                                        .path = {}, .kind = PathKind::Missing, .candidateIndex = 0 });
                    if (answer.kind == PathKind::Missing)
                        return asWritten(std::move(toProbe.printed));
                    return resolvedPathTarget(std::move(toProbe.printed), answer, toProbe.directories);
                },
                [](RefusedPath const&) -> std::optional<LinkTarget> { return std::nullopt; },
                [&](MissingPath& missing) { return asWritten(std::move(missing.printed)); },
                [](NotALink) -> std::optional<LinkTarget> { return std::nullopt; },
            },
            classified);
    };

    _hintModeHandler.activate(area, std::move(mutablePatterns), request.action);
    _inputHandler.setMode(ViMode::Hint);
}

void Terminal::applyHintOverlay(RenderBuffer& output, LineOffset baseLine) const
{
    if (!_hintModeHandler.isActive())
        return;

    auto const& matches = _hintModeHandler.matches();
    if (matches.empty())
        return;

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

    // A match projected from scroll-invariant grid rows into render-buffer rows.
    struct ProjectedMatch
    {
        CellLocationRange body;  ///< The whole match: a run of text that may cross rows.
        CellLocation labelStart; ///< Where the label's first character is drawn.
        std::string_view label;
    };

    // Bucket the matches by the main-screen row they touch, so the sweep below consults only the
    // matches on the row it is on rather than all of them. A wrapped match lands in every row it
    // covers. Under HintScope::Scrollback the match count is unbounded by the page size, which is
    // what makes this necessary rather than merely tidy.
    auto const pageLines = pageSize().lines.as<LineOffset>();
    auto const pageColumns = unbox<int>(pageSize().columns);
    auto rowBuckets = std::vector<std::vector<ProjectedMatch>>(unbox<size_t>(pageLines));

    for (auto const& match: matches)
    {
        // A match a collapsed fold hides is drawn nowhere at all. Skipped rather than projected,
        // because the translation below answers for a hidden line with the row standing in its PLACE --
        // which would paint the label over unrelated text and offer a target the user never saw.
        if (isLineHiddenByFold(match.start.line))
            continue;

        // Through the projection, NOT by adding the scroll offset: with folds collapsed the rows on
        // screen are a non-contiguous selection of grid rows, so the two differ by however many rows
        // those folds hide, and every label landed that far from the text it names. @see
        // Viewport::translateGridToScreenCoordinate, which every other consumer already goes through.
        auto const first = _viewport.translateGridToScreenCoordinate(match.start);
        auto const last = _viewport.translateGridToScreenCoordinate(match.end);
        auto const projected = ProjectedMatch {
            .body = CellLocationRange { .first = first + baseLine, .second = last + baseLine },
            .labelStart = first + baseLine,
            .label = match.label,
        };
        // The label may overrun the row's right edge and wrap onto the next row(s), so it can touch
        // a row the body does not; bucket every row either of them covers.
        auto const labelEndColumn = unbox<int>(first.column) + static_cast<int>(match.label.size()) - 1;
        auto const labelEndLine = first.line + LineOffset(labelEndColumn / pageColumns);
        auto const from = std::max(first.line, LineOffset(0));
        auto const to = std::min(std::max(last.line, labelEndLine), pageLines - LineOffset(1));
        for (auto row = from; row <= to; ++row)
            rowBuckets[unbox<size_t>(row)].push_back(projected);
    }

    // The label occupies label.size() consecutive grid cells starting at labelStart, flowing left to
    // right and wrapping onto the next row at the page's right edge (exactly as the wrapped match's
    // body does). Returns which label character, if any, falls on @p pos.
    auto const labelIndexAt = [&](CellLocation pos, ProjectedMatch const& match) -> std::optional<size_t> {
        auto const rowDelta = unbox<int>(pos.line - match.labelStart.line);
        if (rowDelta < 0)
            return std::nullopt;
        auto const index =
            (rowDelta * pageColumns) + unbox<int>(pos.column) - unbox<int>(match.labelStart.column);
        if (index < 0 || static_cast<size_t>(index) >= match.label.size())
            return std::nullopt;
        return static_cast<size_t>(index);
    };

    for (auto& cell: output.cells)
    {
        // Rows outside the main screen — the status line — carry no hints, so they only ever dim,
        // exactly as before.
        auto const row = cell.position.line - baseLine;
        auto const bucket = row >= LineOffset(0) && row < pageLines
                                ? std::span<ProjectedMatch const> { rowBuckets[unbox<size_t>(row)] }
                                : std::span<ProjectedMatch const> {};

        auto onMatch = false;
        for (auto const& match: bucket)
        {
            // The label is drawn over the match's first label.size() cells — wrapping onto the next
            // row rather than being clipped at the right edge, and drawn in full even where it
            // overruns a match shorter than its own label: a half-drawn label cannot be typed.
            if (auto const labelIdx = labelIndexAt(cell.position, match))
            {
                // Replace codepoints with label character.
                cell.codepoints = std::u32string(1, static_cast<char32_t>(match.label[*labelIdx]));
                cell.width = 1;

                // Distinguish the already-typed prefix from the characters still to type.
                auto const typed = *labelIdx < filter.size();
                cell.attributes.foregroundColor = typed ? typedLabelFg : labelFg;
                cell.attributes.backgroundColor = typed ? typedLabelBg : labelBg;
                cell.attributes.flags |= CellFlag::Bold;

                onMatch = true;
                break;
            }

            if (match.body.contains(cell.position))
            {
                // Highlight match body with palette-configured background blend.
                cell.attributes.backgroundColor =
                    mixColor(cell.attributes.backgroundColor, matchBg, matchBgAlpha);
                onMatch = true;
                break;
            }
        }

        // Fade cells that belong to no match toward the default background, so labels stand out.
        if (!onMatch)
            blendAttributesTo(cell.attributes, _colorPalette.defaultBackground, 0.5f);
    }
}

namespace
{
    /// Pastes a hint's text into @p terminal, unless it cannot arrive as it is (Terminal::pastesUnchanged()):
    /// a path resolved against the working directory carries whatever the OSC 7 URL decoded into it -- an
    /// ESC, a line feed.
    /// @param terminal The terminal to paste into.
    /// @param text The hint's text.
    void typeHint(Terminal& terminal, std::string const& text)
    {
        if (!terminal.pastesUnchanged(text))
        {
            terminalLog()("Not typing the hint \"{}\": a shell would read part of it as keys.",
                          core::escape(text));
            return;
        }
        terminal.sendPaste(text);
    }
} // namespace

void Terminal::HintModeExecutor::onHintSelected(HintMatch const& match, HintAction action)
{
    auto const& matchedText = match.matchedText;
    switch (action)
    {
        case HintAction::Copy: terminal._eventListener.copyToClipboard(matchedText); break;
        case HintAction::Open:
            // As Ctrl+click opens it, with its refusals: a path with file_opener at its location, a URL as a
            // link is followed. Nothing, where the printed text chose a scheme no link may open (§7.6).
            if (match.opening == LinkOpening::Allowed)
                terminal.openLink(match.target);
            break;
        case HintAction::Paste: typeHint(terminal, matchedText); break;
        case HintAction::CopyAndPaste:
            terminal._eventListener.copyToClipboard(matchedText);
            typeHint(terminal, matchedText);
            break;
        case HintAction::Select: {
            // Clear any existing selection so that entering Visual mode uses our
            // cursor position as the anchor, not a stale selection start.
            terminal.clearSelection();

            // Enter vi visual mode with the match range pre-selected. The positions are already
            // grid-absolute, which is what the Vi cursor speaks.
            terminal._viCommands.cursorPosition = match.start;
            terminal._inputHandler.setMode(ViMode::Visual);
            terminal._viCommands.moveCursorTo(match.end);
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
    // Under S8C1T the terminal transmits its C1 control introducers as single 8-bit bytes (CSI -> 0x9B,
    // DCS -> 0x90, ST -> 0x9C, ...). 8-bit C1 transmission is a VT200+ capability, so it applies only
    // while operating at VT level 2 or above: a terminal that has dropped back to VT100 level -- e.g.
    // after a VT52 round-trip, where setVT52Mode() resets the operating level -- replies in 7-bit even
    // if S8C1T was selected earlier. This is xterm's rule and is exactly what vttest's post-VT52 check
    // expects.
    auto const eightBit =
        _c1TransmissionMode == ControlTransmissionMode::S8C1T && conformanceLevelOf(_operatingLevel) >= 2;
    auto const folded = eightBit ? foldC1ControlsToEightBit(text) : std::string {};
    auto const bytes = eightBit ? std::string_view(folded) : text;
    core::locked(_inputMutex, [&] { _inputGenerator.generateRaw(bytes); });

    // Only inside the parser, which defers the local echo of what it flushes until it has unwound. A reply
    // raised outside it -- a selection report, a notification's answer -- comes from a frontend that may hold
    // the state lock, where a flush would echo, which parses and takes that lock again. It goes out with the
    // next flush, as it does without the knob.
    if (_syncPtyOutput && tlParseDepth > 0)
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
    changeInputEncoding([&](InputGenerator& input) { input.setApplicationKeypadMode(enabled); });
    publishBindingModes();
}

void Terminal::setBracketedPaste(bool enabled)
{
    changeInputEncoding([&](InputGenerator& input) { input.setBracketedPaste(enabled); });
}

void Terminal::setModifyOtherKeys(int mode)
{
    changeInputEncoding([&](InputGenerator& input) { input.setModifyOtherKeys(mode); });
}

int Terminal::modifyOtherKeys() const noexcept
{
    return _inputGenerator.modifyOtherKeys();
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

void Terminal::setWarningBellVolume(BellVolume volume)
{
    _settings.warningBellVolume = volume;
}

void Terminal::setMarginBellVolume(BellVolume volume)
{
    _settings.marginBellVolume = volume;
}

void Terminal::setGenerateFocusEvents(bool enabled)
{
    changeInputEncoding([&](InputGenerator& input) { input.setGenerateFocusEvents(enabled); });
}

void Terminal::setLinkPatterns(std::shared_ptr<LinkPatterns const> patterns)
{
    // The table in use: replacing the detector would throw away its index and its probe cache, and the links
    // would vanish until the next quiet time, on every reload of a profile that uses the built-ins.
    if (patterns == _settings.linkPatterns)
        return;
    _settings.linkPatterns = std::move(patterns);
    replaceLinkDetector();
}

void Terminal::setLinkDetectionPolicy(LinkDetectionPolicy policy)
{
    // The policy in use: a reload that leaves it as it was keeps the index and the probe cache.
    if (policy == _settings.linkDetection)
        return;
    _settings.linkDetection = policy;
    replaceLinkDetector();
}

void Terminal::replaceLinkDetector()
{
    // Replaced, not changed: the jobs in flight finish against the configuration they started with, and their
    // results find nobody to adopt them.
    _linkDetector = makeLinkDetector();
    _linkScanDue.store(chrono::steady_clock::time_point::max(), std::memory_order_relaxed);
    // The new detector's ids start over: the hovered one would name another link, or none.
    _hoveringLinkId = LinkId {};
}

std::unique_ptr<LinkDetector> Terminal::makeLinkDetector()
{
    if (!_linkServices || !_settings.linkPatterns->hasClickable()
        || !_settings.linkDetection.detectsAnywhere())
        return nullptr;
    // breakLoopAndRefreshRenderBuffer() touches atomics and the any-thread notifications only, so a job
    // thread may call it, as the status line's commands do.
    return std::make_unique<LinkDetector>(*_linkServices,
                                          _settings.linkPatterns,
                                          _settings.linkDetection,
                                          [this] { breakLoopAndRefreshRenderBuffer(); });
}

LinkIndex const* Terminal::detectedLinks() const noexcept
{
    if (!_linkDetector)
        return nullptr;
    auto const shown =
        LinkIndexKey { .page = _displayedPage, .generation = displayedPage().grid().stableIdGeneration() };
    return _linkDetector->indexes(shown) ? &_linkDetector->index() : nullptr;
}

void Terminal::setLinkDecorationMode(LinkDecorationMode mode)
{
    _settings.linkDecorationMode = mode;
    renderBufferUpdated();
}

FollowModifiers Terminal::followModifiers() const noexcept
{
    auto const& follow = _settings.linkFollowModifiers;
    return follow && *follow != Modifiers {}
                   && bindingModifiers(_heldModifiers, _settings.mouseProtocolBypassModifiers) == *follow
               ? FollowModifiers::Held
               : FollowModifiers::Released;
}

LinkPointer Terminal::hoverPointer() const noexcept
{
    return allowPassMouseEventToApp(_heldModifiers) ? LinkPointer::Elsewhere : LinkPointer::Over;
}

LinkPointerShape Terminal::linkPointerShape() const noexcept
{
    if (!isPointerOverLink())
        return LinkPointerShape::Default;
    return presentLink(_settings.linkDecorationMode, hoverPointer(), followModifiers()).pointer;
}

LinkRevealChange Terminal::updateHeldModifiers(Modifiers held)
{
    auto const before = std::pair { followModifiers(), hoverPointer() };
    _heldModifiers = held;
    if (std::pair { followModifiers(), hoverPointer() } == before)
        return LinkRevealChange::Unchanged;
    renderBufferUpdated();
    return LinkRevealChange::Changed;
}

void Terminal::setLinkFollowModifiers(std::optional<Modifiers> modifiers)
{
    _settings.linkFollowModifiers = modifiers;
    renderBufferUpdated();
}

void Terminal::setMouseProtocol(MouseProtocol protocol, bool enabled)
{
    changeInputEncoding([&](InputGenerator& input) { input.setMouseProtocol(protocol, enabled); });
    // Turned on, tracking takes the wheel; turned off, the wheel acts on the page on display again.
    updateWheelMode();
}

void Terminal::setMouseTransport(MouseTransport transport)
{
    changeInputEncoding([&](InputGenerator& input) { input.setMouseTransport(transport); });
}

namespace
{
    /// The DEC modes that choose how a mouse report spells its coordinates. They are four spellings
    /// of ONE setting rather than four independent flags -- xterm keeps a single
    /// `screen->extend_coords` and says so outright: "Rather than choose an arbitrary precedence
    /// among the coordinate modes, they are mutually exclusive. For consistency, a reset is only
    /// effective against the matching mode." (xterm charproc.c, `srm_EXT_MODE_MOUSE` et al.)
    ///
    /// A fifth spelling is a row here; the switch labels in setMode only route to it.
    constexpr auto MouseCoordinateModes = std::to_array<std::pair<DECMode, MouseTransport>>({
        { DECMode::MouseExtended, MouseTransport::Extended },   // 1005
        { DECMode::MouseSGR, MouseTransport::SGR },             // 1006
        { DECMode::MouseURXVT, MouseTransport::URXVT },         // 1015
        { DECMode::MouseSGRPixels, MouseTransport::SGRPixels }, // 1016
    });

    /// @param mode The DEC mode to classify.
    /// @return The coordinate encoding @p mode selects, or nullopt if it selects none.
    [[nodiscard]] constexpr optional<MouseTransport> mouseCoordinateModeOf(DECMode mode) noexcept
    {
        for (auto const& [decMode, transport]: MouseCoordinateModes)
            if (decMode == mode)
                return transport;
        return nullopt;
    }
} // namespace

void Terminal::setMouseCoordinateMode(DECMode mode, bool enable)
{
    auto const transport = mouseCoordinateModeOf(mode);
    Require(transport.has_value()); // only the coordinate modes route here

    // xterm's rule, and the reason this cannot be four independent cases: a SET selects this
    // encoding, but a RESET only clears it when it is the one in effect. Clearing unconditionally
    // means a peer that replays a whole mode set -- naming the three it does not want alongside the
    // one it does -- destroys the encoding the application chose, because the resets land after it.
    if (enable)
        setMouseTransport(*transport);
    else if (_inputGenerator.mouseTransport() == *transport)
        setMouseTransport(MouseTransport::Default);
}

void Terminal::setMouseWheelMode(InputGenerator::MouseWheelMode mode)
{
    changeInputEncoding([&](InputGenerator& input) { input.setMouseWheelMode(mode); });
}

void Terminal::setMouseWheelScrollMultiplier(LineCount lines)
{
    _settings.mouseWheelScrollMultiplier = lines;
    changeInputEncoding([&](InputGenerator& input) {
        input.setMouseWheelScrollMultiplier(static_cast<unsigned>(unbox(lines)));
    });
}

void Terminal::setWindowTitle(string_view title)
{
    // Writes _windowTitle lock-free intentionally: the parser-thread callers (OSC 0/2 dispatch in
    // Screen, save/restoreWindowTitle) reach this from inside writeToScreen()'s _stateMutex hold, so
    // the write is already serialized under the lock; taking the non-recursive _stateMutex here would
    // self-deadlock. GUI-thread readers must use resolvedWindowTitle(), which locks and copies.
    _windowTitle = title;
    _eventListener.setWindowTitle(title);
    _extensionEvents.post([&] { return StatusLineEvents::TitleChanged { .title = std::string { title } }; });
}

std::string const& Terminal::windowTitle() const noexcept
{
    return _windowTitle;
}

void Terminal::setProgress(Progress progress)
{
    // Lock-free for the same reason as setWindowTitle() above: the OSC 9;4 dispatch in Screen reaches
    // this from inside writeToScreen()'s _stateMutex hold, and _stateMutex is not recursive.
    _progress = progress;
    _eventListener.progressChanged(progress);
    _extensionEvents.post([&] { return StatusLineEvents::ProgressChanged { .progress = progress }; });
}

void Terminal::applyProgramStatus(ProgramStatusReport const& report)
{
    // Lock-free for the reason setProgress() gives: reached from the OSC 7501 dispatch in Screen, inside
    // writeToScreen()'s _stateMutex hold.
    std::visit(
        [this](auto const& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ProgramStatusRecord>)
                announceProgramStatus(_programStatus.apply(value));
            else if constexpr (std::is_same_v<T, ProgramStatusClear>)
                announceProgramStatus(_programStatus.clear(value.id));
            else
                reply(programStatusDetectionReply());
        },
        report);
}

void Terminal::markProcessExited()
{
    auto const _ = std::lock_guard { *this };
    announceProgramStatus(_programStatus.processExited());
}

void Terminal::replaceProgramStatus(std::vector<ProgramStatusRecord> records)
{
    // Lock-free: the mirror applying a host's frame holds _stateMutex, as it does for setProgress().
    announceProgramStatus(_programStatus.replaceAll(std::move(records)));
}

void Terminal::announceProgramStatus(ProgramStatusChange const& change)
{
    if (!change.empty())
        _eventListener.programStatusChanged(change);
}

ContextTransition Terminal::applyContextCommand(ContextCommand const& command)
{
    auto const transition = _contexts.apply(command);

    // Only when something a frontend can SEE moved. The systemd hot path -- a shell context
    // re-announced on every prompt with byte-identical metadata -- is an Updated transition that
    // usually changes nothing, and notifying for it would be a redraw per prompt.
    if (transition.change == ContextChange::Yes)
    {
        publishActiveContext();
        recordForeignDirectory();

        // Lock-free for the same reason setProgress() above is: the OSC 3008 dispatch in Screen reaches
        // this from inside writeToScreen()'s _stateMutex hold, and _stateMutex is not recursive.
        _eventListener.contextChanged(_contexts.activeId());
    }

    return transition;
}

void Terminal::publishActiveContext() noexcept
{
    // Every screen, not just the current one: an application may switch screens or flip pages between
    // one context announcement and the next, and a line written on whichever screen is current then has
    // to be stamped with the context that is actually in effect. The same shape hardReset() uses when
    // it walks _pages.
    auto const id = _contexts.activeId();
    for (auto& page: _pages)
        page->setActiveContextId(id);
    _hostWritableStatusLineScreen.setActiveContextId(id);
    _indicatorStatusScreen.setActiveContextId(id);
}

void Terminal::adoptContext(TerminalContext record)
{
    _contexts.adopt(std::move(record));
}

void Terminal::setContextChain(std::span<ContextId const> ids)
{
    // Not through publishActiveContext(): the rows' contexts arrive with the rows.
    auto const before = _contexts.activeId();
    _contexts.setChain(ids);
    if (_contexts.activeId() != before)
        _eventListener.contextChanged(_contexts.activeId());
}

void Terminal::adoptCommandBlock(CommandBlockRecord record, AdoptMode mode)
{
    auto const role = _replicaRole.load(std::memory_order_acquire);
    // The host's clock read the record's instants; they are kept as this terminal's clock reads them.
    auto const toThisClock = [this](chrono::system_clock::time_point hostInstant) {
        return onThisClock(hostInstant);
    };
    record.promptStartedAt = record.promptStartedAt.transform(toThisClock);
    record.commandStartedAt = record.commandStartedAt.transform(toThisClock);
    // The daemon judged the directories against its own machine; on another one they name directories there.
    // A record that names none keeps saying so.
    if (role == ReplicaRole::OtherMachine)
        for (auto* const directory: { &record.workingDirectory, &record.endWorkingDirectory })
            if (!directory->path.empty())
                directory->locality = ContextLocality::Foreign;
    // Not through publishActiveBlock(): the rows' ids arrive with the rows.
    if (auto const summary = commandBlocks().adopt(std::move(record), mode))
    {
        _eventListener.commandBlockFinished(*summary);
        // A live command the host saw end, as endCommand() is told of one here: it may have switched
        // branches.
        if (_gitHead)
            _gitHead->invalidate();
    }
}

chrono::system_clock::time_point Terminal::sessionEpoch() const noexcept
{
    return _sessionEpoch;
}

void Terminal::adoptSessionEpoch(chrono::system_clock::time_point epoch) noexcept
{
    // Held before it is floored: flooring an instant near the clock's earliest is itself an overflow.
    _sessionEpoch = chrono::floor<chrono::seconds>(heldInstant(epoch));
}

void Terminal::adoptHostClock(chrono::system_clock::time_point hostNow) noexcept
{
    _hostClockOffset = heldInstant(_clocks.wall.now()) - heldInstant(hostNow);
}

chrono::system_clock::time_point Terminal::onThisClock(
    chrono::system_clock::time_point hostInstant) const noexcept
{
    // Another machine's clock read the instant; this one reads it against its own. On the same machine both
    // read one clock, and the measured difference would be nothing but the reading's transit. Held first, so
    // the sum stays inside what the clock can count: each part is within 2^62 ns of the Unix epoch.
    if (_replicaRole.load(std::memory_order_acquire) != ReplicaRole::OtherMachine)
        return hostInstant;
    return heldInstant(heldInstant(hostInstant) + _hostClockOffset);
}

// {{{ Command blocks
namespace
{
    /// Whether the block whose rows reach the first row the grid holds may be headed there for want of a
    /// mark.
    enum class TopRowHead : uint8_t
    {
        Refused = 0, ///< Its head may have left the history, which ids from a dead generation cannot tell.
        Allowed,     ///< Its head is known to lie in the grid still: an erase took it, not eviction.
    };

    /// Offers @p offer the rows of @p grid that may head a command block, in the head rule's order: each
    /// block's rows carrying Marked, lowest first; then those carrying OutputStart, lowest first; then, top
    /// down, the topmost row of each run of rows carrying one id. @p offer keeps the first row it takes for a
    /// block, so the order is the rule.
    ///
    /// A block's head is a row of the block that carries the mark the block began with: ;A's Marked, or --
    /// for a block a ;C minted -- ;C's OutputStart. Marked wins wherever it is, so a block is never headed at
    /// its ;C row while its prompt survives: the walk offers Marked rows as it meets them, and only notes
    /// OutputStart rows, to offer them once it is over. (A block whose prompt row went but whose ;C row
    /// survived is headed at that row: still a row of the block.)
    ///
    /// Bottom up, and the LOWEST such row wins: a row ABOVE the head can carry the block's id. The cursor
    /// hands the running block to a row it reaches that belongs to none -- `clear` homes it onto the page's
    /// top row before ED 2 scrolls the page into the history -- and an erase under the cursor starts a row
    /// afresh in the running block while keeping its marks, an older prompt's among them.
    ///
    /// A block with neither mark left can still hold rows: an erase blanks a row away from the cursor to no
    /// block, its marks with it -- ED 1 inside a command takes the prompt and the output start above the
    /// cursor. Such a block is headed at the topmost row still carrying its id, which is why the runs are
    /// offered top down; a row carrying no block does not end a run.
    /// @param grid The primary screen's grid.
    /// @param unplaced How many blocks wait for a head. The walk stops once Marked rows headed them all: what
    ///        lies above cannot move one.
    /// @param topRow Whether the block whose rows reach the first row the grid holds may take its topmost
    /// row.
    /// @param offer Called as offer(CommandBlockId, int64_t stableId); returns HeadPlacement::Placed when it
    ///        took the row for the block.
    /// @return How many rows the walk looked at.
    template <typename Offer>
    size_t offerHeadRows(Grid const& grid, size_t unplaced, TopRowHead topRow, Offer const& offer)
    {
        auto outputStarts = std::vector<std::pair<CommandBlockId, int64_t>> {};
        // Each run of rows carrying one id, bottom up, as that id and the run's topmost row.
        auto runs = std::vector<std::pair<CommandBlockId, int64_t>> {};
        auto walked = size_t { 0 };
        auto const rowsBottomUp =
            std::views::iota(unbox<int>(grid.addressableTop()), unbox<int>(grid.pageSize().lines))
            | std::views::reverse;
        for (auto const row: rowsBottomUp)
        {
            if (unplaced == 0)
                break;
            ++walked;
            auto const offset = LineOffset::cast_from(row);
            auto const& line = grid.lineAt(offset);
            auto const id = line.blockId();
            if (!id)
                continue;
            auto const stableId = grid.stableLineIdOf(offset);
            if (line.isFlagEnabled(LineFlag::Marked) && offer(id, stableId) == HeadPlacement::Placed)
                --unplaced;
            if (line.isFlagEnabled(LineFlag::OutputStart))
                outputStarts.emplace_back(id, stableId);
            if (!runs.empty() && runs.back().first == id)
                runs.back().second = stableId;
            else
                runs.emplace_back(id, stableId);
        }
        for (auto const& [id, stableId]: outputStarts)
            offer(id, stableId);

        auto const reachesTop =
            !runs.empty() && runs.back().second == grid.stableLineIdOf(grid.addressableTop());
        auto const topBlock =
            reachesTop && topRow == TopRowHead::Refused ? runs.back().first : CommandBlockId {};
        for (auto const& [id, stableId]: runs | std::views::reverse)
            if (id != topBlock)
                offer(id, stableId);
        return walked;
    }

    /// Whether @p record's head may have stopped being the one the head rule gives, though row identity held.
    ///
    /// A scroll moved or dropped what its id named (@p moved), whatever the row there carries now. Or its row
    /// is still in the grid -- one below the floor scrolled out of the history, which is what "evicted" says
    /// -- but carries another block's id or none, or no mark a head is found by: an erase or a new prompt
    /// took the row out of the block, or its marks off it, and the cursor may have handed it back since. A
    /// head of last resort carries no mark either, and is looked at again with the rest. A head recorded in
    /// another generation keeps it until row identity next dies.
    [[nodiscard]] bool isHeadInDoubt(Grid const& grid,
                                     CommandBlockRecord const& record,
                                     std::optional<StableIdSpan> const& moved) noexcept
    {
        if (record.headIdGeneration != grid.stableIdGeneration())
            return false;
        if (moved && moved->contains(record.headStableId))
            return true;
        auto const head = grid.lineOffsetOf(record.headStableId);
        if (!head)
            return false;
        auto const& line = grid.lineAt(*head);
        return line.blockId() != record.id || !line.flags().any(BlockHeadLineFlags);
    }

    /// @return Whether a block target names its block by the record (a pinned id, or the last one finished)
    ///         rather than by a row the target stands on.
    [[nodiscard]] constexpr bool namesBlockByRecord(CommandBlockTarget target, CommandBlockId pinned) noexcept
    {
        return pinned != CommandBlockId {} || target == CommandBlockTarget::Last;
    }
} // namespace

CommandBlockStore& Terminal::commandBlocks() noexcept
{
    syncCommandBlockHeads();
    return _commandBlocks;
}

CommandBlockStore const& Terminal::commandBlocks() const noexcept
{
    syncCommandBlockHeads();
    return _commandBlocks;
}

void Terminal::syncCommandBlockHeads() const noexcept
{
    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    auto const sameGeneration = generation == _commandBlockHeadsGeneration;
    auto const floor = grid.stableRangeFloor();
    auto const formerFloor = std::exchange(_commandBlockHeadsFloor, floor);

    auto const erased = reheadCommandBlocks();

    // A head the history let go of since the last look stays where it was, below the floor, and one whose
    // block an erase left no row of is put just below it, which is what "evicted" says -- and a replica is
    // told so, so the record has changed. Within one generation only: a rebuild renumbers the rows, and a
    // replica is sent the whole store afresh. The change listener is not told
    // (CommandBlockStore::headsLeft()), so this read raises no event: it may be any thread's.
    if (sameGeneration && (floor > formerFloor || !erased.empty()))
    {
        _commandBlockHeadWork += _commandBlocks.size();
        _commandBlocks.headsLeft(generation, formerFloor, floor, erased);
    }
}

std::vector<CommandBlockId> Terminal::reheadCommandBlocks() const noexcept
{
    auto const& screen = primaryScreen();
    auto const& grid = screen.grid();
    auto const generation = grid.stableIdGeneration();
    auto const rowChanges = screen.blockRowChanges();
    auto const moved = std::exchange(_commandBlockRowsMoved, std::nullopt);

    if (generation == _commandBlockHeadsGeneration && rowChanges == _commandBlockHeadsRowChanges && !moved)
        return {};
    auto const rowsRenamed = generation != _commandBlockHeadsGeneration;
    _commandBlockHeadsGeneration = generation;
    _commandBlockHeadsRowChanges = rowChanges;
    if (_commandBlocks.size() == 0)
        return {}; // no shell integration, nothing to re-head: no reason to walk the scrollback

    if (rowsRenamed)
    {
        // The rebuild carried every row's block id, so every block is headed afresh by the rule, among the
        // rows of the new generation. Ids from the old one say nothing here, not even whether a head had
        // scrolled out of the history before, so the rule goes by where rows lie alone, and misjudges:
        // - a block whose prompt row scrolled out but whose ;C row survived: headed at that row, not
        //   evicted -- an OutputStart under an evicted prompt reads as one that began the block;
        // - a block whose head scrolled out, with no mark left and nothing of it left above the rows an erase
        //   blanked (ED 1 inside long output, no history kept): headed at its topmost surviving row, not
        //   evicted;
        // - a block an erase took the head of, with no mark left and its topmost row the first the grid
        //   holds (`\e[H\e[J` with no history): evicted, not headed at that row.
        // Within one generation each comes out right (below); the next reflow decides it again, as here.
        auto const unplaced =
            std::ranges::count_if(_commandBlocks.records(), [generation](CommandBlockRecord const& record) {
                return record.headIdGeneration != generation;
            });
        auto const place = [this, generation](CommandBlockId id, int64_t stableId) {
            return _commandBlocks.placeHead(id, stableId, generation);
        };
        _commandBlockHeadWork +=
            _commandBlocks.size()
            + offerHeadRows(grid, static_cast<size_t>(unplaced), TopRowHead::Refused, place);

        // A block still unplaced lost its head before the rebuild, and the record says so with an id below
        // the new floor, which is what "evicted" means everywhere else.
        _commandBlocks.retireUnplacedHeads(generation, grid.stableRangeFloor() - 1);
        return {};
    }

    // Within one generation an erase, a new prompt or a scroll inside margins has changed rows, and stable
    // ids still compare: a head below the floor scrolled out, and one a scroll moved away from, or still in
    // range but no longer its block's head, is headed afresh by the same rule -- at the first row the grid
    // holds too, if that is the topmost row the block has left.
    auto inDoubt = std::vector<CommandBlockId> {};
    for (auto const& record: _commandBlocks.records())
        if (isHeadInDoubt(grid, record, moved))
            inDoubt.push_back(record.id); // ascending, as the records are
    _commandBlockHeadWork += _commandBlocks.size();
    if (inDoubt.empty())
        return {};
    // A block headed is marked so rather than erased from the list, which would cost a read that heads many
    // of them a shift of the list per block.
    auto placements = std::vector<HeadPlacement>(inDoubt.size(), HeadPlacement::Unchanged);
    auto const place = [this, generation, &inDoubt, &placements](CommandBlockId id, int64_t stableId) {
        auto const it = std::ranges::lower_bound(inDoubt, id);
        if (it == inDoubt.end() || *it != id)
            return HeadPlacement::Unchanged;
        auto& placement = placements[static_cast<size_t>(std::distance(inDoubt.begin(), it))];
        if (placement == HeadPlacement::Placed)
            return HeadPlacement::Unchanged; // the first row offered for a block heads it
        placement = HeadPlacement::Placed;
        _commandBlocks.updateHeadPosition(id, stableId, generation);
        return placement;
    };
    _commandBlockHeadWork += offerHeadRows(grid, inDoubt.size(), TopRowHead::Allowed, place);

    // A block none of whose rows is left: evicted, as after a rebuild.
    auto evicted = std::vector<CommandBlockId> {};
    for (auto const& [id, placement]: std::views::zip(inDoubt, placements))
    {
        if (placement == HeadPlacement::Placed)
            continue;
        _commandBlocks.updateHeadPosition(id, grid.stableRangeFloor() - 1, generation);
        evicted.push_back(id);
    }
    return evicted;
}

void Terminal::placeCommandBlockHeads(std::span<CommandBlockId> blocks)
{
    if (blocks.empty())
        return;
    syncCommandBlockHeads();

    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    // Narrowed where they lie, sorted for the lookups below, down to the distinct ids with a record and no
    // head in this generation -- usually none: the row a keystroke rewrites heads a block headed already.
    auto const headedOrUnknown = [&](CommandBlockId id) {
        auto const* const record = _commandBlocks.find(id);
        return record == nullptr || record->headIdGeneration == generation;
    };
    std::ranges::sort(blocks);
    auto const distinct = std::span { blocks.begin(), std::ranges::unique(blocks).begin() };
    auto const unplaced =
        std::span { distinct.begin(), std::ranges::remove_if(distinct, headedOrUnknown).begin() };
    if (unplaced.empty())
        return;

    // Refused, as after a rebuild: nothing here says whether the head of a block reaching the first row the
    // grid holds scrolled out of the history before its rows got here. placeHead() keeps the first row it
    // is offered for a block, so the order of the offers is the rule.
    auto const place = [this, generation, unplaced](CommandBlockId id, int64_t stableId) {
        if (!std::ranges::binary_search(unplaced, id))
            return HeadPlacement::Unchanged;
        return _commandBlocks.placeHead(id, stableId, generation);
    };
    _commandBlockHeadWork += offerHeadRows(grid, unplaced.size(), TopRowHead::Refused, place);
}

void Terminal::retireCommandBlockHeads(std::span<CommandBlockId const> blocks)
{
    if (blocks.empty())
        return;
    syncCommandBlockHeads();

    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    for (auto const id: blocks)
        if (auto const* const record = _commandBlocks.find(id);
            record != nullptr && record->headIdGeneration != generation)
            _commandBlocks.updateHeadPosition(id, grid.stableRangeFloor() - 1, generation);
}

void Terminal::commandBlockRowsMoved(RowMove const& move) noexcept
{
    _commandBlockRowsMoved =
        _commandBlockRowsMoved ? _commandBlockRowsMoved->joinedWith(move.changed) : move.changed;
}

void Terminal::foldRowsMoved(RowMove const& move)
{
    _foldState.follow(move);
    invalidateFoldRanges();
}

CommandBlockRecord const* Terminal::commandBlockAt(LineOffset gridLine) const
{
    auto const& grid = primaryScreen().grid();
    if (gridLine < grid.addressableTop() || gridLine >= boxed_cast<LineOffset>(grid.pageSize().lines))
        return nullptr;

    // A row an erase blanked while the cursor was elsewhere carries no block until the cursor reaches it
    // again. That is no information, not "outside any block": the fold it lies in still names its block, the
    // one its head row carries -- read from the head directly, so a head that lost its own id names none.
    // Outside every fold, the block still unfinished may be the one: it has no fold yet.
    auto id = grid.lineAt(gridLine).blockId();
    if (!id)
    {
        auto const range = foldCovering(foldRanges(), grid.stableLineIdOf(gridLine));
        if (!range)
            return unfinishedBlockReaching(gridLine);
        auto const head = grid.lineOffsetOf(range->headStableId);
        if (!head)
            return nullptr;
        id = grid.lineAt(*head).blockId();
    }
    return !id ? nullptr : commandBlocks().find(id);
}

CommandBlockRecord const* Terminal::unfinishedBlockReaching(LineOffset gridLine) const
{
    auto const* const record = commandBlocks().current();
    auto const& screen = primaryScreen();
    if (!record || gridLine > screen.cursor().position.line)
        return nullptr;

    // A head the history evicted lies above every row the grid holds. One recorded in another generation --
    // a mirror's record before its rows arrive -- names no row to measure from, so it reaches none.
    auto const& grid = screen.grid();
    if (auto const head = headOffsetOf(grid, *record))
        return gridLine >= *head ? record : nullptr;
    return record->headIdGeneration == grid.stableIdGeneration() ? record : nullptr;
}

std::expected<CommandBlockRows, CommandBlockActionError> Terminal::commandBlockRowsAt(
    LineOffset anchor, CommandBlockPart part) const
{
    // How far the block has got -- whether it is all prompt, and whether an end mark below its rows ends its
    // output -- is its record's to say.
    auto const* const record = commandBlockAt(anchor);
    auto const progress = record != nullptr ? progressOf(*record) : CommandProgress::Unreported;

    // Qualified: the member of the same name hides the free function.
    return vtbackend::commandBlockRows(AttributedBlockRows { *this }, anchor, part, progress);
}

std::optional<LineOffset> Terminal::commandBlockAnchor(CommandBlockRecord const& record) const
{
    return bottomRowOfBlock(AttributedBlockRows { *this }, record.id);
}

std::optional<LineOffset> Terminal::commandBlockAnchor(CommandBlockTarget target, CommandBlockId pinned) const
{
    // A block named by its record -- a pinned id, or the last one finished -- stands on its lowest row, which
    // commandBlockAt() names as that block for as long as its record lasts.
    if (namesBlockByRecord(target, pinned))
    {
        auto const* const record = targetedCommandBlock(target, pinned);
        return record ? commandBlockAnchor(*record) : std::nullopt;
    }

    switch (target)
    {
        case CommandBlockTarget::Pointer:
            // Over any page on display but the primary one, the pointer is over no block.
            if (displayedScreenType() != ScreenType::Primary)
                return std::nullopt;
            return currentMouseGridPosition().transform([](CellLocation position) { return position.line; });
        case CommandBlockTarget::Cursor: {
            // In Insert mode the cursor is the terminal's, on the cursor's page -- the main display's, not
            // the status line's that DECSASD may have selected; in the others it is the vi cursor, on the
            // page on display. While that page is another than the primary one, the shell's cursor is the
            // primary screen's own.
            if (inputHandler().mode() == ViMode::Insert)
                return isPrimaryScreen() ? pageAt(_cursorPage).cursor().position.line
                                         : primaryScreen().realCursorPosition().line;
            return displayedScreenType() == ScreenType::Primary ? _viCommands.cursorPosition.line
                                                                : primaryScreen().realCursorPosition().line;
        }
        case CommandBlockTarget::Last: break; // named by its record above
    }
    return std::nullopt;
}

std::expected<CommandBlockRows, CommandBlockActionError> Terminal::commandBlockRows(
    CommandBlockTarget target, CommandBlockPart part, CommandBlockId pinned) const
{
    auto const anchor = commandBlockAnchor(target, pinned);
    if (!anchor)
        return std::unexpected(CommandBlockActionError::NoBlock);

    // An action copies or selects no blank line after the text: the part ends with its last line that holds
    // any up to where the part ends on it. Never above its first line, so a part that is all blank is still
    // that part. One that is only a line its end mark opened has no line at all.
    auto const& grid = primaryScreen().grid();
    return commandBlockRowsAt(*anchor, part)
        .and_then([&grid](CommandBlockRows rows) -> std::expected<CommandBlockRows, CommandBlockActionError> {
            auto const lines = logicalLinesOf(grid, rows);
            if (lines.empty())
                return std::unexpected(CommandBlockActionError::EmptyPart);
            auto const holdsText = [&grid](CommandBlockLine const& line) {
                return !grid.isLogicalLineBlank(line.head, line.last, line.begin, line.end);
            };
            auto const kept = std::ranges::find_if(lines | std::views::reverse, holdsText);
            auto const& last = kept != std::ranges::rend(lines) ? *kept : lines.front();
            rows.last = last.last;
            rows.endColumn = last.end;
            return rows;
        });
}

std::expected<void, CommandBlockActionError> Terminal::selectCommandBlock(CommandBlockTarget target,
                                                                          CommandBlockPart part,
                                                                          CommandBlockId pinned)
{
    // A selection lands on the page on display, and only the primary screen holds blocks.
    if (displayedScreenType() != ScreenType::Primary)
        return std::unexpected(CommandBlockActionError::NoBlock);
    return commandBlockRows(target, part, pinned).transform([this](CommandBlockRows rows) {
        if (rows.firstColumn == ColumnOffset(0) && !rows.endColumn)
        {
            selectLines(rows.first, rows.last);
            return;
        }

        // An end mark splits a line of the part: select through the columns it leaves the part, linearly,
        // which takes every row between whole.
        auto const& grid = primaryScreen().grid();
        auto const from = grid.cellAtLogicalColumn(rows.first, rows.firstColumn, rows.last);
        auto const to =
            rows.endColumn
                ? grid.cellAtLogicalColumn(std::max(grid.logicalLineHead(rows.last), rows.first),
                                           *rows.endColumn - ColumnOffset(1),
                                           rows.last)
                : CellLocation { .line = rows.last,
                                 .column = boxed_cast<ColumnOffset>(pageSize().columns) - ColumnOffset(1) };
        selectThrough(std::make_unique<LinearSelection>(_selectionHelper, from, selectionUpdatedHelper()),
                      to);
    });
}

std::expected<std::string, CommandBlockActionError> Terminal::commandBlockText(CommandBlockTarget target,
                                                                               CommandBlockPart part,
                                                                               CaptureRendition rendition,
                                                                               CommandBlockId pinned) const
{
    return commandBlockRows(target, part, pinned).transform([this, rendition](CommandBlockRows rows) {
        // A line per logical line, each read once, as the semantic-block query reply reads it.
        auto const& grid = primaryScreen().grid();
        auto text = std::string {};
        text.reserve(unbox<size_t>(rows.last - rows.first + LineOffset(1))
                     * (unbox<size_t>(grid.pageSize().columns) + 1));
        for (auto const& line: logicalLinesOf(grid, rows))
        {
            if (line.head != rows.first)
                text += '\n';
            text += grid.logicalLineText(line.head, line.last, line.begin, line.end, rendition);
        }
        return text;
    });
}

bool Terminal::hasCommandBlockRows(CommandBlockTarget target, CommandBlockId pinned) const
{
    // A row the target stands on is the record's own (targetedCommandBlock()).
    auto const* const record = targetedCommandBlock(target, pinned);
    if (record == nullptr)
        return false;
    if (!namesBlockByRecord(target, pinned))
        return true;
    // Its head row answers while it is on the grid. Once the head has left the history, the rest of the block
    // may still be there: its lowest row is looked for, and the walk stops there.
    return headOffsetOf(primaryScreen().grid(), *record).has_value()
           || commandBlockAnchor(*record).has_value();
}

CommandBlockRecord const* Terminal::targetedCommandBlock(CommandBlockTarget target,
                                                         CommandBlockId pinned) const
{
    if (pinned != CommandBlockId {})
        return commandBlocks().find(pinned);
    // The record outlives its rows: a command line can still be copied once its output scrolled away.
    if (target == CommandBlockTarget::Last)
        return commandBlocks().lastFinished();
    auto const anchor = commandBlockAnchor(target, pinned);
    return anchor ? commandBlockAt(*anchor) : nullptr;
}

std::expected<void, CommandBlockActionError> Terminal::clearToPrompt()
{
    // Rows the user cannot see would go: another page is on display, not the primary screen's history.
    if (displayedScreenType() != ScreenType::Primary)
        return std::unexpected(CommandBlockActionError::NoBlock);

    auto& screen = primaryScreen();
    return commandBlockRowsAt(screen.realCursorPosition().line, CommandBlockPart::Prompt)
        .transform([&](CommandBlockRows prompt) {
            auto const evictedBefore = screen.grid().evictedRowCount();
            auto const pageShift = screen.dropRowsAbove(prompt.first);
            if (screen.grid().evictedRowCount() != evictedBefore)
                rowsDropped(pageShift);
        });
}

Terminal::BlockHeadRow Terminal::cursorBlockHead() const noexcept
{
    auto const& screen = primaryScreen();
    auto const& grid = screen.grid();
    auto const head = grid.logicalLineHead(screen.cursor().position.line);
    return BlockHeadRow { .stableId = grid.stableLineIdOf(head), .generation = grid.stableIdGeneration() };
}

void Terminal::beginCommandBlock()
{
    auto const [headStableId, headIdGeneration] = cursorBlockHead();
    auto const idEpoch = _commandBlocks.idEpoch();
    // The new record is the current one; the one before learns where its command left the shell.
    _commandBlocks.promptStarted(headStableId, headIdGeneration, workingDirectorySnapshot());
    publishActiveBlock(idEpoch);
    // A new prompt ends whatever was running: its working and blocked program-status records go
    // (program-status rev 0.4, "Lifetime"); idle, done and error stay.
    announceProgramStatus(_programStatus.promptStarted());
}

void Terminal::beginCommand(std::optional<std::string> commandLine, CommandLineSource source)
{
    auto const [headStableId, headIdGeneration] = cursorBlockHead();
    auto const idEpoch = _commandBlocks.idEpoch();
    auto const* const context = _contexts.active();
    _commandBlocks.commandStarted(CommandStart {
        .commandLine = std::move(commandLine),
        .source = source,
        .workingDirectory = workingDirectorySnapshot(),
        .context = _contexts.activeId(),
        .contextType = context ? context->type : ContextType::None,
        .headStableId = headStableId,
        .headIdGeneration = headIdGeneration,
    });
    // A ;C with no ;A before it minted a record of its own, whose rows start here.
    publishActiveBlock(idEpoch);
}

void Terminal::publishActiveBlock(uint64_t idEpochBefore) noexcept
{
    if (_commandBlocks.idEpoch() != idEpochBefore)
        primaryScreen().forgetBlockIds();
    primaryScreen().setActiveBlockId(_commandBlocks.currentId());
}

void Terminal::endCommand(int exitCode, ContextOutcome outcome)
{
    // Raised from inside the parse, under _stateMutex, exactly as progressChanged() is.
    if (auto const summary = _commandBlocks.commandFinished(exitCode, outcome))
        _eventListener.commandBlockFinished(*summary);
    // The command may have switched branches; the next frame asks.
    if (_gitHead)
        _gitHead->invalidate();
}

void Terminal::commandContextStarted(ContextId started)
{
    auto const* const context = _contexts.find(started);
    _commandBlocks.commandContextStarted(started, context ? context->parent : ContextId {});
}

void Terminal::enrichCommandOutcome(ContextId ended, ContextOutcome outcome)
{
    _commandBlocks.enrichOutcome(ended, outcome);
}

WorkingDirectorySnapshot Terminal::workingDirectorySnapshot() const
{
    // Display, not Spawn: the record says where the command RAN, which is the right answer even behind a
    // boundary. The locality travels with it, so a later "open locally" can still refuse.
    auto resolved = resolveWorkingDirectory(CwdPurpose::Display);
    if (!resolved)
        return {};
    return WorkingDirectorySnapshot { .path = std::move(resolved->path), .locality = resolved->locality };
}

void Terminal::setCurrentWorkingDirectory(string const& url)
{
    auto parsed = parseFileUrl(url);
    if (!parsed)
    {
        terminalLog()("Ignoring a working directory whose path holds a NUL, so it names no directory: \"{}\"",
                      core::escape(url));
        return;
    }
    // Changed or not: a shell re-reports at every prompt, which is when a branch switch shows.
    if (_gitHead)
        _gitHead->invalidate();
    auto const changed = *parsed != _currentWorkingDirectory;
    _currentWorkingDirectory = *std::move(parsed);
    recordForeignDirectory();
    if (changed)
        _eventListener.workingDirectoryChanged();
    _extensionEvents.post(
        [&] { return StatusLineEvents::CwdChanged { .cwd = _currentWorkingDirectory.path }; });
}

std::string Terminal::gitBranch() const
{
    return _gitHead ? _gitHead->branch() : std::string {};
}

void Terminal::refreshGitHead()
{
    if (!_gitHead || !containsItem<StatusLineDefinitions::Git>(_indicatorStatusLineDefinition))
        return;
    // Only a directory provably on this machine: another machine's path names another repository, or none.
    _gitHead->refresh(
        resolveWorkingDirectory(CwdPurpose::OpenLocally)
            .and_then([](ResolvedWorkingDirectory const& resolved) -> std::optional<std::filesystem::path> {
                // A path this platform cannot spell has no branch rather than a wrong one.
                auto path = core::pathFromUtf8(resolved.path);
                if (!path)
                    return std::nullopt;
                return *std::move(path);
            }));
}

optional<ResolvedWorkingDirectory> Terminal::resolveWorkingDirectory(CwdPurpose purpose) const
{
    switch (_replicaRole.load(std::memory_order_acquire))
    {
        case ReplicaRole::None:
            return vtbackend::resolveWorkingDirectory(
                _contexts, _currentWorkingDirectory, localIdentity(), purpose);
        case ReplicaRole::SameMachine:
            // The daemon's contexts name machines by the ids it derives from theirs, so this machine is
            // named the same way to compare.
            return vtbackend::resolveWorkingDirectory(
                _contexts, _currentWorkingDirectory, _replicaIdentity.view(), purpose);
        case ReplicaRole::OtherMachine: break;
    }

    // Every directory the application reported is one on the daemon's machine. Where the user is still
    // shows; nothing here opens or spawns into it.
    if (needsLocalPath(purpose))
        return std::nullopt;
    auto shown =
        vtbackend::resolveWorkingDirectory(_contexts, _currentWorkingDirectory, localIdentity(), purpose);
    if (shown)
        shown->locality = ContextLocality::Foreign;
    return shown;
}

optional<string_view> Terminal::fileUrlLocalHost() const noexcept
{
    if (_replicaRole.load(std::memory_order_acquire) == ReplicaRole::OtherMachine)
        return std::nullopt;
    return localIdentity().hostname;
}
// }}}

Progress Terminal::resolvedProgress() const
{
    auto const l = std::lock_guard { *this };
    return _progress;
}

std::string Terminal::resolvedWindowTitle() const
{
    auto const l = std::lock_guard { *this };
    return _windowTitle;
}

std::string Terminal::decodeTitle(std::string_view raw) const
{
    // Under SetHex the OSC title argument is a hex string (xterm's title mode 0). A malformed hex string
    // (odd length or a non-hex digit) leaves fromHexString empty; fall back to the raw bytes then, as
    // there is nothing better to decode.
    if (isTitleModeEnabled(TitleModeFeature::SetHex))
        if (auto decoded = core::fromHexString(raw); decoded.has_value())
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
    auto const l = std::lock_guard { *this };
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
    changeInputEncoding([&](InputGenerator& input) { input.setCursorKeysMode(keyMode); });
    publishBindingModes();
}

void Terminal::setMode(AnsiMode mode, bool enable)
{
    if (!isValidAnsiMode(static_cast<unsigned int>(mode)))
        return;

    if (mode == AnsiMode::KeyboardAction)
    {
        if (enable)
            pushStatusDisplay(StatusNotice::KeyboardLocked, StatusDisplayType::Indicator);
        else
            popStatusDisplay(StatusNotice::KeyboardLocked);
    }

    // LNM has two halves. The output half (LF also returns the carriage) reads the mode bit in
    // Screen::linefeed(); the input half (Return sends CR LF) lives in the input generator, which
    // has no view of the mode register and must therefore be told.
    if (mode == AnsiMode::AutomaticNewLine)
        changeInputEncoding([&](InputGenerator& input) { input.setAutomaticNewLineMode(enable); });

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
                mainDisplayMargin().horizontal =
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

            // "... and the Active Position to be set to the display origin (1,1)" (DEC STD 070, DECCOLM), as
            // xterm's set_column_mode() and Windows Terminal do, with DECNCSM too. Before the resize: a
            // cursor left past the new width would have the narrowing wrap the blanks up to it onto a row of
            // their own.
            _currentScreen->moveCursorTo(LineOffset(0), ColumnOffset(0));

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
            for (auto& category: core::log::get())
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
            // The wheel's cursor keys take DECCKM's form, as the keyboard's do.
            updateWheelMode();
            break;
        case DECMode::BracketedPaste: setBracketedPaste(enable); break;
        // The four coordinate encodings are one mutually-exclusive setting; @see
        // MouseCoordinateModes for the rule and its source.
        case DECMode::MouseExtended:  // 1005
        case DECMode::MouseSGR:       // 1006
        case DECMode::MouseURXVT:     // 1015
        case DECMode::MouseSGRPixels: // 1016
            setMouseCoordinateMode(mode, enable);
            break;
        case DECMode::MousePassiveTracking:
            changeInputEncoding([&](InputGenerator& input) { input.setPassiveMouseTracking(enable); });
            setMode(DECMode::MouseSGR, enable);                    // SGR is required.
            setMode(DECMode::MouseProtocolButtonTracking, enable); // ButtonTracking is default
            break;
        case DECMode::MouseAlternateScroll:
            // Alternate scroll's on/off gate -- whether the wheel becomes cursor keys over a page without
            // history -- as xterm's alternateScroll resource and mode are, and on from power-on, as Windows
            // Terminal's and foot's are. It picks no key form: DECCKM does. Into the register first: the
            // wheel mode is derived from it.
            _modes.set(mode, enable);
            updateWheelMode();
            break;
        case DECMode::FocusTracking: setGenerateFocusEvents(enable); break;
        case DECMode::UsePrivateColorRegisters: _usePrivateColorRegisters = enable; break;
        case DECMode::VisibleCursor:
            // One enable for the main and the status display. DEC STD 070 ch. 14 §2.3 item 14 has "Each of
            // the Main and Status Display cursors may be individually enabled or disabled", but the terminals
            // that have a host-writable status line keep one: xterm its screen->cursor_set (charproc.c),
            // which its switch to the status line neither saves nor restores, and mlterm its parser's
            // is_visible_cursor. Contour follows them: an application written against xterm hides the cursor
            // with ?25l and then writes the status line through DECSASD, and a cursor of the status line's
            // own would show there.
            setCursorVisibility(enable);
            break;
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
                showPage(_cursorPage);
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
        case DECMode::BackarrowKey:
            changeInputEncoding([&](InputGenerator& input) { input.setBackarrowKeyMode(enable); });
            break;
        case DECMode::Win32InputMode:
            changeInputEncoding([&](InputGenerator& input) { input.setWin32InputMode(enable); });
            break;
        case DECMode::SemanticBlockProtocol:
            if (enable)
            {
                _semanticBlockTracker.enable(_commandBlocks);
                auto const& t = *_semanticBlockTracker.token();
                reply("\033P>2034;1b{};{};{};{}\033\\", t[0], t[1], t[2], t[3]);
            }
            else
                _semanticBlockTracker.disable();
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
        mainDisplayMargin().vertical.from = sanitizedTop;
        mainDisplayMargin().vertical.to = sanitizedBottom;
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
        mainDisplayMargin().horizontal.from = sanitizedLeft;
        mainDisplayMargin().horizontal.to = sanitizedRight;
    }
}

void Terminal::clearScreen()
{
    pageAt(_cursorPage).clearScreen();
}

void Terminal::setAlternateScreen(DECMode mode, bool enable)
{
    // Modes 47, 1047 and 1049 all switch between the primary and alternate screen buffers; they differ
    // only in whether they save the cursor first, whether they carry it across, and whether they erase the
    // alternate page on the way in or out. That policy is data (alternateScreenBehavior), so the switch below
    // is a single path, modelled on xterm's ToAlternate / FromAlternate (charproc.c).
    auto const behavior = alternateScreenBehavior(mode);
    Require(behavior.has_value());

    // The guards ask which buffer is in use, as xterm's whichBuf and mlterm's edit identity do: the alternate
    // screen is entered from whichever page the cursor is on.
    if (enable && !isAlternateScreenBufferInUse())
    {
        // xterm's 1049 saves the cursor as DECSC does first. Then its ToAlternate: switch in, carrying the
        // cursor across (it is terminal-level, not per-buffer, so it does not move), then optionally erase
        // the alternate page. The margins stay as they are: the alternate screen shares the main display's.
        if (behavior->savesCursor)
        {
            pageAt(_cursorPage).saveCursor();
            // The page too, also when received while DECSASD selects the status line, for which DECSC on the
            // status line keeps its own buffer and saveCursorPage() records nothing.
            _savedCursorPage[savedCursorPageSlot()] = _cursorPage;
        }
        auto const carried = pageAt(_cursorPage).cursor();
        _pagesBeforeAlternateScreen = { .cursor = _cursorPage, .displayed = _displayedPage };
        setScreen(ScreenType::Alternate);
        // A switch of screen buffer is no page movement, which is all DECPCCM governs: the alternate screen
        // is shown whatever DECPCCM says, as xterm shows the buffer it switches to, and as Windows Terminal
        // keeps its alternate buffer outside paging.
        if (_displayedPage != _cursorPage)
            showPage(_cursorPage);
        if (behavior->carriesCursorIn)
            pageAt(_cursorPage).adoptCursor(carried);
        if (behavior->clearsOnEntry)
            clearScreen();
    }
    else if (!enable && isAlternateScreenBufferInUse())
    {
        // xterm's FromAlternate: optionally erase the alternate page first (ED 2 leaves the cursor put),
        // then switch back, carrying the cursor across (47, 1047; on 1049 the page's own cursor is the
        // restore of the cursor saved on the way in). Back to the page the cursor came from, as Windows
        // Terminal's ?1049 restores the page it saved with the cursor: DEC STD 070 defines no alternate
        // screen, and its DECSC, which saves the page with the active position, is the rule extended here.
        if (behavior->clearsOnExit)
            clearScreen();
        auto const carried = pageAt(_cursorPage).cursor();
        setPage(_pagesBeforeAlternateScreen.cursor, PageCursorPlacement::PagesOwn);
        // Coupled, the page on display followed the cursor. Reset, the page on display before the alternate
        // screen came up is shown again, as Windows Terminal's cursor restore moves its visible page only
        // while DECPCCM is set.
        if (!isModeEnabled(DECMode::PageCursorCoupling))
            showPage(_pagesBeforeAlternateScreen.displayed);
        if (behavior->carriesCursorOut)
            pageAt(_cursorPage).adoptCursor(carried);
    }

    // Modes 47, 1047 and 1049 are three views of one piece of state -- whether the alternate screen
    // buffer is in use -- so DECRQM must report all three consistently, no matter which one switched
    // (xterm keys every one off screen->whichBuf). Mirror the resulting buffer state onto all three
    // bits; the trailing _modes.set(mode, enable) in setMode() then agrees with it.
    auto const onAlternate = isAlternateScreenBufferInUse();
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
    // NB: the OSC 3008 context ancestry is deliberately left alone here; hardReset() below says why.

    // The status display is exited first (DEC STD 070 ch. 14 §2.3: "The Status Display will be exited, but
    // not erased"), so what follows resets the main display's state, not the status line's: ch. 4's
    // soft_terminal_reset() sets "active_display = MAIN_DISPLAY;" before the margins, the rendition and the
    // cursor save buffer, as xterm's ReallyReset leaves its status line before it resets anything. DEC's
    // algorithm lists origin mode and the character sets before that switch; they are the main display's
    // here, as in xterm's ReallyReset. Left as DECSASD 0 leaves it, a denial's owed title included.
    exitStatusLine();

    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setGraphicsRendition(GraphicsRendition::Reset); // SGR
    _currentScreen->resetSavedCursorState();        // DECSC (Save cursor state)
    // And the page in it, where DECRC reads the buffer on the page DECSC saved: DEC STD 070 DECSC, "Execution
    // of Soft Terminal Reset (DECSTR) also causes the Save Buffer to be set to the default values",
    // position.page = 1 among them.
    auto const slot = savedCursorPageSlot();
    _savedCursorPage[slot] = NoSavedCursorPages[slot];
    pageAt(_savedCursorPage[slot]).resetSavedCursorState();
    setMode(DECMode::VisibleCursor, true); // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false);       // DECOM
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

    // The macro definitions, the UDKs and the soft character set stay: DEC STD 070 lists "DRCS", "UDK" and
    // "Macro Definitions" among what is "NOT affected by the soft reset". hardReset() clears them.
    resetColorPalette();
    resetLocator();

    // The status display's type stays, shown and selected, with the host-writable line's text: DECSTR exited
    // it above, "but not erased" (DEC STD 070 ch. 14 §2.3), and its reset list names no status display type.
    // RIS is what erases it and returns to the configured type.

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

    // Clear the input generator's mirrored mode state *before* the mode register is replayed below.
    // DECCKM, DECNKM, DECBKM, DECMode::MousePassiveTracking and friends live in the generator rather
    // than in `_modes`, and setMode() is what pushes them across. Resetting the generator afterwards
    // would therefore undo every mode this function is about to (re-)establish -- and for a mode listed
    // in `frozenModes` that damage is permanent, because setMode() early-returns on a frozen mode and
    // nothing can ever resync the two halves again.
    core::locked(_inputMutex, [&] { _inputGenerator.reset(); });

    // KAM is reset with the rest of the register, not through setMode(), so its notice is dropped here; the
    // type RIS shows is set below.
    _statusNotices.disable(StatusNotice::KeyboardLocked);
    if (_statusNotices.none())
        _savedStatusDisplayType.reset();
    _modes.reset();

    // The register is cleared whole, not through setMode(), so the state a mode keeps outside it
    // goes here: mode 2034's reader session ends with the mode, and its token with it.
    _semanticBlockTracker.disable();

    // RIS removes every program-status record (program-status rev 0.4); DECSTR, softReset(), does not.
    announceProgramStatus(_programStatus.reset());

    // SRM, set: the terminal does *not* echo what it sends -- the host does. The mode register was just
    // cleared to all zeroes, and a *reset* SRM means local echo is on, so leaving it here would echo every
    // keystroke on top of the shell's own echo for the rest of the session. @see flushInput(), softReset().
    setMode(AnsiMode::SendReceive, true);

    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::AutoRepeat, true);
    setMode(DECMode::SixelCursorNextToGraphic, true);
    // A configured setting, so RIS returns it to the configuration as the VT510 manual's RIS replaces every
    // set-up feature with its saved setting -- DECSDM's VT340 counterpart is one -- and as xterm's full reset
    // restores its sixelScrolling resource. From the factory settings, which a reload keeps current.
    setMode(DECMode::NoSixelScrolling, _factorySettings.sixelScrolling == SixelScrolling::Disabled);
    setMode(DECMode::TextReflow, _settings.primaryScreen.allowReflowOnResize);
    setMode(DECMode::Unicode, _settings.graphemeClustering);
    setMode(DECMode::VisibleCursor, true);
    setMode(DECMode::PageCursorCoupling, true);
    setMode(DECMode::MouseAlternateScroll, true); // alternate scroll on, as at power-on

    for (auto const& [mode, frozen]: _settings.frozenModes)
        freezeMode(mode, frozen);

    _checksumExtension = _settings.checksumExtension;                       // XTCHECKSUM
    _userPreferredSupplementalSet = _settings.userPreferredSupplementalSet; // DECAUPSS
    _settings.warningBellVolume = _factorySettings.warningBellVolume;       // DECSWBV
    _settings.marginBellVolume = _factorySettings.marginBellVolume;         // DECSMBV

    // RIS restores the title modes to their default (xterm resets title_modes only on a full reset, not
    // on DECSTR). @see resetTitleModes, TitleModeFeature.
    resetTitleModes();

    // Before the pages: their reset re-seats each cursor, which stamps the row it lands on with the active
    // block -- and after RIS there is none.
    _commandBlocks.clear();
    primaryScreen().setActiveBlockId(_commandBlocks.currentId());

    // Reset all pages.
    for (auto& page: _pages)
        page->hardReset();
    _cursorPage = PageIndex(0);
    showPage(PageIndex(0));
    _pagesBeforeAlternateScreen = {};
    _savedCursorPage = NoSavedCursorPages; // DEC STD 070 DECSC: on reset the save buffer's page is 1
    _hostWritableStatusLineScreen.hardReset();
    _indicatorStatusScreen.hardReset();

    _imagePool.clear();
    _tabs.clear();

    // DEC STD 070: "All macro definitions are cleared on receipt of a Reset to Initial State (RIS) or Secure
    // Reset (DECSR)." A macro body that sent this RIS ends with it, and so does every body around it.
    clearMacros();

    // The VT520 manual's RIS "Clears user-defined keys" and "Clears the soft character set", as xterm's
    // ReallyReset clears its UDKs and Windows Terminal's HardReset its soft font -- on RIS alone.
    clearUDKs();
    clearDRCS();

    // A pointer shape is application state like any other, so RIS withdraws it: a program that dies
    // holding a hand cursor must not leave the user with one. The stack keeps its bottom entry -- the
    // terminal's own default -- exactly as popPointerShape does.
    resetPointerShape();
    // The OSC 3008 context ancestry is deliberately NOT reset here, and neither does softReset() touch
    // it. UAPI.15 makes that a safety property rather than a convenience: a program running down the
    // ancestry must not be able to erase the context a program above it established, and RIS is
    // something any program can send. A shell that has announced "you are inside container X" must
    // still be able to say so after a program inside that container resets the terminal.
    //
    // The full reset the specification DOES name is vhangup(), which has no equivalent here: closing
    // the PTY destroys the TerminalSession and this Terminal with it, so the ancestry dies with the
    // object and needs no mechanism. ContextStack::clear() exists for that teardown alone and is
    // reachable from no escape sequence.
    //
    // @see Screen_context_test.cpp, "osc3008 survives a hard reset".

    resetColorPalette();

    // A hard reset (RIS) clears the application-assigned window-frame/tab color (DECAC item 2). The
    // frame color is not part of the color palette, so it must be reset explicitly. (Soft reset
    // deliberately leaves it untouched, matching how window decorations survive DECSTR.) This only
    // withdraws what the application itself assigned: a color the user picked in the GUI is a separate,
    // higher-precedence source that no escape sequence can clear.
    resetWindowFrameColor();

    // Likewise the progress indicator (OSC 9;4): a program that dies mid-operation must not leave a
    // half-filled bar behind. Announced rather than assigned, so the frontends withdraw it too.
    setProgress(Progress {});

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

    // Reset the margins to their defaults.
    _mainDisplayMargin = makeDefaultMargin(mainDisplayPageSize);
    primaryScreen().verifyState();

    showStatusDisplayOfHost(_factorySettings.statusDisplayType);
    _selectedStatusDisplayType = StatusDisplayType::None;

    // NB: _inputGenerator.reset() deliberately runs near the top of this function, before the mode
    // register is replayed. @see the comment there.
    _pendingLocalEcho.clear();

    // That reset cleared DECCKM and DECNKM past their setters, which publish the binding modes.
    publishBindingModes();
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
    publishAnimationsEnd();
}

void Terminal::publishAnimationsEnd() noexcept
{
    auto const endOf = [](auto const& animation) {
        return animation.active ? animation.startTime + animation.duration
                                : chrono::steady_clock::time_point::min();
    };
    _animationsEnd.store(std::max({ endOf(_cursorMotion), endOf(_screenTransition), endOf(_lineFlash) }),
                         std::memory_order_relaxed);
}

void Terminal::movePage(int page, PageCursorPlacement placement)
{
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return;
    // The alternate screen is a display of one page, so page movement stays on it: NP and PP move home there,
    // and PPA, PPR and PPB leave the cursor where it is.
    if (isAlternateScreenBufferInUse())
    {
        if (placement == PageCursorPlacement::Home)
            pageAt(_cursorPage).moveCursorTo(LineOffset(0), ColumnOffset(0));
        return;
    }
    setPage(PageIndex(std::clamp(page, 0, AlternateScreenPageIndex.value - 1)), placement);
}

void Terminal::setPage(PageIndex target, PageCursorPlacement placement)
{
    // Clamp target to valid range.
    auto const clamped = PageIndex(std::clamp(target.value, 0, MaxPageCount - 1));
    if (clamped == _cursorPage)
    {
        // NP and PP move home on the last and the first page too, as DEC STD 070's algorithms do.
        if (placement == PageCursorPlacement::Home)
            pageAt(clamped).moveCursorTo(LineOffset(0), ColumnOffset(0));
        return;
    }

    // Cancel any active cursor motion animation on page change.
    _cursorMotion.active = false;
    publishAnimationsEnd();

    // Coupled, the page on display changes with the cursor's, so the outgoing page's glide and sub-line
    // offset end here, before its snapshot is taken. Reset, the page on display stays, and so does the user's
    // scroll.
    if (isModeEnabled(DECMode::PageCursorCoupling))
        resetSmoothScroll();

    // Capture snapshot of the outgoing screen for crossfade transition.
    if (isModeEnabled(DECMode::PageCursorCoupling)
        && _settings.screenTransitionStyle == ScreenTransitionStyle::Fade)
    {
        if (_screenTransition.active)
            finalizeScreenTransition();

        auto const savedChanges = _changes.load();
        auto const savedFrameID = _lastFrameID.load();

        // The build alone: no frame of the display's, so neither link detection nor _screenDirty sees it.
        RenderBuffer snapshotBuffer;
        fillRenderBufferInternal(snapshotBuffer, false);

        _changes.store(savedChanges);
        _lastFrameID.store(savedFrameID);

        _screenTransition.snapshotCells = std::move(snapshotBuffer.cells);
        _screenTransition.snapshotCursor = snapshotBuffer.cursor;
        _screenTransition.startTime = currentTime();
        _screenTransition.duration = _settings.screenTransitionDuration;
        _screenTransition.active = true;
        publishAnimationsEnd();
    }

    // DEC STD 070 keeps one active position and one rendition, so a page movement carries the cursor on.
    auto const leaving = pageAt(_cursorPage).cursor();
    _cursorPage = clamped;
    // While DECSASD selects the status line, it stays the screen the terminal acts on; the page takes over
    // when DECSASD returns to the main display (setActiveStatusDisplay).
    if (_activeStatusDisplay == ActiveStatusDisplay::Main)
        _currentScreen = _pages[clamped.value].get();

    // When DECPCCM is set, the displayed page follows the cursor page.
    if (isModeEnabled(DECMode::PageCursorCoupling))
        showPage(clamped);

    _currentScreenType = screenTypeFromPage(clamped);

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    switch (placement)
    {
        case PageCursorPlacement::PagesOwn: break;
        case PageCursorPlacement::SameLineAndColumn: pageAt(clamped).adoptCursor(leaving); break;
        case PageCursorPlacement::Home:
            pageAt(clamped).adoptCursor(leaving);
            pageAt(clamped).moveCursorTo(LineOffset(0), ColumnOffset(0));
            break;
    }
}

void Terminal::showPage(PageIndex page)
{
    auto const isAnotherPage = page != _displayedPage;
    if (isAnotherPage)
    {
        // What a page scrolled before it came on display is no output to follow.
        _displayedPageLinesFollowed = pageAt(page).linesScrolledIntoHistory();
        _displayedPage = page;
        _displayedScreenType = screenTypeFromPage(page);
        // The glide and the sub-line offset were the page shown before's.
        resetSmoothScroll();
    }
    _viewport.clampScrollOffset();
    if (!isAnotherPage)
        return;

    // What acts on what the user looks at follows it: the wheel, the key bindings, and the frontend -- its
    // scrollbar and its pointer shape.
    updateWheelMode();
    publishBindingModes();
    bufferChanged(_displayedScreenType);
}

InputGenerator::MouseWheelMode Terminal::wheelModeForDisplayedPage() const noexcept
{
    using Mode = InputGenerator::MouseWheelMode;
    // Tracking wins, as in Windows Terminal (mouseInput.cpp asks for tracking before alternate scroll);
    // page 1 has a history to scroll, as xterm's alternate scroll applies to its alternate buffer only; and
    // ?1007 reset turns alternate scroll off, as in xterm, Windows Terminal and foot.
    if (mouseProtocol().has_value() || displayedScreenType() == ScreenType::Primary
        || !isModeEnabled(DECMode::MouseAlternateScroll))
        return Mode::Default;
    // Cursor keys, in DECCKM's form at the moment, as xterm, Windows Terminal and foot send them.
    return applicationCursorKeys() ? Mode::ApplicationCursorKeys : Mode::NormalCursorKeys;
}

void Terminal::updateWheelMode()
{
    setMouseWheelMode(wheelModeForDisplayedPage());
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
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return;
    _savedCursorPage[savedCursorPageSlot()] = _cursorPage;
}

Screen& Terminal::restoreCursorPage(Screen& receiving)
{
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return receiving;
    auto const saved = _savedCursorPage[savedCursorPageSlot()];
    if (saved != _cursorPage)
        setPage(saved, PageCursorPlacement::PagesOwn);
    return pageAt(saved);
}

void Terminal::setScreen(ScreenType type)
{
    setPage(type == ScreenType::Primary ? PageIndex(0) : AlternateScreenPageIndex,
            PageCursorPlacement::PagesOwn);
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
    // clang-format on

    // Through the screens, as every page above: resizing a grid behind its screen's back left that screen's
    // cursor past a narrowed right margin and its cached cursor line un-re-pointed.
    auto const statusLinePageSize = PageSize { StatusLineHeight, _settings.pageSize.columns };
    _hostWritableStatusLineScreen.applyPageSizeToMainDisplay(statusLinePageSize);
    _indicatorStatusScreen.applyPageSizeToMainDisplay(indicatorStatusLinePageSize());

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

    core::ignoreUnused(position);
    // if (_selection->contains(position))
    //     clearSelection();
}

void Terminal::markRegionDirty(Rect area) noexcept
{
    if (_activeStatusDisplay != ActiveStatusDisplay::Main)
        return;

    if (!_selection)
        return;

    core::ignoreUnused(area);
    // if (_selection->intersects(area))
    //     clearSelection();
}

bool Terminal::isRenderingSuppressed() const noexcept
{
    // Keyed on the render-buffer flag rather than the mode register: both are set by the same
    // transition, but only this one is atomic, and this predicate is read from the render thread.
    if (_renderBufferUpdateEnabled.load(std::memory_order_acquire))
        return false;

    return (_clocks.steady.now() - _synchronizedOutputSince.load(std::memory_order_acquire))
           < _settings.synchronizedOutputTimeout;
}

void Terminal::synchronizedOutput(bool enabled)
{
    ZoneScoped;
    _renderBufferUpdateEnabled = !enabled;
    if (enabled)
    {
        _synchronizedOutputSince.store(_clocks.steady.now(), std::memory_order_release);
        return;
    }

    // The frame this ends with is built here, on the parser thread, at the time it ends: the clock is set to
    // that time, as the render thread's tick() sets it for the frames built there. Not by tick() itself,
    // which would advance the cursor blink and a glide from a second thread.
    auto const now = _clocks.steady.now();
    _currentTime.store(now, std::memory_order_relaxed);

    auto const diff = now - _renderBuffer.lastUpdate.load(std::memory_order_relaxed);
    if (diff < _refreshInterval.get().value)
    {
        // Instrumentation only: this is the throttle doing its job, and it is the expected outcome
        // for most block ends. Named so a capture can tell it apart from the branch below.
        ZoneScopedN("syncOut.dropped.throttled");
        return;
    }

    if (_renderBuffer.state == RenderBufferState::TrySwapBuffers)
    {
        // Instrumentation only. This branch drops a repaint that the throttle would have allowed, and
        // nothing re-arms it: while this state lasts, the batch's screenUpdated() only retries the swap of
        // the frame built before, and tells the display only when that retry succeeds, so while a paint
        // still holds the front buffer the block's end shows only with the next repaint something else asks
        // for.
        ZoneScopedN("syncOut.dropped.trySwap");
        return;
    }

    {
        ZoneScopedN("syncOut.refreshed");
        // The build alone, on the parser thread: link detection runs on the display's next frame, which the
        // batch's screenUpdated() tells of the block through _screenDirty.
        _renderBuffer.state = RenderBufferState::RefreshBuffersAndTrySwap;
        std::ignore =
            refreshRenderBufferWith([this](RenderBuffer& output) { fillRenderBufferInternal(output, true); });
        _eventListener.screenUpdated();
    }
}

void Terminal::onBufferScrolled(Screen const& scrolled, LineCount n) noexcept
{
    // The viewer's state is on the page on display, which the viewport is the window onto. A page the cursor
    // writes to with DECPCCM reset, the status line DECSASD selected and the indicator status line, which
    // every frame writes, each scroll under nobody's view: none of it moves.
    if (&scrolled != &displayedPage())
        return;

    // Adjust Normal-mode's cursor accordingly to make it fixed at the scroll-offset as if nothing has
    // happened.
    _viCommands.cursorPosition.line -= n;

    // Adjust viewport accordingly to make it fixed at the scroll-offset as if nothing has happened.
    // A viewport that actually moved has already run onViewportChanged() (and, in hint mode, the
    // refreshHints() there); remember that so the visible-scope branch below does not re-scan twice.
    auto viewportFollowedContent = false;
    if (viewport().scrolled() || _viewport.pixelOffset() > 0.0f || _inputHandler.mode() == ViMode::Normal)
        viewportFollowedContent = viewport().scrollUp(n);

    // Keep an open hint session consistent with content that just scrolled. Scrollback labels are
    // fixed for the session, so shift each match up to stay on its own text. The visible scope
    // re-scans its (now-changed) viewport instead — but only when the viewport did not already move,
    // since onViewportChanged() re-scans for us when it does.
    if (_hintModeHandler.isActive())
    {
        if (_hintScope == HintScope::Scrollback)
            _hintModeHandler.applyScroll(boxed_cast<LineOffset>(n), primaryScreen().historyLineCount());
        else if (!viewportFollowedContent)
            refreshHints();
    }

    if (!_selection)
        return;

    auto const top = -boxed_cast<LineOffset>(primaryScreen().historyLineCount());
    if (_selection->from().line > top && _selection->to().line > top)
        _selection->applyScroll(boxed_cast<LineOffset>(n), primaryScreen().historyLineCount());
    else
        clearSelection();
}

void Terminal::clampToHistory() noexcept
{
    _viewport.clampScrollOffset();

    // The Normal-mode cursor is walked one line further up on every scroll (@see onBufferScrolled)
    // with nothing bounding it, so an eviction leaves it pointing below the oldest row there is.
    // Bounded by addressableTop() rather than by the history depth, because that is the one place
    // allowed to answer "where does this grid start" -- the two differ after a reverse scroll has
    // wrapped unreset rows into the oldest slots.
    auto const top = primaryScreen().grid().addressableTop();
    if (_viCommands.cursorPosition.line < top)
        _viCommands.cursorPosition.line = top;
}
// }}}

void Terminal::setHistoryLimits(HistoryLimits historyLimits)
{
    _settings.historyLimits = historyLimits;
    primaryScreen().setHistoryLimits(historyLimits);
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

void Terminal::defineMacro(int id, MacroDeletion deletion, std::string body)
{
    // A definition of no macro there can be is ignored, deletion and all (DEC STD 070).
    if (id < 0 || id >= MaxMacroCount)
        return;

    switch (deletion)
    {
        case MacroDeletion::All: _macros.clear(); break;
        case MacroDeletion::SameId: break; // just below, the same as for the other
    }

    // The old definition goes first, as Pdt says, so its room counts toward the new one -- and stays gone
    // when the new one does not fit after all: "Any macro definitions which cannot fit in the remaining
    // macro storage space are ignored" (DEC STD 070).
    _macros.erase(id);
    if (body.empty() || body.size() > macroSpaceAvailable())
        return;
    _macros.emplace(id, std::move(body));
}

size_t Terminal::macroSpaceAvailable() const noexcept
{
    auto const sizes = _macros | std::views::values
                       | std::views::transform([](std::string const& body) { return body.size(); });
    auto const used = std::ranges::fold_left(sizes, size_t { 0 }, std::plus {});
    return MacroSpaceBytes - std::min(used, MacroSpaceBytes);
}

size_t Terminal::macroSpaceFor(int id, MacroDeletion deletion) const noexcept
{
    switch (deletion)
    {
        case MacroDeletion::All: return MacroSpaceBytes;
        case MacroDeletion::SameId:
            return macroSpaceAvailable() + macroBody(id).value_or(std::string_view {}).size();
    }
    core::unreachable();
}

uint16_t Terminal::macroChecksum() const noexcept
{
    // DEC STD 070 asks for "a checksum of the current text macro definitions" without spelling the sum out.
    // This is Windows Terminal's MacroBuffer::CalculateChecksum, "intended to match the checksums produced
    // by the original DEC VT420" -- over the definitions alone, not the remnants a VT420's memory would hold.
    // The order of the definitions makes no difference to a sum.
    auto sum = uint16_t { 0 };
    for (auto const& body: _macros | std::views::values)
        for (auto const byte: body)
            sum = static_cast<uint16_t>(sum - static_cast<unsigned char>(byte));
    return sum;
}

void Terminal::invokeMacro(int id)
{
    auto const it = _macros.find(id);
    if (it == _macros.end())
        return;

    // DEC STD 070: "Macros can invoke other macros with up to 16 levels of nesting, but cannot invoke
    // themselves recursively" -- through another macro no more than directly.
    if (std::ssize(_runningMacros) >= MaxMacroNestingDepth || std::ranges::contains(_runningMacros, id))
        return;

    // The bound on the fan-out nesting leaves open, Windows Terminal's: "we don't allow the total sequence
    // length to exceed the maximum buffer size" -- the bodies one invocation from the stream runs, counted
    // afresh for each such invocation.
    if (!isMacroRunning())
        _macroBytesRun = 0;
    auto const& body = it->second;
    if (body.size() >= MaxMacroBytesPerInvocation - _macroBytesRun)
        return;
    _macroBytesRun += body.size();

    auto invocation = MacroInvocation { .id = id, .body = body };

    // Not during a parse: in the trace modes a queued DECINVM is executed as it is stepped through, after the
    // parse has moved on, so its body is parsed then, from where the parse stands.
    if (tlParseDepth == 0)
    {
        runMacro(invocation);
        return;
    }

    // In place, as DEC STD 070 has it: "The terminal substitutes the DECINVM sequence with the contents of
    // the macro definition, and executes the substituted text." The main parser is still handling the
    // DECINVM, so the body runs once the parser has returned after it -- done with the sequence, and back in
    // Ground -- as Windows Terminal's InvokeMacro waits for the CSI to complete.
    _pendingMacroInvocation = std::move(invocation);
    _parser.returnAfterCurrentByte();
}

void Terminal::runMacro(MacroInvocation const& invocation)
{
    // Through the main parser, which goes on from the state the body leaves it in exactly as it would after
    // the same bytes from the host -- in VT52, with the character REP repeats, in the middle of a sequence
    // -- as Windows Terminal's InvokeMacro hands the body to the state machine reading the stream. The
    // terminal's state carries over as well: "The current state of the machine is not saved or restored when
    // macros are invoked" (DEC STD 070).
    _runningMacros.push_back(invocation.id);
    auto const _ = core::Finally { [&]() noexcept {
        _runningMacros.pop_back();
        if (!isMacroRunning())
            _areRunningMacrosVoid = false; // what a hard reset voided has all ended now
    } };
    parseExpandingMacros(invocation.body);
}

void Terminal::clearMacros() noexcept
{
    _macros.clear();
    if (!isMacroRunning())
        return;

    // A hard reset in a body: Windows Terminal's ClearMacrosIfInUse replaces the definitions -- the very text
    // its running bodies are read from -- "with NUL characters to prevent any further output". So every body
    // running ends here, and the parser hands back control right after the reset to let them.
    _areRunningMacrosVoid = true;
    _parser.returnAfterCurrentByte();
}

std::shared_ptr<RasterizedImage> Terminal::createDRCSImage(DRCSGlyph const& glyph, RGBColor foregroundColor)
{
    // Convert monochrome bitmap to RGBA
    auto const pixelCount = static_cast<size_t>(glyph.width) * static_cast<size_t>(glyph.height);
    auto rgbaData = Image::Data(pixelCount * 4, 0);

    for (size_t i = 0; i < pixelCount && i < glyph.bitmap.size(); ++i)
    {
        if (glyph.bitmap[i])
        {
            rgbaData[(i * 4) + 0] = foregroundColor.red;
            rgbaData[(i * 4) + 1] = foregroundColor.green;
            rgbaData[(i * 4) + 2] = foregroundColor.blue;
            rgbaData[(i * 4) + 3] = 0xFF;
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
            glyph.bitmap.resize(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

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
                        auto const idx = (static_cast<size_t>(row + bit) * static_cast<size_t>(width))
                                         + static_cast<size_t>(col);
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
    // DEC STD 070 ch. 13 DECELR: "Undefined values for Ps or Pu causes the entire sequence to be ignored."
    // Ps: 0 disabled, 1 enabled, 2 one shot. Pu: 0 (or omitted) and 2 character cells, 1 device pixels.
    auto const defined = [](int value) {
        return 0 <= value && value <= 2;
    };
    if (!defined(ps) || !defined(pu))
        return;

    static constexpr auto ReportingOf =
        std::array { LocatorReporting::Off, LocatorReporting::Continuous, LocatorReporting::OneShot };
    _locatorState.reporting = ReportingOf[static_cast<size_t>(ps)];
    _locatorState.coordUnit = pu == 1 ? LocatorCoordUnit::DevicePixels : LocatorCoordUnit::CharacterCells;
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

void Terminal::sendLocatorReport(int event)
{
    // Off the grid the locator is unavailable, as xterm answers a position outside its window (LocatorCoords'
    // out-of-range flag). In device pixels that also takes a pixel outside the grid -- over the window's
    // padding, where the cell is still the one at the edge -- and every pixel while the cell's size is not
    // known.
    auto const offTheGrid = [&] {
        if (_pointerPlacement == PointerPlacement::OffGrid)
            return true;
        if (_locatorState.coordUnit != LocatorCoordUnit::DevicePixels)
            return false;
        auto const extent = cellPixelSize() * pageSize();
        auto const [x, y] = _currentMousePixelPosition;
        return x.value < 0 || y.value < 0 || x.value >= unbox<int>(extent.width)
               || y.value >= unbox<int>(extent.height);
    }();
    if (event == 0 || offTheGrid)
        reply("\033[0&w");
    else
    {
        // Pr and Pc, from 1, in the unit DECELR selected: device pixels as xterm's LocatorCoords gives them,
        // or cells.
        auto const [row, column] = [&]() -> std::pair<int, int> {
            switch (_locatorState.coordUnit)
            {
                case LocatorCoordUnit::CharacterCells:
                    return { *_currentMousePosition.line + 1, *_currentMousePosition.column + 1 };
                case LocatorCoordUnit::DevicePixels:
                    return { _currentMousePixelPosition.y.value + 1, _currentMousePixelPosition.x.value + 1 };
            }
            core::unreachable();
        }();
        // Pp: the page seen through the window, always sent.
        reply("\033[{};{};{};{};{}&w", event, _locatorButtonsHeld, row, column, displayedPageNumber());
    }
    flushInput();

    // DEC STD 070 ch. 13 DECLRP: in one shot mode "a single locator report (DECLRP) shall reset locator
    // reporting mode to disabled" -- a request's answer as a button's, and as xterm the unavailable one too.
    if (_locatorState.reporting == LocatorReporting::OneShot)
        _locatorState.reporting = LocatorReporting::Off;
}

void Terminal::requestLocatorPosition()
{
    sendLocatorReport(_locatorState.reporting == LocatorReporting::Off ? 0 : 1);
}

namespace
{
    /// A button of the DEC locator, as DECLRP encodes it (DEC STD 070 ch. 13).
    struct LocatorButtonCode
    {
        MouseButton button; ///< The button.
        int down;           ///< Pe when it goes down.
        int up;             ///< Pe when it goes up.
        uint8_t held;       ///< Its bit in Pb, which names the buttons held down.
    };

    /// The buttons the locator reports, a row each, as DEC STD 070 and xterm encode them.
    constexpr auto LocatorButtonCodes = std::array {
        LocatorButtonCode { .button = MouseButton::Left, .down = 2, .up = 3, .held = 0b100 },
        LocatorButtonCode { .button = MouseButton::Middle, .down = 4, .up = 5, .held = 0b010 },
        LocatorButtonCode { .button = MouseButton::Right, .down = 6, .up = 7, .held = 0b001 },
    };
} // namespace

Handled Terminal::handleLocatorMouseEvent(LocatorEvent event, MouseButton button)
{
    auto const code = std::ranges::find(LocatorButtonCodes, button, &LocatorButtonCode::button);
    if (code == LocatorButtonCodes.end())
        return Handled { false };

    switch (event)
    {
        case LocatorEvent::ButtonDown: _locatorButtonsHeld |= code->held; break;
        case LocatorEvent::ButtonUp: _locatorButtonsHeld &= static_cast<uint8_t>(~code->held); break;
    }

    auto const selected = [&] {
        switch (event)
        {
            case LocatorEvent::ButtonDown: return _locatorState.reportButtonDown;
            case LocatorEvent::ButtonUp: return _locatorState.reportButtonUp;
        }
        core::unreachable();
    }();
    if (_locatorState.reporting == LocatorReporting::Off || !selected)
        return Handled { false };

    sendLocatorReport(event == LocatorEvent::ButtonDown ? code->down : code->up);
    return Handled { true };
}

void Terminal::programUDK(bool clearAll, bool locked, std::string_view data)
{
    // Under the input lock, which a function key looks its string up under. @see udkString()
    auto const _ = std::scoped_lock { _inputMutex };

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

std::optional<std::string> Terminal::udkStringForKey(Key key) const
{
    return userDefinedKeyNumberOf(key).and_then([this](int keyNumber) { return udkString(keyNumber); });
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

void Terminal::recordStatusDisplaySelection(StatusDisplayType type)
{
    _selectedStatusDisplayType = type;
    if (type != StatusDisplayType::HostWritable && _activeStatusDisplay == ActiveStatusDisplay::StatusLine)
        exitStatusLine();
}

void Terminal::selectStatusDisplayByHost(StatusDisplayType type)
{
    recordStatusDisplaySelection(type);
    switch (type)
    {
        case StatusDisplayType::None:
        case StatusDisplayType::Indicator: showStatusDisplayOfHost(type); break;
        case StatusDisplayType::HostWritable:
            // Shown already, or to be shown once a notice is down: nothing to ask the frontend.
            if (_statusDisplayType != StatusDisplayType::HostWritable
                && _savedStatusDisplayType != StatusDisplayType::HostWritable)
                requestShowHostWritableStatusLine();
            break;
    }
}

void Terminal::showStatusDisplayOfHost(StatusDisplayType type)
{
    if (_statusNotices.any())
    {
        _savedStatusDisplayType = type;
        return;
    }
    setStatusDisplay(type);
}

void Terminal::selectStatusDisplayByUser(StatusDisplayType type)
{
    recordStatusDisplaySelection(type);
    setStatusDisplay(type);
}

void Terminal::selectConfiguredStatusDisplay(StatusDisplayType type)
{
    if (_statusNotices.none())
    {
        selectStatusDisplayByUser(type);
        return;
    }
    // A type shown for a while stays up; leaving it shows the type the configuration now names.
    recordStatusDisplaySelection(type);
    _savedStatusDisplayType = type;
}

bool Terminal::isHostWritableStatusLineSelected() const noexcept
{
    return _selectedStatusDisplayType == StatusDisplayType::HostWritable
           || _statusDisplayType == StatusDisplayType::HostWritable;
}

void Terminal::answerHostWritableStatusLineRequest(HostWritableStatusLineAnswer answer)
{
    switch (answer)
    {
        case HostWritableStatusLineAnswer::Denied:
            // Owed whatever the configuration says now: it is read at DECSASD 0, where it acts.
            _deniedStatusLineTitle = DeniedStatusLineTitle::Owed;
            break;
        case HostWritableStatusLineAnswer::Shown:
            showStatusDisplayOfHost(StatusDisplayType::HostWritable);
            _deniedStatusLineTitle = DeniedStatusLineTitle::Settled;
            break;
    }
}

void Terminal::syncWindowTitleWithDeniedStatusLine()
{
    if (_activeStatusDisplay != ActiveStatusDisplay::StatusLine
        || _deniedStatusLineTitle != DeniedStatusLineTitle::Owed
        || !_settings.syncWindowTitleWithHostWritableStatusDisplay)
        return;

    setWindowTitle(core::trimRight(_hostWritableStatusLineScreen.grid().lineText(LineOffset(0))));
    _deniedStatusLineTitle = DeniedStatusLineTitle::Settled;
}

void Terminal::exitStatusLine()
{
    syncWindowTitleWithDeniedStatusLine();
    setActiveStatusDisplay(ActiveStatusDisplay::Main);
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

void Terminal::pushStatusDisplay(StatusNotice owner, StatusDisplayType type)
{
    // The type shown before the first notice is what the last one down shows again.
    if (_statusNotices.none())
        _savedStatusDisplayType = _statusDisplayType.get();
    _statusNotices.enable(owner);

    setStatusDisplay(type);
}

void Terminal::popStatusDisplay(StatusNotice owner)
{
    // Only the owner's own: a KAM reset never takes down a notice KAM did not put up.
    if (!_statusNotices.test(owner))
        return;
    _statusNotices.disable(owner);
    if (_statusNotices.any() || !_savedStatusDisplayType)
        return;

    setStatusDisplay(_savedStatusDisplayType.value());
    _savedStatusDisplayType.reset();
}

void Terminal::setUserInputLock(UserInputLock lock)
{
    if (_userInputLock.exchange(lock, std::memory_order_relaxed) == lock)
        return;
    switch (lock)
    {
        case UserInputLock::Locked:
            pushStatusDisplay(StatusNotice::UserInputLocked, StatusDisplayType::Indicator);
            break;
        case UserInputLock::Unlocked: popStatusDisplay(StatusNotice::UserInputLocked); break;
    }
}

bool Terminal::setNewSearchTerm(std::u32string text, SearchOrigin origin)
{
    _search.origin = origin;

    if (_search.pattern == text)
        return false;

    _search.pattern = std::move(text);
    _search.comparison = caseComparisonFor(_search.pattern, _search.caseSensitivity);
    publishBindingModes();
    return true;
}

bool Terminal::setSearchCaseSensitivity(SearchCaseSensitivity mode)
{
    if (_search.caseSensitivity == mode)
        return false;

    _search.caseSensitivity = mode;
    _search.comparison = caseComparisonFor(_search.pattern, mode);
    return true;
}

SearchMatchTally Terminal::tallySearchMatches(CellLocation position, size_t limit)
{
    return displayedPage().tallyMatches(
        u32string_view(_search.pattern), position, _search.caseSensitivity, limit);
}

optional<CellLocation> Terminal::searchReverse(u32string text, CellLocation searchPosition)
{
    if (!setNewSearchTerm(std::move(text), SearchOrigin::Typed))
        return searchPosition;

    return searchReverse(searchPosition);
}

optional<CellLocation> Terminal::search(CellLocation searchPosition)
{
    auto const searchText = u32string_view(_search.pattern);
    // Through the cached comparison rather than the policy: Search::comparison is kept in step with
    // the pattern by this class's own setters, and re-deriving it here would put a UCD lookup per
    // codepoint of the needle on every keystroke of an incremental search. @see Search::comparison.
    auto const matchLocation = displayedPage().searchFrom(searchText, searchPosition, _search.comparison);

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
    else if (cursorPosition.line > -boxed_cast<LineOffset>(displayedPage().historyLineCount()))
    {
        startPosition.line--;
        startPosition.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    }

    return searchReverse(startPosition);
}

void Terminal::clearSearch()
{
    _search.pattern.clear();
    publishBindingModes();
    _search.origin = SearchOrigin::Typed;
    // Kept in step with the (now empty) pattern by the same rule the setters use, rather than pinned
    // to a constant: an empty needle under a pinned Sensitive policy still compares exactly.
    _search.comparison = caseComparisonFor(_search.pattern, _search.caseSensitivity);

    // The highlights of the pattern just dropped are still in the render buffer, and nothing on this
    // path rebuilds it -- unlike search()/searchReverse(). Raised as its own event rather than as
    // screenUpdated(), whose auto-scroll would additionally yank the viewport to the bottom and lose
    // whatever the user had scrolled back to read.
    _eventListener.searchCleared();
}

bool Terminal::wordDelimited(CellLocation position) const noexcept
{
    return wordDelimited(position, u32string_view(_settings.wordDelimiters));
}

bool Terminal::wordDelimited(CellLocation position, std::u32string_view wordDelimiters) const noexcept
{
    // Word selection may be off by one
    position.column = std::min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    return displayedPage().grid().cellEmptyOrContainsOneOf(position, wordDelimiters);
}

std::tuple<std::u32string, CellLocationRange> Terminal::extractWordUnderCursor(
    CellLocation position) const noexcept
{
    auto const& screen = displayedPage();
    auto const range = screen.grid().wordRangeUnderCursor(position, u32string_view(_settings.wordDelimiters));
    return { screen.grid().extractText(range), range };
}

optional<CellLocation> Terminal::searchReverse(CellLocation searchPosition)
{
    auto const searchText = u32string_view(_search.pattern);
    // The cached comparison, for the reason Terminal::search() states.
    auto const matchLocation =
        displayedPage().searchReverseFrom(searchText, searchPosition, _search.comparison);

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

    auto const anchorBlock = displayedPage().multicellBlockAt(anchor);
    if (!anchorBlock || anchorBlock->rows < 2)
        return pointer;

    auto const pointerBlock = displayedPage().multicellBlockAt(pointer);
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
                       return core::ascending(highlightRange.from, cell, highlightRange.to)
                              || core::ascending(highlightRange.to, cell, highlightRange.from);
                   }
                   else
                   {
                       return core::ascending(highlightRange.from.line, cell.line, highlightRange.to.line)
                              && core::ascending(
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

void Terminal::setColorPalette(ColorPalette const& palette)
{
    auto const previousBackground = _colorPalette.defaultBackground;
    _colorPalette = palette;
    if (_colorPalette.defaultBackground != previousBackground)
        notifyDefaultBackgroundColorChanged();
}

void Terminal::resetColorPalette(ColorPalette const& colors)
{
    // Measured against the palette in effect, colours a program set included: that is what an application
    // subscribed to the report has seen. It answers each report by querying its colours again and redrawing,
    // so a re-seed that changes nothing -- every reload re-seeds -- must not send one.
    auto const changed = _colorPalette != colors;
    auto const backgroundChanged = _colorPalette.defaultBackground != colors.defaultBackground;

    _colorPalette = colors;
    _defaultColorPalette = colors;
    _settings.colorPalette = colors;
    _factorySettings.colorPalette = colors;

    if (changed && isModeEnabled(DECMode::ReportColorPaletteUpdated))
        _currentScreen->reportColorPaletteUpdate();
    if (backgroundChanged)
        notifyDefaultBackgroundColorChanged();
}

void Terminal::reconfigureFactorySettings(Settings configured)
{
    // Both follow the live terminal rather than the configuration: the window decides the page size, and
    // the color palette arrives through resetColorPalette(), which keeps it in step itself.
    configured.pageSize = _factorySettings.pageSize;
    configured.colorPalette = _factorySettings.colorPalette;
    _factorySettings = std::move(configured);
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
    // Looking at the xterm's source code, and simply mimicking their semantics without questioning,
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

SequenceHandler* TraceHandler::macroBodyDisplay() const noexcept
{
    return _terminal->isMacroRunning() ? &_terminal->activeDisplay() : nullptr;
}

void TraceHandler::executeControlCode(char controlCode)
{
    if (auto* const display = macroBodyDisplay())
    {
        display->executeControlCode(controlCode);
        return;
    }
    auto seq = Sequence {};
    seq.setCategory(FunctionCategory::C0);
    seq.setFinalChar(controlCode);
    _pendingSequences.emplace_back(std::move(seq));
}

void TraceHandler::processSequence(Sequence const& sequence)
{
    if (auto* const display = macroBodyDisplay())
        display->processSequence(sequence);
    else
        _pendingSequences.emplace_back(sequence);
}

void TraceHandler::processAPC(std::string_view body)
{
    // Queued like everything else, and for the same reason: this handler's whole job is to preserve
    // the ORDER in which the application sent things while letting the user step through them.
    // Forwarding an APC straight to the display let it overtake every sequence still queued ahead of
    // it -- a kitty image placed against the cursor position a queued CUP had not moved yet, so
    // tracing a graphics problem showed behaviour that never happens outside trace mode.
    if (auto* const display = macroBodyDisplay())
        display->processAPC(body);
    else
        _pendingSequences.emplace_back(ApplicationProgramCommand { .body = std::string(body) });
}

void TraceHandler::writeText(char32_t codepoint)
{
    if (auto* const display = macroBodyDisplay())
        display->writeText(codepoint);
    else
        _pendingSequences.emplace_back(codepoint);
}

void TraceHandler::writeText(std::string_view codepoints, size_t cellCount)
{
    if (auto* const display = macroBodyDisplay())
        display->writeText(codepoints, cellCount);
    else
        _pendingSequences.emplace_back(CodepointSequence { .text = codepoints, .cellCount = cellCount });
}

void TraceHandler::writeTextEnd()
{
    if (auto* const display = macroBodyDisplay())
        display->writeTextEnd();
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
        if (auto const* functionDefinition = seq->functionDefinition(_terminal->supportedSequences()))
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
std::string toString(AnsiMode mode)
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

std::string toString(DECMode mode)
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
