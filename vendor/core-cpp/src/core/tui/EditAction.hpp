// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// Actions that can be performed on text in an InputField.
///
/// These actions can be bound to key combinations via KeyBindings.
/// The keybinding system maps KeyEvents to EditActions, which are then
/// executed by the InputField.
///
/// This design allows for:
/// - Configurable keybindings (users can rebind keys)
/// - Multiple keys bound to the same action
/// - Future support for different editing modes (e.g., vi mode)
enum class EditAction : std::uint8_t
{
    None, ///< No action (key not bound)

    // === Movement ===
    MoveForwardChar,      ///< Move cursor forward one character
    MoveBackwardChar,     ///< Move cursor backward one character
    MoveForwardWord,      ///< Move cursor forward one word
    MoveBackwardWord,     ///< Move cursor backward one word
    MoveToLineStart,      ///< Move cursor to start of current line
    MoveToLineEnd,        ///< Move cursor to end of current line
    MoveToBufferStart,    ///< Move cursor to start of buffer
    MoveToBufferEnd,      ///< Move cursor to end of buffer
    MoveUp,               ///< Move cursor up one line (or history prev in single-line)
    MoveDown,             ///< Move cursor down one line (or history next in single-line)
    SmartMoveToLineStart, ///< Move to line start; if already there, move to previous line's start
    SmartMoveToLineEnd,   ///< Move to line end; if already there, move to next line's end

    // === Editing ===
    DeleteCharBackward,    ///< Delete character before cursor (Backspace)
    DeleteCharForward,     ///< Delete character at cursor (Delete)
    DeleteWord,            ///< Delete word after cursor
    DeleteWordBackward,    ///< Delete word before cursor
    DeleteBigWordBackward, ///< Delete whitespace-delimited word before cursor (Fish Alt+Backspace bigword)
    KillToEnd,             ///< Kill (cut to kill ring) from cursor to end of line
    KillToStart,           ///< Kill from cursor to start of line
    Transpose,             ///< Transpose characters around cursor
    ClearBuffer,           ///< Clear the entire input buffer

    // === Undo/Redo ===
    Undo, ///< Undo last edit
    Redo, ///< Redo last undone edit

    // === Kill Ring (Emacs-style) ===
    Yank,    ///< Yank (paste) from kill ring
    YankPop, ///< Replace last yank with previous kill ring entry

    // === Selection ===
    SelectAll, ///< Select all text in buffer

    // === Clipboard ===
    Cut,   ///< Cut selection to system clipboard
    Copy,  ///< Copy selection to system clipboard
    Paste, ///< Paste from system clipboard

    // === Control ===
    Submit,            ///< Submit input (Enter)
    Abort,             ///< Abort input (Ctrl+D on empty, or explicit abort)
    InsertNewline,     ///< Insert newline in multiline mode
    AgentMode,         ///< Enter agent/AI mode
    CycleAgentMode,    ///< Cycle between agent sub-modes (Shift+Tab)
    CycleThinkingMode, ///< Cycle through thinking/reasoning modes (Ctrl+/)
    CycleModel,        ///< Cycle through available models for the active provider (Ctrl+.)

    // === History ===
    HistoryPrev, ///< Navigate to previous history entry
    HistoryNext, ///< Navigate to next history entry

    // === Command Palette ===
    CommandPalette, ///< Open the command palette (Ctrl+Shift+P)

    // === Fuzzy File Finder ===
    FuzzyFileFinder, ///< Open the fuzzy file finder (Ctrl+G)

    // === Prompt Control ===
    NewPrompt, ///< Abandon current input and start a fresh prompt
};

} // namespace core::tui
