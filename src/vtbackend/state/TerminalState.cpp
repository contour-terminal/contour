// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalState.hpp>

#include <vtbackend/graphics/SixelParser.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/state/StateFields.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <span>

namespace vtbackend
{

namespace
{
    using detail::StateFailure;

    [[nodiscard]] PaletteSlice sliceOf(ColorPalette const& palette)
    {
        return PaletteSlice { .palette = palette.palette,
                              .defaultForeground = palette.defaultForeground,
                              .defaultBackground = palette.defaultBackground,
                              .cursor = palette.cursor,
                              .mouseForeground = palette.mouseForeground,
                              .mouseBackground = palette.mouseBackground,
                              .alternateTextColors = palette.alternateTextColors };
    }

    void overlay(ColorPalette& palette, PaletteSlice const& slice)
    {
        palette.palette = slice.palette;
        palette.defaultForeground = slice.defaultForeground;
        palette.defaultBackground = slice.defaultBackground;
        palette.cursor = slice.cursor;
        palette.mouseForeground = slice.mouseForeground;
        palette.mouseBackground = slice.mouseBackground;
        palette.alternateTextColors = slice.alternateTextColors;
    }

    [[nodiscard]] bool isPage(PageIndex page) noexcept
    {
        return page.value >= 0 && page.value < MaxPageCount;
    }

    /// @return The construction settings of @p settings a restore depends on, the primary screen's scrollback
    ///         limits being @p historyLimits.
    [[nodiscard]] ConstructionState constructionOf(Settings const& settings, HistoryLimits historyLimits)
    {
        return ConstructionState {
            .pageSize = settings.pageSize,
            .historyLimits = historyLimits,
            .primaryReflow =
                settings.primaryScreen.allowReflowOnResize ? ReflowOnResize::Yes : ReflowOnResize::No,
            .maxImageRegisterCount = settings.maxImageRegisterCount,
            .hyperlinkCacheCapacity = settings.hyperlinkCacheCapacity,
            .maxProgramStatusRecords = settings.maxProgramStatusRecords,
            .maxCommandBlocks = settings.commandBlockLimits.maxRecords,
            .maxContextDepth = settings.contextLimits.maxDepth,
            .maxRetainedContexts = settings.contextLimits.maxRetained,
        };
    }

    /// @return Whether @p margin lies inside a page of @p size. A margin may be empty (its end one before its
    ///         start): a status line not shown has one.
    [[nodiscard]] bool isInside(Margin const& margin, PageSize size) noexcept
    {
        auto const fits = [](auto from, auto to, auto count) {
            using Offset = decltype(from);
            return from >= Offset(0) && to >= from - Offset(1) && to < count;
        };
        return fits(margin.vertical.from, margin.vertical.to, boxed_cast<LineOffset>(size.lines))
               && fits(margin.horizontal.from, margin.horizontal.to, boxed_cast<ColumnOffset>(size.columns));
    }

    /// @return Whether @p location is a cell of @p grid, history included; a selection may end one column
    /// past
    ///         the last.
    [[nodiscard]] bool isSelectable(CellLocation location, GridState const& grid) noexcept
    {
        auto const history = unbox<int64_t>(grid.linesUsed) - unbox<int64_t>(grid.pageSize.lines);
        return unbox<int64_t>(location.line) >= -history
               && location.line < boxed_cast<LineOffset>(grid.pageSize.lines)
               && location.column >= ColumnOffset(0)
               && location.column <= boxed_cast<ColumnOffset>(grid.pageSize.columns);
    }

    /// Rejects every enumerator and flag of a state this build has no wire name for: whatever indexes by one
    /// (a palette's tints, a status display switch) then never meets one out of range.
    class EnumChecker
    {
      public:
        template <typename T>
        void operator()(uint32_t /*id*/, T const& value)
        {
            check(value);
        }

        template <typename T>
        void operator()(uint32_t /*id*/, T const& value, std::string_view /*name*/)
        {
            check(value);
        }

        void fail(StateDecodeError /*error*/) noexcept { _isListed = false; }

        /// @return Whether every enumerator met is listed.
        [[nodiscard]] bool isListed() const noexcept { return _isListed; }

        template <typename T>
        void check(T const& value)
        {
            using namespace detail;
            if (!_isListed)
                return;
            if constexpr (std::is_enum_v<T>)
                _isListed = wireIndexOf(value) != UnlistedEnumerator;
            else if constexpr (FlagSetType<T>)
                _isListed = wireBitsOf(value) != UnlistedEnumerator;
            else if constexpr (BoxedValue<T> || isVarintValue<T>()
                               || std::is_same_v<T, std::string> || std::is_same_v<T, std::u32string>
                               || std::is_same_v<T, std::vector<uint8_t>> || std::is_empty_v<T>)
            {
            }
            else if constexpr (IsStdVector<T>::value || IsStdArray<T>::value)
            {
                for (auto const& element: value)
                    check(element);
            }
            else if constexpr (MapLike<T>)
            {
                for (auto const& [key, mapped]: value)
                {
                    check(key);
                    check(mapped);
                }
            }
            else if constexpr (IsStdVariant<T>::value)
                std::visit([&](auto const& alternative) { check(alternative); }, value);
            else if constexpr (IsStdOptional<T>::value)
            {
                if (value)
                    check(*value);
            }
            else if constexpr (IsStdPair<T>::value)
            {
                check(value.first);
                check(value.second);
            }
            else if constexpr (ListsOwnFields<EnumChecker, T const>)
                value.visitFields(*this);
            else
                visitStateFields(*this, value);
        }

      private:
        bool _isListed = true;
    };
} // namespace

// {{{ InputGenerator

std::expected<void, StateDecodeError> ExtendedKeyboardInputGenerator::validateExtendedState(
    ExtendedKeyboardState const& state)
{
    // The flags the protocol defines: 1, 2, 4, 8 and 16.
    constexpr auto DefinedFlagBits = uint8_t { 0b1'1111 };
    if (state.currentStackTop >= MaxStackDepth
        || std::ranges::any_of(state.flags, [](uint8_t bits) { return (bits & ~DefinedFlagBits) != 0; }))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> ExtendedKeyboardInputGenerator::restoreExtendedState(
    ExtendedKeyboardState const& state)
{
    if (auto const valid = validateExtendedState(state); !valid)
        return valid;
    StandardKeyboardInputGenerator::restoreState(state.standard);
    for (auto const index: std::views::iota(size_t { 0 }, _flags.size()))
        _flags[index] = KeyboardEventFlags::fromValue(state.flags[index]);
    _currentStackTop = static_cast<size_t>(state.currentStackTop);
    return {};
}

InputGenerator::State InputGenerator::captureState() const
{
    return State { .bracketedPaste = _bracketedPaste,
                   .generateFocusEvents = _generateFocusEvents,
                   .win32InputMode = _win32InputMode,
                   .mouseProtocol = _mouseProtocol,
                   .passiveMouseTracking = _passiveMouseTracking,
                   .mouseTransport = _mouseTransport,
                   .mouseWheelMode = _mouseWheelMode,
                   .mouseWheelScrollMultiplier = _mouseWheelScrollMultiplier,
                   .modifyOtherKeys = _modifyOtherKeys,
                   .pendingSequence = _pendingSequence,
                   .consumedBytes = _consumedBytes,
                   .pressedMouseButtons = std::vector<MouseButton>(_currentlyPressedMouseButtons.begin(),
                                                                   _currentlyPressedMouseButtons.end()),
                   .currentMousePosition = _currentMousePosition,
                   .keyboard = _keyboardInputGenerator.captureExtendedState() };
}

std::expected<void, StateDecodeError> InputGenerator::validateState(State const& state)
{
    // peek() hands out the pending input from consumedBytes on: it must lie inside it.
    if (state.consumedBytes < 0 || static_cast<size_t>(state.consumedBytes) > state.pendingSequence.size())
        return StateFailure { StateDecodeError::InvalidValue };
    return ExtendedKeyboardInputGenerator::validateExtendedState(state.keyboard);
}

std::expected<void, StateDecodeError> InputGenerator::restoreState(State const& state)
{
    if (auto const valid = validateState(state); !valid)
        return valid;
    if (auto const done = _keyboardInputGenerator.restoreExtendedState(state.keyboard); !done)
        return done;
    _bracketedPaste = state.bracketedPaste;
    _generateFocusEvents = state.generateFocusEvents;
    _win32InputMode = state.win32InputMode;
    _mouseProtocol = state.mouseProtocol;
    _passiveMouseTracking = state.passiveMouseTracking;
    _mouseTransport = state.mouseTransport;
    _mouseWheelMode = state.mouseWheelMode;
    _mouseWheelScrollMultiplier = state.mouseWheelScrollMultiplier;
    _modifyOtherKeys = state.modifyOtherKeys;
    _pendingSequence = state.pendingSequence;
    _consumedBytes = state.consumedBytes;
    _currentlyPressedMouseButtons =
        std::set<MouseButton>(state.pressedMouseButtons.begin(), state.pressedMouseButtons.end());
    _currentMousePosition = state.currentMousePosition;
    return {};
}

// }}}

// {{{ HyperlinkStorage

HyperlinkStorageState HyperlinkStorage::captureState() const
{
    auto state = HyperlinkStorageState { .entries = {}, .nextHyperlinkId = nextHyperlinkId };
    for (auto const& item: cache)
        state.entries.push_back({ .id = item.key, .userId = item.value->userId, .uri = item.value->uri });
    return state;
}

std::expected<void, StateDecodeError> HyperlinkStorage::validateState(
    HyperlinkStorageState const& state) const
{
    auto ids = std::set<uint16_t> {};
    if (state.entries.size() > cache.capacity()
        || std::ranges::any_of(state.entries, [&](HyperlinkEntryState const& entry) {
               return !entry.id || !ids.insert(entry.id.value).second;
           }))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> HyperlinkStorage::restoreState(HyperlinkStorageState const& state)
{
    if (auto const valid = validateState(state); !valid)
        return valid;
    cache.clear();
    // Most recently used first in the state: relink from the other end, each emplace going to the front.
    for (auto const& entry: state.entries | std::views::reverse)
        cache.emplace(
            entry.id,
            std::make_shared<HyperlinkInfo>(HyperlinkInfo { .userId = entry.userId, .uri = entry.uri }));
    nextHyperlinkId = state.nextHyperlinkId;
    return {};
}

// }}}

// {{{ Terminal

TerminalState Terminal::captureState() const
{
    // Not captured, deliberately: what a client owns and sends again when it reattaches -- whether the
    // terminal has focus, the tabs its status line shows, the window state and the cell's pixel size. The
    // daemon sets none of them itself.
    auto builder = ImageTableBuilder { _imagePool };
    auto state = TerminalState {};
    state.construction = constructionOf(_settings, _pages[0]->grid().historyLimits());
    state.modes = _modes.captureState();

    state.paging = PagingState {
        .cursorPage = _cursorPage,
        .displayedPage = _displayedPage,
        .cursorPageBeforeAlternate = _pagesBeforeAlternateScreen.cursor,
        .displayedPageBeforeAlternate = _pagesBeforeAlternateScreen.displayed,
        .savedCursorPage = _savedCursorPage,
        .activeStatusDisplay = _activeStatusDisplay,
        .statusDisplayType = _statusDisplayType.get(),
        .selectedStatusDisplayType = _selectedStatusDisplayType,
        .savedStatusDisplayType = _savedStatusDisplayType,
        .statusNotices = _statusNotices,
        .isDeniedStatusLineTitleOwed = _deniedStatusLineTitle == DeniedStatusLineTitle::Owed,
        .scrollOffset = _viewport.scrollOffset(),
        .displayedPageLinesFollowed = _displayedPageLinesFollowed,
    };

    for (auto const& page: _pages)
        state.pages.push_back(page->captureState(builder));
    state.hostWritableStatusLine = _hostWritableStatusLineScreen.captureState(builder);
    state.indicatorStatusLine = _indicatorStatusScreen.captureState(builder);
    state.images = std::move(builder).finish();

    state.margins = MarginsState { .mainDisplay = _mainDisplayMargin,
                                   .hostWritable = _hostWritableScreenMargin,
                                   .indicator = _indicatorScreenMargin,
                                   .tabs = _tabs };

    state.colors.current = sliceOf(_colorPalette);
    for (auto const& saved: _savedColorPalettes)
        state.colors.saved.push_back(sliceOf(saved));
    state.colors.lastSaved = _lastSavedColorPalette;

    state.titles = TitlesState { .windowTitle = _windowTitle,
                                 .iconTitle = _iconTitle,
                                 .savedTitles = _savedTitles,
                                 .tabName = _tabName,
                                 .titleModes = static_cast<uint8_t>(_titleModes.to_ulong()) };

    state.profile = ProfileState { .terminalId = _terminalId,
                                   .operatingLevel = _operatingLevel,
                                   .c1TransmissionMode = _c1TransmissionMode,
                                   .userPreferredSupplementalSet = _userPreferredSupplementalSet,
                                   .checksumExtension = _checksumExtension.value(),
                                   .maxSixelColorRegisters = _maxSixelColorRegisters,
                                   .maxImageRegisterCount = _maxImageRegisterCount,
                                   .usePrivateColorRegisters = _usePrivateColorRegisters,
                                   .negotiatedImageCanvasSize = _negotiatedImageCanvasSize,
                                   .cursorDisplay = _settings.cursorDisplay,
                                   .cursorShape = _settings.cursorShape,
                                   .warningBellVolume = _settings.warningBellVolume,
                                   .marginBellVolume = _settings.marginBellVolume,
                                   .smoothLineScrolling = _settings.smoothLineScrolling,
                                   .modeGatedSequences = {} };
    for (auto const index: std::views::iota(size_t { 0 }, ModeGatedSequences.size()))
        state.profile.modeGatedSequences.at(index) =
            std::ranges::contains(_supportedVTSequences.activeSequences(), ModeGatedSequences.at(index))
                ? SequenceAvailability::Active
                : SequenceAvailability::Inactive;

    state.macros.macros = std::map<int, std::string>(_macros.begin(), _macros.end());
    state.macros.running = _runningMacros;
    if (_pendingMacroInvocation)
        state.macros.pending =
            PendingMacroState { .id = _pendingMacroInvocation->id, .body = _pendingMacroInvocation->body };
    state.macros.areRunningMacrosVoid = _areRunningMacrosVoid;
    state.macros.bytesRun = _macroBytesRun;
    state.macros.pendingLocalEcho = _pendingLocalEcho;

    state.keyStrings = KeyStringsState {
        .userDefinedKeys = std::map<int, std::string>(_userDefinedKeys.begin(), _userDefinedKeys.end()),
        .isUdkLocked = _udkLocked,
        .locator = _locatorState,
        .locatorButtonsHeld = _locatorButtonsHeld,
        .locatorButtonsOwnedByApp = _locatorButtonsOwnedByApp,
        .drcsCharsets = std::map<int, DRCSCharset>(_drcsCharsets.begin(), _drcsCharsets.end()),
        .drcsDesignators = std::map<std::string, int>(_drcsDesignatorMap.begin(), _drcsDesignatorMap.end()),
    };

    if (_sixelColorPalette)
        state.sixel = SixelPaletteState { .registers = _sixelColorPalette->registers(),
                                          .maxSize = _sixelColorPalette->maxSize(),
                                          .registersAtHook = _sixelRegistersAtHook };

    state.hyperlinks = _hyperlinks.captureState();

    state.session = SessionState {
        .kittyClipboardWrite = _kittyClipboardWrite,
        .isKittyClipboardWriteOpen = _kittyClipboardWriteOpen,
        .currentWorkingDirectory = _currentWorkingDirectory,
        .progress = _progress,
        .programStatus = std::vector<ProgramStatusRecord>(_programStatus.records().begin(),
                                                          _programStatus.records().end()),
        .pointerShapes = _pointerShapes,
        .isPointerShapeBaseSetByApplication = _pointerShapeBaseSetByApplication,
        .userVars = _userVars,
        .instructionCounter = _instructionCounter,
        .sessionEpoch = _sessionEpoch,
        .hostClockOffset = _hostClockOffset,
        .userInputLock = _userInputLock.load(),
        .synchronizedOutputFor = {},
    };
    if (!_renderBufferUpdateEnabled.load(std::memory_order_acquire))
        state.session.synchronizedOutputFor = std::chrono::duration_cast<std::chrono::nanoseconds>(
            _clocks.steady.now() - _synchronizedOutputSince.load(std::memory_order_acquire));

    {
        auto const lock = std::scoped_lock { _inputMutex };
        state.input = _inputGenerator.captureState();
    }

    state.shell = ShellState {
        .commandBlocks = _commandBlocks.captureState(),
        .semanticBlocks = _semanticBlockTracker.captureState(),
        .contexts = _contexts.captureState(),
        .markArbiter = _markArbiter.captureState(),
        .userMarks = std::vector<int64_t>(_userMarks.ids().begin(), _userMarks.ids().end()),
        .userMarksGeneration = _userMarks.generation(),
        .userMarkChanges = _userMarkChanges,
        .collapsedFolds =
            std::vector<int64_t>(_foldState.collapsedIds().begin(), _foldState.collapsedIds().end()),
        .foldStateGeneration = _foldStateGeneration,
        .foldStateFloor = _foldStateFloor,
        .commandBlockHeadsGeneration = _commandBlockHeadsGeneration,
        .commandBlockHeadsRowChanges = _commandBlockHeadsRowChanges,
        .commandBlockHeadsFloor = _commandBlockHeadsFloor,
        .commandBlockRowsMoved = _commandBlockRowsMoved,
        .commandBlockHeadWork = _commandBlockHeadWork,
        .hoveredCommandBlock = _hoveredCommandBlock,
        .focusedCommandBlock = _focusedCommandBlock,
    };

    state.interaction = InteractionState {
        .viInput = _inputHandler.captureState(),
        .viCommands = _viCommands.captureState(),
        .selection = {},
        .customWordDelimiters = _customWordDelimiters,
        .search =
            SearchState { .pattern = _search.pattern, .options = _search.options, .origin = _search.origin },
    };
    if (_hintModeHandler.isActive())
    {
        auto hint = _hintModeHandler.captureState();
        hint.scope = _hintScope;
        hint.previousViMode = _hintModeExecutor.previousViMode;
        hint.directory = _hintDirectory;
        state.interaction.hint = std::move(hint);
    }

    if (_selection)
    {
        auto const kind = _selection->kind();
        auto const helper = [&] {
            if (&_selection->helper() == &_extendedSelectionHelper)
                return SelectionHelperKind::Extended;
            if (&_selection->helper() == &_customSelectionHelper)
                return SelectionHelperKind::Custom;
            return SelectionHelperKind::Standard;
        }();
        state.interaction.selection = SelectionState { .kind = kind,
                                                       .helper = helper,
                                                       .from = _selection->from(),
                                                       .to = _selection->to(),
                                                       .state = _selection->state() };
    }

    state.notifications = _desktopNotificationManager.captureState();

    state.parser = ParserState {
        .grammar = _parser.isVT52Mode() ? ParserGrammar::Vt52 : ParserGrammar::Ansi,
        .precedingGraphicCharacter = _parser.precedingGraphicCharacter(),
        .hookOrigin = _inFlightHookOrigin,
    };
    state.inFlightTailOverflowed = _inFlightTailCompleteness;
    if (_inFlightTailCompleteness == TailCompleteness::Complete)
    {
        // The half-decoded character first: the state machine leaves the decoder alone outside Ground, so it
        // was as it is now when the sequence in flight began.
        auto const pending = _parser.pendingCharacterBytes();
        state.inFlightTail.reserve(pending.size() + _inFlightTail.size());
        auto const toByte = [](char ch) {
            return static_cast<uint8_t>(ch);
        };
        std::ranges::transform(pending, std::back_inserter(state.inFlightTail), toByte);
        std::ranges::transform(_inFlightTail, std::back_inserter(state.inFlightTail), toByte);
    }
    return state;
}

std::expected<void, StateDecodeError> Terminal::validateRestore(TerminalState const& state) const
{
    auto const fails = [](std::expected<void, StateDecodeError> const& result) {
        return !result.has_value();
    };
    auto const& paging = state.paging;
    auto const& construction = state.construction;
    auto checker = EnumChecker {};
    checker.check(state);
    if (!checker.isListed() || state.pages.size() != _pages.size()
        || construction != constructionOf(_settings, _pages[0]->grid().historyLimits())
        || !isPage(paging.cursorPage) || !isPage(paging.displayedPage)
        || !isPage(paging.cursorPageBeforeAlternate) || !isPage(paging.displayedPageBeforeAlternate)
        || !std::ranges::all_of(paging.savedCursorPage, isPage)
        || state.titles.titleModes >= (1U << TitleModeFeatureCount) || state.session.pointerShapes.empty()
        || state.session.programStatus.size() > _programStatus.maxRecords()
        || state.inFlightTailOverflowed != TailCompleteness::Complete
        || state.colors.saved.size() > MaxColorPaletteSaveStackSize
        || state.colors.lastSaved > state.colors.saved.size()
        // A capture is taken between two parses, where no macro runs.
        || !state.macros.running.empty())
        return StateFailure { StateDecodeError::InvalidValue };

    // Only the page the cursor is on has the terminal's size, less the status line it shows: a resize leaves
    // the others as they are until they are switched to.
    auto const statusLines =
        paging.statusDisplayType == StatusDisplayType::None ? LineCount(0) : StatusLineHeight;
    auto const& cursorGrid = state.pages[static_cast<size_t>(paging.cursorPage.value)].grid;
    auto const& displayedGrid = state.pages[static_cast<size_t>(paging.displayedPage.value)].grid;
    auto const statusPage = PageSize { StatusLineHeight, construction.pageSize.columns };
    // The indicator's row is as wide as the page and the gutter together, as the gutter's settings stood when
    // it was last laid out: never narrower than the page, nor wider than the page beside the widest gutter.
    auto const isIndicatorPage = [&](PageSize size) {
        return size.lines == statusPage.lines && size.columns >= statusPage.columns
               && size.columns <= statusPage.columns + ColumnCount(MaxGutterColumns);
    };
    auto const isTab = [&](ColumnOffset column) {
        return column >= ColumnOffset(0) && column < boxed_cast<ColumnOffset>(cursorGrid.pageSize.columns);
    };
    auto const& selection = state.interaction.selection;
    if (cursorGrid.pageSize != construction.pageSize - statusLines
        || state.hostWritableStatusLine.grid.pageSize != statusPage
        || !isIndicatorPage(state.indicatorStatusLine.grid.pageSize)
        || unbox<int64_t>(paging.scrollOffset) < 0
        || unbox<int64_t>(paging.scrollOffset)
               > unbox<int64_t>(displayedGrid.linesUsed) - unbox<int64_t>(displayedGrid.pageSize.lines)
        || !isInside(state.margins.mainDisplay, cursorGrid.pageSize)
        || !isInside(state.margins.hostWritable, statusPage) || !isInside(state.margins.indicator, statusPage)
        || !std::ranges::all_of(state.margins.tabs, isTab) || !std::ranges::is_sorted(state.margins.tabs)
        || (selection
            && (!isSelectable(selection->from, cursorGrid) || !isSelectable(selection->to, cursorGrid))))
        return StateFailure { StateDecodeError::InvalidValue };

    if (fails(Modes::validateState(state.modes)) || fails(_hyperlinks.validateState(state.hyperlinks))
        || fails(InputGenerator::validateState(state.input))
        || fails(_commandBlocks.validateState(state.shell.commandBlocks))
        || fails(SemanticBlockTracker::validateState(state.shell.semanticBlocks))
        || fails(_contexts.validateState(state.shell.contexts))
        || fails(ViCommands::validateState(state.interaction.viCommands))
        || fails(DesktopNotificationManager::validateState(state.notifications)))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> Terminal::restoreState(TerminalState const& state)
{
    // A restore is the first thing a terminal does: nothing was parsed yet.
    Require(_instructionCounter == 0);

    // Everything that can be refused is refused before the first member changes: the checks, then the image
    // table and every screen built aside. A refused restore leaves the terminal as it was constructed; the
    // images built aside drop out of the pool's index again with their last reference.
    auto images = std::optional<RestoredImageTable> {};
    auto screens = std::vector<StagedScreen> {};
    try
    {
        if (auto const valid = validateRestore(state); !valid)
            return valid;
        auto table = RestoredImageTable::build(_imagePool, state.images);
        if (!table)
            return StateFailure { table.error() };
        images = std::move(*table);
        screens.reserve(_pages.size() + 2);
        auto const stage = [&](Screen const& screen, ScreenState const& screenState) {
            auto staged = screen.stageRestore(screenState, *images);
            if (!staged)
                return std::expected<void, StateDecodeError> { StateFailure { staged.error() } };
            screens.push_back(*std::move(staged));
            return std::expected<void, StateDecodeError> {};
        };
        for (auto const index: std::views::iota(size_t { 0 }, _pages.size()))
            if (auto const done = stage(*_pages[index], state.pages[index]); !done)
                return done;
        if (auto const done = stage(_hostWritableStatusLineScreen, state.hostWritableStatusLine); !done)
            return done;
        if (auto const done = stage(_indicatorStatusScreen, state.indicatorStatusLine); !done)
            return done;
    }
    catch (std::bad_alloc const&)
    {
        // The one exception a malformed stream can raise: a size no check above could bound.
        return StateFailure { StateDecodeError::TooLarge };
    }

    commitRestore(state, *images, screens);
    announceRestoredState();
    return {};
}

void Terminal::commitRestore(TerminalState const& state,
                             RestoredImageTable const& images,
                             std::span<StagedScreen> screens)
{
    // Nothing here can be refused: validateRestore() and the staging checked all of it.
    auto const succeeded = [](std::expected<void, StateDecodeError> const& result) {
        Require(result.has_value());
    };
    succeeded(_modes.restoreState(state.modes));
    images.commit(_imagePool);
    for (auto const index: std::views::iota(size_t { 0 }, _pages.size()))
        _pages[index]->commitRestore(std::move(screens[index]));
    _hostWritableStatusLineScreen.commitRestore(std::move(screens[_pages.size()]));
    _indicatorStatusScreen.commitRestore(std::move(screens[_pages.size() + 1]));

    auto const& paging = state.paging;
    _cursorPage = paging.cursorPage;
    _displayedPage = paging.displayedPage;
    _pagesBeforeAlternateScreen = { .cursor = paging.cursorPageBeforeAlternate,
                                    .displayed = paging.displayedPageBeforeAlternate };
    _savedCursorPage = paging.savedCursorPage;
    _activeStatusDisplay = paging.activeStatusDisplay;
    switch (_activeStatusDisplay)
    {
        case ActiveStatusDisplay::Main: _currentScreen = _pages[_cursorPage.value].get(); break;
        case ActiveStatusDisplay::StatusLine: _currentScreen = &_hostWritableStatusLineScreen; break;
        case ActiveStatusDisplay::IndicatorStatusLine: _currentScreen = &_indicatorStatusScreen; break;
    }
    _currentScreenType = screenTypeFromPage(_cursorPage);
    _displayedScreenType = screenTypeFromPage(_displayedPage);
    _statusDisplayType = paging.statusDisplayType;
    _selectedStatusDisplayType = paging.selectedStatusDisplayType;
    _savedStatusDisplayType = paging.savedStatusDisplayType;
    _statusNotices = paging.statusNotices;
    _deniedStatusLineTitle =
        paging.isDeniedStatusLineTitleOwed ? DeniedStatusLineTitle::Owed : DeniedStatusLineTitle::Settled;
    _viewport.restoreScrollOffset(paging.scrollOffset);
    _displayedPageLinesFollowed = paging.displayedPageLinesFollowed;

    _mainDisplayMargin = state.margins.mainDisplay;
    _hostWritableScreenMargin = state.margins.hostWritable;
    _indicatorScreenMargin = state.margins.indicator;
    _tabs = state.margins.tabs;

    // The palette is overlaid on what this process was configured with; the saved slots likewise.
    _savedColorPalettes.clear();
    for (auto const& slice: state.colors.saved)
    {
        auto& saved = _savedColorPalettes.emplace_back(_colorPalette);
        overlay(saved, slice);
    }
    _lastSavedColorPalette = static_cast<size_t>(state.colors.lastSaved);
    overlay(_colorPalette, state.colors.current);
    refreshDefaultBackground();

    _windowTitle = state.titles.windowTitle;
    _iconTitle = state.titles.iconTitle;
    _savedTitles = state.titles.savedTitles;
    _tabName = state.titles.tabName;
    _titleModes = std::bitset<TitleModeFeatureCount>(state.titles.titleModes);

    _terminalId = state.profile.terminalId;
    // The sequences the level makes available, then those a mode turned on or off since.
    setOperatingLevel(state.profile.operatingLevel);
    for (auto const index: std::views::iota(size_t { 0 }, ModeGatedSequences.size()))
    {
        if (state.profile.modeGatedSequences.at(index) == SequenceAvailability::Active)
            _supportedVTSequences.enableSequence(ModeGatedSequences.at(index));
        else
            _supportedVTSequences.disableSequence(ModeGatedSequences.at(index));
    }
    _c1TransmissionMode = state.profile.c1TransmissionMode;
    _userPreferredSupplementalSet = state.profile.userPreferredSupplementalSet;
    _checksumExtension = ChecksumFlags::fromValue(state.profile.checksumExtension);
    _maxSixelColorRegisters = state.profile.maxSixelColorRegisters;
    _maxImageRegisterCount = state.profile.maxImageRegisterCount;
    _usePrivateColorRegisters = state.profile.usePrivateColorRegisters;
    _negotiatedImageCanvasSize = state.profile.negotiatedImageCanvasSize;
    _settings.cursorDisplay = state.profile.cursorDisplay;
    _settings.cursorShape = state.profile.cursorShape;
    _settings.warningBellVolume = state.profile.warningBellVolume;
    _settings.marginBellVolume = state.profile.marginBellVolume;
    _settings.smoothLineScrolling = state.profile.smoothLineScrolling;

    _macros = std::unordered_map<int, std::string>(state.macros.macros.begin(), state.macros.macros.end());
    _runningMacros.clear();
    _pendingMacroInvocation.reset();
    if (state.macros.pending)
        _pendingMacroInvocation =
            MacroInvocation { .id = state.macros.pending->id, .body = state.macros.pending->body };
    _areRunningMacrosVoid = state.macros.areRunningMacrosVoid;
    _macroBytesRun = static_cast<size_t>(state.macros.bytesRun);
    _pendingLocalEcho = state.macros.pendingLocalEcho;

    _userDefinedKeys = std::unordered_map<int, std::string>(state.keyStrings.userDefinedKeys.begin(),
                                                            state.keyStrings.userDefinedKeys.end());
    _udkLocked = state.keyStrings.isUdkLocked;
    _locatorState = state.keyStrings.locator;
    _locatorButtonsHeld = state.keyStrings.locatorButtonsHeld;
    _locatorButtonsOwnedByApp = state.keyStrings.locatorButtonsOwnedByApp;
    _drcsCharsets = std::unordered_map<int, DRCSCharset>(state.keyStrings.drcsCharsets.begin(),
                                                         state.keyStrings.drcsCharsets.end());
    _drcsDesignatorMap = std::unordered_map<std::string, int>(state.keyStrings.drcsDesignators.begin(),
                                                              state.keyStrings.drcsDesignators.end());

    // Present on both sides: a terminal always has the shared palette, and is built here with the size the
    // state carries (ConstructionState::maxImageRegisterCount).
    if (state.sixel)
        _sixelColorPalette->restore(state.sixel->registers, state.sixel->maxSize);

    succeeded(_hyperlinks.restoreState(state.hyperlinks));
    succeeded(_desktopNotificationManager.restoreState(state.notifications));

    _kittyClipboardWrite = state.session.kittyClipboardWrite;
    _kittyClipboardWriteOpen = state.session.isKittyClipboardWriteOpen;
    _currentWorkingDirectory = state.session.currentWorkingDirectory;
    _progress = state.session.progress;
    std::ignore = _programStatus.replaceAll(state.session.programStatus);
    _pointerShapes = state.session.pointerShapes;
    _pointerShapeBaseSetByApplication = state.session.isPointerShapeBaseSetByApplication;
    _userVars = state.session.userVars;
    _instructionCounter = state.session.instructionCounter;
    _sessionEpoch = state.session.sessionEpoch;
    _hostClockOffset = state.session.hostClockOffset;
    _userInputLock = state.session.userInputLock;
    _renderBufferUpdateEnabled = !state.session.synchronizedOutputFor.has_value();
    if (state.session.synchronizedOutputFor)
        _synchronizedOutputSince.store(_clocks.steady.now()
                                           - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               *state.session.synchronizedOutputFor),
                                       std::memory_order_release);

    {
        auto const lock = std::scoped_lock { _inputMutex };
        succeeded(_inputGenerator.restoreState(state.input));
    }

    auto const& shell = state.shell;
    succeeded(_commandBlocks.restoreState(shell.commandBlocks));
    succeeded(_semanticBlockTracker.restoreState(shell.semanticBlocks));
    succeeded(_contexts.restoreState(shell.contexts));
    _markArbiter.restoreState(shell.markArbiter);
    _userMarks.rebuild(shell.userMarks, shell.userMarksGeneration);
    _userMarkChanges = shell.userMarkChanges;
    _foldState.expandAll();
    for (auto const head: shell.collapsedFolds)
        _foldState.collapse(head);
    _foldStateGeneration = shell.foldStateGeneration;
    _foldStateFloor = shell.foldStateFloor;
    _commandBlockHeadsGeneration = shell.commandBlockHeadsGeneration;
    _commandBlockHeadsRowChanges = shell.commandBlockHeadsRowChanges;
    _commandBlockHeadsFloor = shell.commandBlockHeadsFloor;
    _commandBlockRowsMoved = shell.commandBlockRowsMoved;
    _commandBlockHeadWork = shell.commandBlockHeadWork;
    _hoveredCommandBlock = shell.hoveredCommandBlock;
    _focusedCommandBlock = shell.focusedCommandBlock;

    succeeded(_viCommands.restoreState(state.interaction.viCommands));
    _inputHandler.restoreState(state.interaction.viInput);
    if (auto const& hint = state.interaction.hint)
    {
        // Reopened as it was, labels and filter included, not re-scanned: a scrollback session's labels are
        // fixed for the session, and a re-scan would forget the filter typed so far.
        _hintScope = hint->scope;
        _hintDirectory = hint->directory;
        _hintModeExecutor.previousViMode = hint->previousViMode;
        _hintModeHandler.restoreState(*hint,
                                      HintPatternSet { .table = _settings.linkPatterns,
                                                       .names = {},
                                                       .resolve = hintResolver(_hintDirectory) });
    }

    setCustomWordDelimiters(state.interaction.customWordDelimiters);
    _selection.reset();
    if (auto const& selection = state.interaction.selection)
    {
        auto const& helper = [&]() -> SelectionHelper const& {
            switch (selection->helper)
            {
                case SelectionHelperKind::Standard: break;
                case SelectionHelperKind::Extended: return _extendedSelectionHelper;
                case SelectionHelperKind::Custom: return _customSelectionHelper;
            }
            return _selectionHelper;
        }();
        auto restored = [&]() -> std::unique_ptr<Selection> {
            switch (selection->kind)
            {
                case SelectionKind::Linear: break;
                case SelectionKind::Rectangular:
                    return std::make_unique<RectangularSelection>(
                        helper, selection->from, selectionUpdatedHelper());
                case SelectionKind::WordWise:
                    return std::make_unique<WordWiseSelection>(
                        helper, selection->from, selectionUpdatedHelper());
                case SelectionKind::FullLine:
                    return std::make_unique<FullLineSelection>(
                        helper, selection->from, selectionUpdatedHelper());
            }
            return std::make_unique<LinearSelection>(helper, selection->from, selectionUpdatedHelper());
        }();
        restored->restoreRange(selection->from, selection->to, selection->state);
        setSelector(std::move(restored));
    }

    _search.pattern = state.interaction.search.pattern;
    _search.options = state.interaction.search.options;
    _search.origin = state.interaction.search.origin;
    compileSearch();
    publishBindingModes();

    // The parser last: what it has in flight is replayed onto everything else as it is.
    _parser.setVT52Mode(state.parser.grammar == ParserGrammar::Vt52);
    _parser.setPrecedingGraphicCharacter(state.parser.precedingGraphicCharacter);
    _inFlightHookOrigin = state.parser.hookOrigin;
    if (state.sixel && state.sixel->registersAtHook)
        _sixelColorPalette->restore(*state.sixel->registersAtHook, state.sixel->maxSize);
    replayInFlightTail(std::string_view { reinterpret_cast<char const*>(state.inFlightTail.data()),
                                          state.inFlightTail.size() });

    invalidateFoldRanges();
    _screenDirty.store(true);
    _changes.fetch_add(1);
}

void Terminal::announceRestoredState()
{
    // One row per fact a listener keeps a copy of. A restore replaces them all without going through the
    // setters that announce each, so it announces them here, together, once the terminal is whole.
    using Announce = void (*)(Terminal&);
    static constexpr auto Announcements = std::array<Announce, 11> {
        [](Terminal& terminal) { terminal._eventListener.setWindowTitle(terminal._windowTitle); },
        [](Terminal& terminal) { terminal._eventListener.setIconTitle(terminal._iconTitle); },
        [](Terminal& terminal) {
            if (terminal._tabName)
                terminal._eventListener.setTabName(*terminal._tabName);
        },
        [](Terminal& terminal) { terminal._eventListener.bufferChanged(terminal._currentScreenType); },
        [](Terminal& terminal) { terminal._eventListener.setPointerShape(terminal._pointerShapes.back()); },
        [](Terminal& terminal) { terminal._eventListener.progressChanged(terminal._progress); },
        [](Terminal& terminal) { terminal._eventListener.workingDirectoryChanged(); },
        [](Terminal& terminal) { terminal._eventListener.contextChanged(terminal._contexts.activeId()); },
        [](Terminal& terminal) { terminal._eventListener.inputModeChanged(terminal._inputHandler.mode()); },
        [](Terminal& terminal) { terminal._eventListener.screenUpdated(); },
        // The kitty animations playing on the page on display: what arms the daemon's frame timer.
        [](Terminal& terminal) {
            terminal.publishKittyAnimationDeadline(terminal.displayedPage().nextKittyFrameDue());
        },
    };
    for (auto const announce: Announcements)
        announce(*this);
}

void Terminal::replayInFlightTail(std::string_view tail)
{
    if (tail.empty())
        return;

    // The bytes in flight are replayed for the state they leave the parser in, and everything they did
    // already happened to the state restored. Inside a sequence the state machine acts in three ways only:
    //
    //   - It executes a C0 control (VT500 parsers execute BS, CR, LF, ... inside ESC, CSI and a DCS's
    //     parameters). That is suppressed here: its effect is in the state, and executing changes nothing
    //     the state machine itself holds, so the parser ends up where it would have.
    //   - It hooks a DCS. The hook builds the sub-parser afresh from what it was built from first: the
    //     sequence, which the replay rebuilds; the terminal's state, which nothing in flight changed (a C0
    //     control cannot reach what a hook reads); and where the hook was, carried as hookOrigin(). What the
    //     hook does to the terminal is idempotent (a ReGIS Pmode reset; ensureReGIS()), or counted
    //     (the instruction counter, put back below).
    //   - It hands payload to the hooked sub-parser. Only sixel's acts before the string ends, redefining the
    //     shared registers -- restored as they were at the hook, so it redefines them again in order. ReGIS
    //     buffers its payload, and so do the collectors: their effects come at the unhook.
    //
    // It prints nothing: text is in Ground, which a sequence in flight has left, and DCS_Ignore drops its
    // payload. A half-decoded character ahead of the sequence is only decoded on. So the replay leaves the
    // terminal as it was, with the parser where the captured one stood.
    auto const instructionCounter = _instructionCounter;
    _tailReplay = TailReplay::Replaying;
    parseFragmentChunked(tail);
    _tailReplay = TailReplay::Off;
    _instructionCounter = instructionCounter;
}

// }}}

std::expected<Settings, StateDecodeError> settingsFor(TerminalState const& state, Settings base)
{
    auto const& construction = state.construction;
    auto const isBounded = [](MaxHistoryLineCount const& count) {
        auto const* lines = std::get_if<LineCount>(&count);
        return !lines || (*lines >= LineCount(0) && *lines <= LineCount(MaxStateHistoryLines));
    };
    if (construction.pageSize.lines < LineCount(1) || construction.pageSize.columns < ColumnCount(1))
        return StateFailure { StateDecodeError::InvalidValue };
    if (construction.pageSize.lines > LineCount(MaxStateHistoryLines)
        || construction.pageSize.columns > ColumnCount(MaxStateLineColumns)
        || !isBounded(construction.historyLimits.guaranteed)
        || !isBounded(construction.historyLimits.capacity)
        || construction.hyperlinkCacheCapacity > std::numeric_limits<uint16_t>::max()
        || construction.maxProgramStatusRecords > MaxStateHistoryLines
        || construction.maxCommandBlocks > MaxStateHistoryLines
        || construction.maxContextDepth > construction.maxRetainedContexts
        || construction.maxRetainedContexts > MaxStateHistoryLines
        || construction.maxImageRegisterCount > MaxStateHistoryLines)
        return StateFailure { StateDecodeError::TooLarge };
    base.pageSize = construction.pageSize;
    base.historyLimits = construction.historyLimits;
    base.primaryScreen.allowReflowOnResize = construction.primaryReflow == ReflowOnResize::Yes;
    base.maxImageRegisterCount = construction.maxImageRegisterCount;
    base.hyperlinkCacheCapacity = static_cast<size_t>(construction.hyperlinkCacheCapacity);
    base.maxProgramStatusRecords = static_cast<size_t>(construction.maxProgramStatusRecords);
    base.commandBlockLimits.maxRecords = static_cast<size_t>(construction.maxCommandBlocks);
    base.contextLimits.maxDepth = static_cast<size_t>(construction.maxContextDepth);
    base.contextLimits.maxRetained = static_cast<size_t>(construction.maxRetainedContexts);
    return base;
}

} // namespace vtbackend
