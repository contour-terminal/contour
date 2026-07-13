// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/Config.h>

#include <vtbackend/InputGenerator.h>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace contour
{

/// What a key binding is bound TO: a named key (F3, Enter, arrows) or a character ('P', '\\').
///
/// Mirrors the two halves of config::InputMappings — KeyInputMapping keys on vtbackend::Key,
/// CharInputMapping on char32_t — so one renderer can serve both without either caller unwrapping
/// its own binding first.
using ShortcutInput = std::variant<vtbackend::Key, char32_t>;

/// One chord modifier and the name a UI shows it under.
///
/// Separate from vtbackend::ChordModifierTable on purpose: that table is the wire/config vocabulary
/// ("Control"), this one is the user-facing vocabulary ("Ctrl"), and the ORDER of the rows here is
/// the order they are rendered in — the conventional Ctrl+Alt+Shift+Key, not the bit order that
/// std::formatter<Modifiers> would give ("Shift|Control").
struct ShortcutModifierRow
{
    vtbackend::Modifier modifier; ///< The modifier bit.
    std::string_view name;        ///< Its display name.
};

/// The chord modifiers in display order, each with its display name.
///
/// Adding a modifier to a shortcut's rendering is adding a row here.
constexpr inline auto ShortcutModifierTable = std::array {
    ShortcutModifierRow { vtbackend::Modifier::Control, "Ctrl" },
    ShortcutModifierRow { vtbackend::Modifier::Alt, "Alt" },
    ShortcutModifierRow { vtbackend::Modifier::Shift, "Shift" },
    ShortcutModifierRow { vtbackend::Modifier::Super, "Super" },
    ShortcutModifierRow { vtbackend::Modifier::Hyper, "Hyper" },
    ShortcutModifierRow { vtbackend::Modifier::Meta, "Meta" },
};

static_assert(ShortcutModifierTable.size() == vtbackend::ChordModifierTable.size(),
              "every chord modifier must have a display name here: a modifier added to vtbackend's "
              "table but not to this one would silently stop rendering in the shortcut column");

/// Renders a key chord the way a UI shows it, e.g. "Ctrl+Shift+P" or "Alt+Enter".
///
/// @param modifiers The chord modifiers (locks are not part of a binding and must not be passed).
/// @param input     The key or character the chord ends on.
/// @return The rendered shortcut; just the key's own name when @p modifiers is empty ("F3").
[[nodiscard]] std::string shortcutText(vtbackend::Modifiers modifiers, ShortcutInput input);

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
/// @return Command id (see commandId()) -> rendered shortcut.
[[nodiscard]] std::unordered_map<std::string, std::string> shortcutIndex(
    config::InputMappings const& mappings);

} // namespace contour
