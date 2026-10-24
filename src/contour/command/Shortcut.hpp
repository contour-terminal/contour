// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace contour::command
{

/// What a key binding is bound TO: a named key (F3, Enter, arrows) or a character ('P', '\\').
///
/// Mirrors the two halves of InputMappings — KeyInputMapping keys on vtbackend::Key,
/// CharInputMapping on char32_t — so one renderer can serve both without either caller unwrapping
/// its own binding first.
using ShortcutInput = std::variant<vtbackend::Key, char32_t>;

/// How a chord is spelled for the user.
enum class ShortcutStyle : uint8_t
{
    Text = 0,  ///< Spelled-out names joined by plus signs: "Ctrl+Shift+P".
    MacGlyphs, ///< Apple's modifier and key glyphs, unseparated: "⇧⌘P".
};

/// The style the host platform's users expect: glyphs on macOS, spelled-out text elsewhere.
/// @return The platform's shortcut style.
[[nodiscard]] constexpr ShortcutStyle platformShortcutStyle() noexcept
{
#if defined(__APPLE__)
    return ShortcutStyle::MacGlyphs;
#else
    return ShortcutStyle::Text;
#endif
}

/// One chord modifier and the name a UI shows it under.
///
/// Separate from vtbackend::ChordModifierTable on purpose: that table is the wire vocabulary and the
/// canonical config spelling ("Control"), this one is the user-facing vocabulary ("Ctrl"), and the
/// ORDER of the rows here is the order they are rendered in — the conventional Ctrl+Alt+Shift+Key,
/// not the bit order that std::formatter<Modifiers> would give ("Shift|Control").
///
/// The set contour.yml actually ACCEPTS is a third table, contour::ConfigModifierTable, which is
/// wider than either of these because it carries synonyms — including the "Ctrl" spelling this table
/// teaches the user. Keeping them in sync is what issue #1987 was about.
struct ShortcutModifierRow
{
    vtbackend::Modifier modifier; ///< The modifier bit.
    std::string_view name;        ///< Its display name.
    std::string_view glyph;       ///< Its ShortcutStyle::MacGlyphs symbol; empty when it has none.
};

/// The chord modifiers in display order, each with its display name.
///
/// Adding a modifier to a shortcut's rendering is adding a row here.
constexpr inline auto ShortcutModifierTable = std::array {
    ShortcutModifierRow { vtbackend::Modifier::Control, "Ctrl", "⌃" },
    ShortcutModifierRow { vtbackend::Modifier::Alt, "Alt", "⌥" },
    ShortcutModifierRow { vtbackend::Modifier::Shift, "Shift", "⇧" },
    ShortcutModifierRow { vtbackend::Modifier::Super, "Super", "⌘" },
    ShortcutModifierRow { vtbackend::Modifier::Hyper, "Hyper", "" },
    ShortcutModifierRow { vtbackend::Modifier::Meta, "Meta", "" },
};

static_assert(ShortcutModifierTable.size() == vtbackend::ChordModifierTable.size(),
              "every chord modifier must have a display name here: a modifier added to vtbackend's "
              "table but not to this one would silently stop rendering in the shortcut column");

static_assert(std::ranges::all_of(ShortcutModifierTable,
                                  [](auto const& row) {
                                      return row.modifier == config::ModifierWithoutConfigSpelling
                                             || config::parseModifierName(row.name) == row.modifier;
                                  }),
              "every name shown in the shortcut column must be a spelling contour.yml ACCEPTS: this "
              "table is what teaches a user how to write a chord, so a display name the config "
              "parser refuses sends them to a binding that silently never fires -- which is exactly "
              "what issue #1987 was. Add the spelling to contour::config::ConfigModifierTable as an alias.");

/// One named key and the symbol ShortcutStyle::MacGlyphs shows it as.
struct ShortcutKeyGlyphRow
{
    vtbackend::Key key;     ///< The key.
    std::string_view glyph; ///< Its symbol.
};

/// The keys that have a ShortcutStyle::MacGlyphs symbol; every other key keeps its name.
constexpr inline auto ShortcutKeyGlyphTable = std::array {
    ShortcutKeyGlyphRow { vtbackend::Key::Enter, "↩" },
    ShortcutKeyGlyphRow { vtbackend::Key::Escape, "⎋" },
    ShortcutKeyGlyphRow { vtbackend::Key::Tab, "⇥" },
    ShortcutKeyGlyphRow { vtbackend::Key::Backspace, "⌫" },
    ShortcutKeyGlyphRow { vtbackend::Key::Delete, "⌦" },
    ShortcutKeyGlyphRow { vtbackend::Key::LeftArrow, "←" },
    ShortcutKeyGlyphRow { vtbackend::Key::UpArrow, "↑" },
    ShortcutKeyGlyphRow { vtbackend::Key::RightArrow, "→" },
    ShortcutKeyGlyphRow { vtbackend::Key::DownArrow, "↓" },
    ShortcutKeyGlyphRow { vtbackend::Key::PageUp, "⇞" },
    ShortcutKeyGlyphRow { vtbackend::Key::PageDown, "⇟" },
    ShortcutKeyGlyphRow { vtbackend::Key::Home, "↖" },
    ShortcutKeyGlyphRow { vtbackend::Key::End, "↘" },
};

/// Renders a chord's modifiers the way a UI shows them: "Ctrl+Shift+" as text, "⌃⇧" as glyphs.
/// A modifier without a glyph keeps its "Name+" spelling in either style.
/// @param modifiers The chord modifiers (locks are not part of a binding and must not be passed).
/// @param style     How to spell them.
/// @return The rendered modifiers; empty when @p modifiers is.
[[nodiscard]] std::string modifiersText(vtbackend::Modifiers modifiers, ShortcutStyle style);

/// Renders a key chord the way a UI shows it, e.g. "Ctrl+Shift+P" or "Alt+Enter".
///
/// @param modifiers The chord modifiers (locks are not part of a binding and must not be passed).
/// @param input     The key or character the chord ends on.
/// @param style     How to spell it.
/// @return The rendered shortcut; just the key's own name when @p modifiers is empty ("F3").
[[nodiscard]] std::string shortcutText(vtbackend::Modifiers modifiers,
                                       ShortcutInput input,
                                       ShortcutStyle style);

/// Indexes every key/char binding in @p mappings by the id of the command it runs, so a view can ask
/// "what is the shortcut for this command?" in one lookup rather than re-scanning the bindings.
///
/// Mouse bindings are deliberately absent: a command palette teaches KEYBOARD shortcuts, and showing
/// "Ctrl+Left-click" in the shortcut column would not tell the user anything they can type.
///
/// A binding runs a LIST of actions; only single-action bindings are indexed, because a chord that
/// fires three actions is not the shortcut for any one of them. When several bindings run the same
/// command, the first wins (config order), so the rendering is stable.
///
/// @param mappings The configured input mappings.
/// @param style    How to spell the chords.
/// @return Command id (see commandId()) -> rendered shortcut.
[[nodiscard]] std::unordered_map<std::string, std::string> shortcutIndex(
    config::InputMappings const& mappings, ShortcutStyle style);

/// @copydoc shortcutIndex(config::InputMappings const&)
///
/// Indexes the built-in fallback rows @p config enables after the user's own bindings, so a built-in
/// chord such as Ctrl+Shift+G is advertised too. A fallback row whose chord the user's table binds is
/// left out: the user's binding is found first, so that chord never reaches the fallback.
/// @param config The configuration: its input mappings and the options that enable fallback rows.
/// @param style  How to spell the chords.
[[nodiscard]] std::unordered_map<std::string, std::string> shortcutIndex(config::Config const& config,
                                                                         ShortcutStyle style);

/// How links are opened, for the settings page: "Ctrl+Click (Shift+Ctrl+Click inside applications that use
/// the mouse)". Spelled as the shortcut column spells a chord.
/// @param config The loaded configuration: its FollowHyperlink mouse binding and bypass modifiers.
/// @param style  How to spell the modifiers.
/// @return The gesture, spelled for a user.
[[nodiscard]] std::string describeLinkGesture(config::Config const& config, ShortcutStyle style);

} // namespace contour::command
