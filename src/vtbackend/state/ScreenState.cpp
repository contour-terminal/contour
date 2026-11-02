// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/ScreenState.hpp>

#include <vtbackend/graphics/SixelParser.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <ranges>

namespace vtbackend
{

namespace
{
    using detail::StateFailure;

    /// @return Whether @p location is a cell of a page of @p size.
    [[nodiscard]] bool isOnPage(CellLocation location, PageSize size) noexcept
    {
        return location.line >= LineOffset(0) && location.line < boxed_cast<LineOffset>(size.lines)
               && location.column >= ColumnOffset(0)
               && location.column < boxed_cast<ColumnOffset>(size.columns);
    }
} // namespace

// {{{ CharsetMapping

CharsetState CharsetMapping::captureState() const noexcept
{
    return CharsetState { .charsetIds = _charsetIds,
                          .is96Charset = _is96Charset,
                          .isUpss = _isUpss,
                          .drcsFontNumber = _drcsFontNumber,
                          .tableForNextGraphic = _tableForNextGraphic,
                          .selectedTable = _selectedTable,
                          .selectedTableGR = _selectedTableGR };
}

std::expected<void, StateDecodeError> CharsetMapping::restoreState(CharsetState const& state) noexcept
{
    constexpr auto IsKnownTable = [](CharsetTable table) {
        return table <= CharsetTable::G3;
    };
    constexpr auto IsKnownSet = [](CharsetId id) {
        return id <= CharsetId::ISOLatin1Supplemental;
    };
    if (!std::ranges::all_of(state.charsetIds, IsKnownSet) || !IsKnownTable(state.tableForNextGraphic)
        || !IsKnownTable(state.selectedTable) || !IsKnownTable(state.selectedTableGR))
        return StateFailure { StateDecodeError::InvalidValue };

    _charsetIds = state.charsetIds;
    _is96Charset = state.is96Charset;
    _isUpss = state.isUpss;
    _drcsFontNumber = state.drcsFontNumber;
    _tableForNextGraphic = state.tableForNextGraphic;
    _selectedTable = state.selectedTable;
    _selectedTableGR = state.selectedTableGR;
    for (auto const index: std::views::iota(size_t { 0 }, _tables.size()))
        _tables[index] = charsetMap(_charsetIds[index]);
    return {};
}

// }}}

// {{{ Screen

ScreenState Screen::captureState(ImageTableBuilder& images) const
{
    auto state = ScreenState { .grid = _grid.captureState(images),
                               .cursor = _cursor.captureState(),
                               .savedCursor = _savedCursor.captureState(),
                               .savedGraphicsRenditions = _savedGraphicsRenditions,
                               .lastCursorPosition = _lastCursorPosition,
                               .stamp = _stamp,
                               .blockRowChanges = _blockRowChanges,
                               .restampWork = _restampWork,
                               .linesScrolledIntoHistory = _linesScrolledIntoHistory,
                               .livePromptMarks = _livePromptMarks.entries(),
                               .promptRedraw = _promptRedraw,
                               .outputStartMarking = _outputStartMarking,
                               .kitty = captureKittyState(images),
                               .regis = {},
                               .rectangularAttributeMode = _rectangularAttributeMode,
                               .isoProtectionActive = _isoProtectionActive,
                               .enableLocalFunctions = _enableLocalFunctions,
                               .localFunctionKeyControl = _localFunctionKeyControl,
                               .modifierKeyReporting = _modifierKeyReporting };
    if (_regisContext && _regisCanvas)
        state.regis = regis::ReGISState::capture(*_regisContext, *_regisCanvas);
    return state;
}

std::expected<StagedScreen, StateDecodeError> Screen::stageRestore(
    ScreenState const& state, ImageFragmentResolver const& resolver) const
{
    if (!isOnPage(state.cursor.position, state.grid.pageSize)
        || !isOnPage(state.savedCursor.position, state.grid.pageSize)
        || (state.regis && !state.regis->isConsistent()))
        return StateFailure { StateDecodeError::InvalidValue };
    auto staged =
        StagedScreen { .state = &state, .resolver = &resolver, .grid = {}, .cursor = {}, .savedCursor = {} };
    if (auto const done = staged.cursor.restoreState(state.cursor).and_then([&] {
            return staged.savedCursor.restoreState(state.savedCursor);
        });
        !done)
        return StateFailure { done.error() };
    if (auto const valid = validateKittyState(state.kitty, resolver); !valid)
        return StateFailure { valid.error() };
    auto grid = _grid.stageRestore(state.grid, resolver);
    if (!grid)
        return StateFailure { grid.error() };
    staged.grid = *std::move(grid);
    return staged;
}

void Screen::commitRestore(StagedScreen staged)
{
    auto const& state = *staged.state;
    _grid.commitRestore(std::move(staged.grid));
    auto const kitty = restoreKittyState(state.kitty, *staged.resolver);
    Require(kitty.has_value());

    _cursor = staged.cursor;
    _savedCursor = staged.savedCursor;
    _savedGraphicsRenditions = state.savedGraphicsRenditions;
    _lastCursorPosition = state.lastCursorPosition;
    _stamp = state.stamp;
    _blockRowChanges = state.blockRowChanges;
    _restampWork = state.restampWork;
    _linesScrolledIntoHistory = state.linesScrolledIntoHistory;
    _livePromptMarks.restore(state.livePromptMarks);
    _promptRedraw = state.promptRedraw;
    _outputStartMarking = state.outputStartMarking;
    _rectangularAttributeMode = state.rectangularAttributeMode;
    _isoProtectionActive = state.isoProtectionActive;
    _enableLocalFunctions = state.enableLocalFunctions;
    _localFunctionKeyControl = state.localFunctionKeyControl;
    _modifierKeyReporting = state.modifierKeyReporting;

    if (state.regis)
    {
        ensureReGIS();
        *_regisContext = state.regis->context;
        _regisCanvas->restore(state.regis->canvasSize, state.regis->pixels, state.regis->patternPhase);
    }

    // Not updateCursorIterator(): that would stamp the row, and the rows already carry what they were born
    // with.
    _currentLine = &_grid.lineAt(_cursor.position.line);
    _sixelImageBuilder.reset();
}

std::expected<void, StateDecodeError> Screen::restoreState(ScreenState const& state,
                                                           ImageFragmentResolver const& resolver)
{
    return stageRestore(state, resolver).transform([this](StagedScreen staged) {
        commitRestore(std::move(staged));
    });
}

// }}}

} // namespace vtbackend
