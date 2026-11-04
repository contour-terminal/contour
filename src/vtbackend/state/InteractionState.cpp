// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/InteractionState.hpp>

#include <utility>

namespace vtbackend
{

ViInputHandlerState ViInputHandler::captureState() const
{
    return ViInputHandlerState {
        .mode = _viMode, .pendingInput = _pendingInput, .count = _count, .lastChar = _lastChar
    };
}

void ViInputHandler::restoreState(ViInputHandlerState const& state)
{
    _viMode = state.mode;
    _pendingInput = state.pendingInput;
    _count = state.count;
    _lastChar = state.lastChar;
}

HintSessionState HintModeHandler::captureState() const
{
    return HintSessionState { .action = _action,
                              .scope = HintScope::Visible,
                              .patternNames = _patterns.names,
                              .matches = _allMatches,
                              .filter = _filter,
                              .previousViMode = std::nullopt,
                              .directory = std::nullopt };
}

void HintModeHandler::restoreState(HintSessionState const& state, HintPatternSet patterns)
{
    _active = true;
    _action = state.action;
    _patterns = std::move(patterns);
    _patterns.names = state.patternNames;
    _allMatches = state.matches;
    _filter = state.filter;
    // The filtered list is the labels the filter is a prefix of, whichever way the session got there.
    updateFilteredMatches();
}

ViCommandsState ViCommands::captureState() const
{
    return ViCommandsState { .cursorPosition = cursorPosition,
                             .lastMode = _lastMode,
                             .lastCursorShape = _lastCursorShape,
                             .lastChar = _lastChar,
                             .lastCharMotion = _lastCharMotion,
                             .lastCursorVisible = _lastCursorVisible,
                             .jumpHistory = _jumpHistory.captureState() };
}

std::expected<void, StateDecodeError> ViCommands::validateState(ViCommandsState const& state)
{
    return JumpHistory::validateState(state.jumpHistory);
}

std::expected<void, StateDecodeError> ViCommands::restoreState(ViCommandsState const& state)
{
    if (auto const done = _jumpHistory.restoreState(state.jumpHistory); !done)
        return done;
    cursorPosition = state.cursorPosition;
    _lastMode = state.lastMode;
    _lastCursorShape = state.lastCursorShape;
    _lastChar = state.lastChar;
    _lastCharMotion = state.lastCharMotion;
    _lastCursorVisible = state.lastCursorVisible;
    return {};
}

} // namespace vtbackend
