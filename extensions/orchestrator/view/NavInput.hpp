// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/NavKeys.hpp>
#include <orchestrator/view/Keys.hpp>

namespace orchestrator::view
{

/// Whether a box's text field has the keys.
enum class FieldMode : uint8_t
{
    Navigating = 0, ///< The keys move between the box's settings: letters may act.
    Typing,         ///< The text field has the keys: letters type.
};

/// Why a key resolved to no action.
enum class NavMiss : uint8_t
{
    NotBound = 0, ///< No navigation key it is, in the context it was pressed in: the surface decides.
    Pending,      ///< It starts a sequence: it is held until the next key, and nothing acts.
};

/// @return The name a binding gives @p key: a printable character, `^<letter>`, or a SpecialKeyNames name;
///         empty for a key with Alt, or one no name is given.
[[nodiscard]] std::string keyNameFor(core::tui::KeyEvent const& key);

/// @param name A key spelling, as `keys` writes it.
/// @return How a hint shows it: a glyph for a special key that has one (`↑`, `⏎`), else the spelling.
[[nodiscard]] std::string_view shownKeyName(std::string_view name) noexcept;

/// @param engine The engine.
/// @return The keys in force: the configuration's, else NavKeys::defaults(VimKeys::On) (one static
///         instance).
[[nodiscard]] NavKeys const& keysInForce(Engine const& engine) noexcept;

/// @param engine The engine; nullptr: none, as a view test without one has.
/// @return keysInForce(*engine), or the defaults without an engine.
[[nodiscard]] NavKeys const& keysInForce(Engine const* engine) noexcept;

/// Resolves a key for the surface that has the keys (Decision 7). A pending first key and @p key complete a
/// sequence, or the pending key is dropped and @p key is resolved alone; a key that starts a sequence becomes
/// the pending one (NavMiss::Pending: the key is taken, and nothing acts).
///
/// A task's configured key (`tasks[].key`) is no navigation key's concern and may equal one: it starts
/// nothing from a list (an item's agent starts from the Work view), so a navigation key always wins there.
/// @param key A key the terminal sent.
/// @param context Where it was pressed: navigating, or typing a text field.
/// @param keys The keys in force.
/// @param pending Interaction::pendingKey; set or cleared.
/// @return The action @p key does, or why it does none.
[[nodiscard]] std::expected<NavAction, NavMiss> navActionOf(core::tui::KeyEvent const& key,
                                                            NavContext context,
                                                            NavKeys const& keys,
                                                            std::string& pending);

/// Resolves a key a text field did not take, as if nothing were typed: only a special key (an arrow, Home,
/// End, PgUp, PgDn, ...) without Ctrl or Alt, so that the arrows keep moving while a filter or the message
/// box is typed. A letter or a chord, `^d` or a configured one, never acts as navigation while typing.
/// @param key A key the text field did not take.
/// @param keys The keys in force.
/// @return The action it does with nothing typed; std::nullopt: none, or it is no plain special key.
[[nodiscard]] std::optional<NavAction> passedKeyAction(core::tui::KeyEvent const& key, NavKeys const& keys);

/// @return The context of a list surface: Typing while a filter is typed or the message box has the
///         keys; else Navigating. The palette is never resolved.
/// @param interaction Where the user is.
/// @param context The selected row and the engine: whether the message box has the keys.
[[nodiscard]] NavContext navContextOf(Interaction const& interaction, KeyContext const& context) noexcept;

/// @param action A navigation action.
/// @param view The view whose list has the keys.
/// @return The command @p action is in @p view's list (Decision 9's first and third columns);
///         std::nullopt for LeaveField, EnterField and Close, which no list command stands for.
[[nodiscard]] std::optional<KeyCommand> listCommandOf(NavAction action, View view) noexcept;

/// @param keys The keys in force.
/// @param action A navigation action.
/// @return How a hint names @p action's first key (`↑`, `j`, `^d`); for EnterField, its first key other
///         than `enter`; empty: no such key.
[[nodiscard]] std::string navKeyLabel(NavKeys const& keys, NavAction action);

} // namespace orchestrator::view
