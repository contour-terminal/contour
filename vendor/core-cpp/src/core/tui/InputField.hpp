// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/CompletionPopup.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyBindings.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/TextDecorator.hpp>
#include <core/tui/TuiCoreExport.hpp>
#include <core/tui/completer/CompletionProvider.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

class Canvas;

/// @brief Style configuration for InputField.
///
/// Allows parent components to customize InputField's appearance.
/// If not set, InputField uses theme defaults.
struct InputFieldStyles
{
    std::optional<Style> text;       ///< Normal text style
    std::optional<Style> ghost;      ///< Ghost text (dimmed suggestion) style
    std::optional<Style> selection;  ///< Selected text style
    std::optional<Style> background; ///< Background fill style
};

/// @brief Result of processing an input event in InputField.
enum class InputFieldAction : std::uint8_t
{
    Changed,           ///< Buffer content or cursor changed, re-render needed.
    Submit,            ///< User pressed Enter.
    Abort,             ///< User pressed Ctrl+C.
    Eof,               ///< User pressed Ctrl+D on an empty line.
    AgentMode,         ///< User triggered agent mode (Ctrl+T by default).
    CycleAgentMode,    ///< User toggled agent sub-mode (Shift+Tab by default).
    CycleThinkingMode, ///< User toggled thinking mode (Ctrl+/ by default).
    CycleModel,        ///< User cycled through models (Ctrl+. by default).
    CommandPalette,    ///< User triggered the command palette (Ctrl+Shift+P by default).
    FuzzyFileFinder,   ///< User triggered the fuzzy file finder (Ctrl+G by default).
    NewPrompt,         ///< User requested a new prompt without executing.
    None,              ///< Event not consumed by InputField.
};

/// @brief Text editor component with Emacs keybindings, history, and kill ring.
///
/// InputField is a TUI component that provides rich text editing capabilities.
/// It accepts InputEvent objects and updates internal state. The component
/// renders itself using the Canvas API. Operates on grapheme cluster boundaries
/// using libunicode for correct Unicode handling.
///
/// Supports both single-line and multiline modes. In multiline mode:
/// - Shift+Enter inserts a newline
/// - Up/Down navigate between lines (history when on first/last line)
/// - Home/End move to start/end of current line
/// - Ctrl+Home/Ctrl+End move to start/end of buffer
///
/// As a Component, InputField can be added to a Screen's component tree
/// and will receive events through the standard event dispatch mechanism.
class CORE_TUI_CORE_API InputField: public Component
{
  public:
    /// @brief Constructs an empty field timed by the process's steady clock.
    InputField();

    /// @brief Constructs an empty field whose multi-clicks are timed by @p clock.
    /// @param clock The clock double- and triple-clicks are measured on (not owned; must outlive
    ///        the field).
    explicit InputField(IClock const& clock CORE_LIFETIMEBOUND);

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    explicit InputField(IClock const&& clock) = delete;

    ~InputField() override = default;

    /// @brief Returns the clock multi-clicks are measured on.
    [[nodiscard]] IClock const& clock() const noexcept { return _clock; }

    // --- Component Interface ---

    /// @brief Renders the input field to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    /// @param event The input event.
    /// @return EventResult for event bubbling.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief InputField can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief InputField uses I-beam cursor when focused.
    [[nodiscard]] CursorShape cursorShape() const override { return CursorShape::SteadyBar; }

    /// @brief Returns preferred size based on content.
    [[nodiscard]] Size preferredSize() const override;

    // --- InputField-Specific Event Processing ---

    /// @brief Processes an input event and returns the resulting action.
    /// @param event The input event to process.
    /// @return The action resulting from the event.
    [[nodiscard]] auto processEvent(InputEvent const& event) -> InputFieldAction;

    /// @brief Returns the current buffer content.
    [[nodiscard]] auto text() const noexcept -> std::string_view;

    /// @brief Returns the cursor position as a byte offset into text().
    [[nodiscard]] auto cursor() const noexcept -> std::size_t;

    /// @brief Sets the prompt string.
    /// @param prompt The prompt to display before user input.
    void setPrompt(std::string_view prompt);

    /// @brief Returns the current prompt string.
    [[nodiscard]] auto prompt() const noexcept -> std::string_view;

    /// @brief Sets custom styles for the input field.
    ///
    /// Allows parent components to customize InputField's appearance.
    /// Any style set to std::nullopt will use theme defaults.
    /// @param styles The styles to apply.
    void setStyles(InputFieldStyles styles);

    /// @brief Returns the current custom styles.
    [[nodiscard]] InputFieldStyles const& styles() const noexcept { return _styles; }

    /// @brief Clears the buffer and resets cursor to position 0.
    void clear();

    /// @brief Sets the buffer content programmatically.
    /// @param text New buffer content.
    void setText(std::string_view text);

    /// @brief Sets the cursor position.
    /// @param pos The byte offset to move the cursor to (clamped to buffer size).
    void setCursor(size_t pos);

    /// @brief Adds an entry to the history ring.
    /// @param entry The line to add.
    void addHistory(std::string entry);

    /// @brief Sets the maximum number of history entries to retain.
    /// @param n Maximum history size.
    void setMaxHistory(std::size_t n);

    /// @brief Enables or disables masked (password) mode.
    ///
    /// When masked, each grapheme cluster is rendered as '*' and ghost text is suppressed.
    /// @param masked True to enable masked input.
    void setMasked(bool masked);

    /// @brief Returns whether masked mode is enabled.
    [[nodiscard]] auto isMasked() const noexcept -> bool;

    /// @brief Enables or disables multiline mode.
    /// @param enable True to enable multiline editing.
    void setMultiline(bool enable);

    /// @brief Returns whether multiline mode is enabled.
    [[nodiscard]] auto isMultiline() const noexcept -> bool;

    /// @brief Returns the number of lines in the current buffer.
    [[nodiscard]] auto lineCount() const noexcept -> int;

    /// @brief Returns the current cursor line (0-based).
    [[nodiscard]] auto cursorLine() const noexcept -> int;

    /// @brief Returns the current cursor column within the line (0-based, in graphemes).
    [[nodiscard]] auto cursorColumn() const noexcept -> int;

    /// @brief Returns the content of a specific line (0-based index).
    [[nodiscard]] auto lineAt(int lineIndex) const -> std::string_view;

    /// @brief Positions the cursor based on a click at the given line and column.
    ///
    /// Coordinates are relative to the input field content (not screen coordinates).
    /// The caller is responsible for translating screen coordinates to field-relative coordinates.
    /// @param line The line number (0-based).
    /// @param column The column within the line (0-based, in graphemes).
    /// @param extendSelection If true, extends selection from current anchor; otherwise clears selection.
    void setCursorFromClick(int line, int column, bool extendSelection = false);

    // ========================================================================
    // Mouse handling
    // ========================================================================

    /// @brief Handles a mouse event with field-relative coordinates.
    ///
    /// Implements click-to-position, drag selection, double-click word selection,
    /// and triple-click line selection.
    /// @param type The mouse event type (Press, Release, Move, ScrollUp, ScrollDown).
    /// @param line The line number (0-based).
    /// @param column The column within the line (0-based, in graphemes).
    /// @param mods Active modifier keys.
    /// @return The action resulting from the event.
    [[nodiscard]] auto handleMouse(MouseEvent::Type type, int line, int column, Modifier mods)
        -> InputFieldAction;

    /// @brief Selects the word at the given byte position (fish-style word boundaries).
    /// @param position Byte offset into the buffer.
    void selectWord(std::size_t position);

    /// @brief Selects the entire line at the given index.
    /// @param lineIndex The line number (0-based).
    void selectLine(int lineIndex);

    /// @brief Scrolls the view by the given number of lines (for multiline mode).
    ///
    /// Positive values scroll down, negative scroll up.
    /// @param lines Number of lines to scroll.
    void scrollBy(int lines);

    /// @brief Returns the current scroll offset (for multiline mode).
    [[nodiscard]] auto scrollOffset() const noexcept -> int;

    /// @brief Sets the scroll offset (for multiline mode).
    void setScrollOffset(int offset);

    /// @brief Returns the byte offset of the start of the given line.
    /// @param lineIndex The 0-based line index.
    /// @return Byte offset into the buffer where the line starts.
    [[nodiscard]] auto lineStartOffset(int lineIndex) const -> std::size_t;

    /// @brief Sets the maximum number of lines allowed in multiline mode (0 = unlimited).
    void setMaxLines(int maxLines);

    /// @brief Returns the maximum number of lines allowed.
    [[nodiscard]] auto maxLines() const noexcept -> int;

    // ========================================================================
    // Ghost text (fish-style autosuggestion)
    // ========================================================================

    /// @brief Sets the ghost text suggestion to display after the cursor.
    ///
    /// Ghost text appears dimmed after the cursor and can be accepted
    /// with Right arrow or End key. It's automatically cleared when
    /// the buffer is modified.
    /// @param ghost The suggestion text (suffix to append).
    void setGhostText(std::string_view ghost);

    /// @brief Clears the ghost text suggestion.
    void clearGhostText();

    /// @brief Accepts the ghost text, inserting it at the cursor.
    void acceptGhostText();

    /// @brief Returns the current ghost text.
    [[nodiscard]] auto ghostText() const noexcept -> std::string_view;

    /// @brief Returns whether there is ghost text to display.
    [[nodiscard]] auto hasGhostText() const noexcept -> bool;

    // ========================================================================
    // Clipboard callback
    // ========================================================================

    /// @brief Callback type for clipboard copy operations.
    using ClipboardCallback = std::function<void(std::string_view)>;

    /// @brief Sets a callback to be invoked when text is copied.
    ///
    /// This allows integration with system clipboard (e.g., OSC 52).
    /// The callback receives the text that was copied.
    /// @param callback The callback function, or nullptr to disable.
    void setClipboardCallback(ClipboardCallback callback);

    // ========================================================================
    // Selection support (GUI-style: Shift+arrows, not Emacs set-mark)
    // ========================================================================

    /// @brief Returns whether there is an active selection.
    [[nodiscard]] auto hasSelection() const noexcept -> bool;

    /// @brief Returns the start of the selection (lower byte offset).
    [[nodiscard]] auto selectionStart() const noexcept -> std::size_t;

    /// @brief Returns the end of the selection (higher byte offset).
    [[nodiscard]] auto selectionEnd() const noexcept -> std::size_t;

    /// @brief Returns the selected text.
    [[nodiscard]] auto selectedText() const -> std::string_view;

    /// @brief Clears the selection without deleting text.
    void clearSelection();

    /// @brief Selects all text in the buffer.
    void selectAll();

    // ========================================================================
    // Keybindings
    // ========================================================================

    /// @brief Sets custom keybindings for the input field.
    /// @param bindings The keybindings to use.
    void setKeyBindings(KeyBindings bindings);

    /// @brief Returns the current keybindings (const).
    [[nodiscard]] auto keyBindings() const noexcept -> KeyBindings const&;

    /// @brief Returns the current keybindings (mutable).
    [[nodiscard]] auto keyBindings() noexcept -> KeyBindings&;

    // ========================================================================
    // Text decorator (for syntax highlighting, error underlines, custom backgrounds)
    // ========================================================================

    /// @brief Sets a text decorator for custom rendering (syntax highlight, underlines, backgrounds).
    /// @param decorator Pointer to decorator (caller owns, must outlive InputField usage). Nullptr to clear.
    void setTextDecorator(TextDecorator const* decorator);

    /// @brief Sets the continuation prompt for non-first lines in multiline mode.
    /// @param prompt The continuation text (e.g., "·· " or "  ").
    void setContinuationPrompt(std::string_view prompt);

    /// @brief Returns the continuation prompt.
    [[nodiscard]] auto continuationPrompt() const noexcept -> std::string_view;

    /// @brief Copies the selected text to the clipboard (kill ring for now).
    /// @return True if text was copied.
    auto copySelection() -> bool;

    /// @brief Cuts the selected text to the clipboard.
    /// @return True if text was cut.
    auto cutSelection() -> bool;

    // ========================================================================
    // Undo/Redo support
    // ========================================================================

    /// @brief Undoes the last editing operation.
    /// @return True if an undo was performed.
    auto undo() -> bool;

    /// @brief Redoes the last undone operation.
    /// @return True if a redo was performed.
    auto redo() -> bool;

    /// @brief Returns whether undo is available.
    [[nodiscard]] auto canUndo() const noexcept -> bool;

    /// @brief Returns whether redo is available.
    [[nodiscard]] auto canRedo() const noexcept -> bool;

    /// @brief Clears the undo/redo history.
    void clearUndoHistory();

  private:
    std::string _buffer;
    std::size_t _cursor = 0;
    std::string _prompt;
    KeyBindings _keyBindings = KeyBindings::defaults();
    InputFieldStyles _styles;                      ///< Custom styles (nullopt values use theme defaults)
    std::string _ghostText;                        ///< Ghost text suggestion (displayed dimmed after cursor)
    std::string _ghostConsumed;                    ///< Ghost head chars eaten by matching keystrokes, so a
                                                   ///< backspace can restore the suggestion even after the
                                                   ///< last char trimmed the ghost to empty (anti-flicker).
    TextDecorator const* _textDecorator = nullptr; ///< Optional decorator for custom rendering.
    std::string _continuationPrompt;               ///< Continuation prompt for non-first lines.
    bool _masked = false;
    bool _multiline = false;
    int _maxLines = 0;      ///< 0 = unlimited
    int _scrollOffset = 0;  ///< Scroll offset for multiline mode
    int _hScrollOffset = 0; ///< Horizontal scroll offset (display columns) for single-line mode

    // Mouse state
    bool _dragging = false; ///< True during click-and-drag selection
    IClock const& _clock;   ///< The clock multi-clicks are timed on.
    SteadyTimePoint _lastClickTime;
    int _lastClickLine = -1;
    int _lastClickColumn = -1;
    int _clickCount = 0; ///< 1=single, 2=double, 3=triple

    static constexpr auto DoubleClickTimeout = std::chrono::milliseconds(400);
    static constexpr int DoubleClickTolerance = 2; ///< Max distance in cells for multi-click

    // Clipboard callback
    ClipboardCallback _clipboardCallback;

    // Selection state (GUI-style)
    std::optional<std::size_t>
        _selectionAnchor; ///< Anchor point where selection started (nullopt = no selection).

    // Undo/Redo state
    struct UndoState
    {
        std::string buffer;
        std::size_t cursor;
    };

    std::vector<UndoState> _undoStack;
    std::vector<UndoState> _redoStack;
    static constexpr std::size_t MaxUndoHistory = 100;

    // History
    std::vector<std::string> _history;
    std::size_t _historyIndex = 0;
    std::string _savedLine;
    std::size_t _maxHistory = 100;

    // Kill ring (Emacs-style)
    std::vector<std::string> _killRing;
    std::size_t _killRingIndex = 0;
    static constexpr std::size_t MaxKillRing = 16;
    bool _lastWasKill = false; ///< Whether the last action was a kill (for kill ring appending).

    /// @brief Dispatches a KeyEvent to the appropriate editing operation.
    [[nodiscard]] auto handleKey(KeyEvent const& key) -> InputFieldAction;

    /// @brief Executes an edit action.
    /// @param action The action to execute.
    /// @return The result of executing the action.
    [[nodiscard]] auto executeAction(EditAction action) -> InputFieldAction;

    // Editing operations
    void killToEnd();            ///< Ctrl+K: Kill from cursor to end of line.
    void killToStart();          ///< Ctrl+U: Kill from cursor to start of line.
    void killWord();             ///< Alt+D: Kill word forward.
    void killWordBackward();     ///< Ctrl+W / Ctrl+Backspace: Kill word backward.
    void killBigWordBackward();  ///< Alt+Backspace: Kill whitespace-delimited word backward.
    void yank();                 ///< Ctrl+Y: Yank (paste) from kill ring.
    void yankPop();              ///< Alt+Y: Cycle kill ring.
    void deleteChar();           ///< Delete / Ctrl+D: Delete character at cursor.
    void deleteCharBackward();   ///< Backspace: Delete character before cursor.
    void moveToStart();          ///< Ctrl+A / Home: Move cursor to start of line (or buffer).
    void moveToEnd();            ///< Ctrl+E / End: Move cursor to end of line (or buffer).
    void moveToBufferStart();    ///< Move cursor to start of buffer.
    void moveToBufferEnd();      ///< Move cursor to end of buffer.
    void moveToLineStart();      ///< Move cursor to start of current line.
    void moveToLineEnd();        ///< Move cursor to end of current line.
    void moveForwardChar();      ///< Ctrl+F / Right: Move cursor forward one grapheme.
    void moveBackwardChar();     ///< Ctrl+B / Left: Move cursor backward one grapheme.
    void moveForwardWord();      ///< Alt+F / Ctrl+Right: Move cursor forward one word.
    void moveBackwardWord();     ///< Alt+B / Ctrl+Left: Move cursor backward one word.
    void moveUp();               ///< Move cursor up one line (multiline mode).
    void moveDown();             ///< Move cursor down one line (multiline mode).
    void smartMoveToLineStart(); ///< Move to line start; if already there in multiline, move to previous
                                 ///< line's start.
    void smartMoveToLineEnd(); ///< Move to line end; if already there in multiline, move to next line's end.
    void historyPrev();        ///< Up / Ctrl+P: Previous history entry.
    void historyNext();        ///< Down / Ctrl+N: Next history entry.
    void transpose();          ///< Ctrl+T: Transpose characters before cursor.
    void insertNewline();      ///< Insert a newline at cursor (multiline mode).

    /// @brief Pushes text onto the kill ring.
    void pushKillRing(std::string text);

    /// @brief Inserts a UTF-8 encoded codepoint at the cursor.
    void insertCodepoint(char32_t cp);

    /// @brief Inserts a UTF-8 string at the cursor.
    void insertText(std::string_view text);

    // Line position helpers
    [[nodiscard]] auto findLineStart(std::size_t pos) const -> std::size_t;
    [[nodiscard]] auto findLineEnd(std::size_t pos) const -> std::size_t;
    [[nodiscard]] auto countGraphemesInRange(std::size_t start, std::size_t end) const -> int;
    [[nodiscard]] auto moveToGraphemeInLine(std::size_t lineStart, int graphemeIndex) const -> std::size_t;

    // Unicode helpers (using libunicode)
    [[nodiscard]] auto nextGraphemeCluster(std::size_t pos) const -> std::size_t;
    [[nodiscard]] auto prevGraphemeCluster(std::size_t pos) const -> std::size_t;

    // Word boundary helpers (fish-style: path separators break words). These classify a whole
    // Unicode codepoint (char32_t), not a single UTF-8 byte, so multibyte separators (e.g. the
    // ideographic space U+3000) are recognized correctly.
    [[nodiscard]] static auto isWordChar(char32_t c) -> bool;
    [[nodiscard]] static auto isBigWordChar(char32_t c) -> bool;
    [[nodiscard]] auto findWordStart(std::size_t pos) const -> std::size_t;
    [[nodiscard]] auto findWordEnd(std::size_t pos) const -> std::size_t;

    /// Kill backward from the cursor to the start of the previous word, using the given
    /// boundary predicate to decide which codepoints count as part of a word. Handles ghost
    /// text, the kill ring, and undo state. Backs both small-word and bigword backward kills.
    /// @param isWordCharFn Returns true for codepoints that belong to a word (non-boundary).
    void killWordBackwardWith(bool (*isWordCharFn)(char32_t));

    // Mouse helpers
    [[nodiscard]] auto detectClickCount(int line, int column) -> int;

    // Selection helpers
    void deleteSelection();        ///< Deletes selected text if any.
    void startOrExtendSelection(); ///< Starts selection at cursor if none, keeps anchor otherwise.
    void moveWithSelection(void (InputField::*move)()); ///< Executes move while extending selection.

    // Undo/Redo helpers
    void saveUndoState(); ///< Saves current state to undo stack before an editing operation.

    /// @brief Renders ghost text character-by-character with per-column decorator background.
    void renderGhostText(Canvas& canvas, int row, int& col, Style const& ghostStyle) const;

    /// @brief Re-prepends a just-deleted run onto the ghost text so a backward deletion
    ///        at the buffer end keeps the inline suggestion stable (no flicker).
    ///
    /// Symmetric inverse of the type-at-end trim: deleting the last character(s) of the
    /// buffer makes the deleted run the new prefix of the suggestion (e.g. buffer "git c"
    /// + ghost "heckout" after backspacing the 'h' from "git ch"). For prefix-consistent
    /// completers this yields the exact correct suggestion; the consumer's debounced
    /// recompute confirms or corrects it afterwards.
    ///
    /// @param deleted The grapheme run removed from the end of the buffer. No-op if the
    ///        ghost text is currently empty or @p deleted is empty. The run is truncated at
    ///        the first newline (the ghost is single-line, matching setGhostText).
    void prependGhostText(std::string_view deleted);

    /// @brief Adjusts ghost text after a backward deletion (Backspace / Ctrl-W / Ctrl-U).
    ///
    /// Re-prepends the just-deleted run onto the suggestion when the deletion happened at the
    /// buffer end @e and text remains, keeping the inline suggestion stable across the
    /// consumer's debounced recompute. Otherwise clears the ghost: a mid-buffer deletion is
    /// not rendered, and a deletion that empties the buffer must not resurrect the killed line.
    ///
    /// @param deletedRun The grapheme run removed from the buffer.
    /// @param wasAtEnd   Whether the cursor was at the buffer end before the deletion.
    void adjustGhostAfterBackwardDelete(std::string_view deletedRun, bool wasAtEnd);
};

} // namespace core::tui
