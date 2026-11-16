// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/EditAction.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// A key chord represents a single key press with modifiers.
///
/// This is used to define keybindings. A KeyChord can match against
/// a KeyEvent to determine if the binding should trigger.
struct KeyChord
{
    KeyCode key {};                      ///< The key code (for special keys like Enter, arrows)
    Modifier modifiers = Modifier::None; ///< Modifier keys (Ctrl, Alt, Shift)
    char32_t codepoint = 0;              ///< Unicode codepoint (for letter keys)

    /// Creates a KeyChord from a special key with optional modifiers.
    [[nodiscard]] static constexpr KeyChord fromKey(KeyCode k, Modifier mod = Modifier::None)
    {
        return KeyChord { .key = k, .modifiers = mod, .codepoint = 0 };
    }

    /// Creates a KeyChord from a character with optional modifiers.
    [[nodiscard]] static constexpr KeyChord fromChar(char c, Modifier mod = Modifier::None)
    {
        return KeyChord { .key = {}, .modifiers = mod, .codepoint = static_cast<char32_t>(c) };
    }

    /// Parses a key chord from a string like "ctrl+y", "alt+shift+enter".
    ///
    /// Supported formats:
    /// - Modifiers: ctrl, alt, shift (case-insensitive)
    /// - Letter keys: a-z (case-insensitive, stored lowercase)
    /// - Special keys: enter, backspace, delete, tab, escape, up, down, left, right,
    ///                 home, end, pageup, pagedown, insert, f1-f12
    /// - Combined: ctrl+y, ctrl+shift+z, alt+enter
    ///
    /// @param str The string to parse.
    /// @return The parsed KeyChord, or std::nullopt if parsing fails.
    [[nodiscard]] static std::optional<KeyChord> parse(std::string_view str);

    /// Converts this KeyChord to a human-readable string.
    ///
    /// @return A string like "ctrl+y" or "alt+shift+enter".
    [[nodiscard]] std::string toString() const;

    /// Checks if this chord matches the given key event.
    [[nodiscard]] bool matches(KeyEvent const& event) const noexcept;

    /// Equality comparison for KeyChord.
    [[nodiscard]] bool operator==(KeyChord const& other) const noexcept = default;
};

/// Parses an EditAction from its string name.
///
/// Action names are case-insensitive and use the following conventions:
/// - Movement: move-forward-char, move-backward-word, move-to-line-start, etc.
/// - Editing: delete-char-backward, delete-word, kill-to-end, transpose
/// - Undo/Redo: undo, redo
/// - Kill Ring: yank, yank-pop
/// - Selection: select-all
/// - Clipboard: cut, copy, paste
/// - Control: submit, abort, insert-newline
/// - History: history-prev, history-next
///
/// @param str The action name to parse.
/// @return The parsed EditAction, or std::nullopt if unknown.
[[nodiscard]] std::optional<EditAction> parseEditAction(std::string_view str);

/// Converts an EditAction to its string name.
///
/// @param action The action to convert.
/// @return The action name (e.g., "redo", "move-forward-char").
[[nodiscard]] std::string_view editActionToString(EditAction action) noexcept;

/// Info about a bindable edit action (name + short description).
struct EditActionInfo
{
    std::string_view name;
    std::string_view description;
};

/// Returns all bindable edit actions with their descriptions.
[[nodiscard]] std::vector<EditActionInfo> allEditActionNames();

/// Returns all recognized key names (enter, escape, tab, up, f1, etc.).
[[nodiscard]] std::vector<std::string_view> allKeyNames();

/// Configurable keybinding system for InputField.
///
/// KeyBindings maps KeyChords to EditActions. Multiple keys can be bound
/// to the same action (e.g., both Ctrl+Y and Ctrl+Shift+Z can trigger Redo).
///
/// Usage:
/// @code
/// KeyBindings bindings = KeyBindings::defaults();
///
/// // Customize bindings
/// bindings.bind(KeyChord::fromChar('y', Modifier::Ctrl), EditAction::Yank);
///
/// // Look up action for a key event
/// if (auto action = bindings.lookup(event); action && *action != EditAction::None)
///     executeAction(*action);
/// @endcode
///
/// The design supports future editing modes (vi, emacs) by allowing
/// complete replacement of the binding set.
class KeyBindings
{
  public:
    /// Creates an empty keybinding set.
    KeyBindings() = default;

    /// Binds a key chord to an action.
    /// If the chord is already bound, the binding is replaced.
    void bind(KeyChord chord, EditAction action);

    /// Binds multiple key chords to the same action.
    void bindMultiple(std::initializer_list<KeyChord> chords, EditAction action);

    /// Removes a binding for the given chord.
    void unbind(KeyChord chord);

    /// Looks up the action for a key event.
    /// @return The bound action, or std::nullopt if no binding matches.
    [[nodiscard]] std::optional<EditAction> lookup(KeyEvent const& event) const noexcept;

    /// Returns all bindings for iteration.
    [[nodiscard]] std::span<std::pair<KeyChord, EditAction> const> bindings() const noexcept;

    /// Creates the default keybinding set.
    ///
    /// Default bindings:
    /// - Ctrl+Z: Undo
    /// - Ctrl+Y, Ctrl+Shift+Z: Redo
    /// - Ctrl+C: Copy
    /// - Ctrl+X: Cut
    /// - Ctrl+V: Paste
    /// - Ctrl+A: Select All
    /// - Ctrl+D: Abort (on empty) / Delete char
    /// - And more... (see implementation)
    [[nodiscard]] static KeyBindings defaults();

  private:
    std::vector<std::pair<KeyChord, EditAction>> _bindings;
};

} // namespace core::tui
