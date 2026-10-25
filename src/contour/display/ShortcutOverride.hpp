// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuBarTable.hpp>

#include <core/Overloaded.hpp>

#include <QtCore/QKeyCombination>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <variant>

namespace contour::display
{

/// Who a key chord belongs to when Qt asks, ahead of delivering it, whether a shortcut may take it.
enum class ShortcutOverrideClaim : std::uint8_t
{
    Terminal = 0, ///< The terminal keeps the chord: no shortcut or menu may take it.
    Menu,         ///< A menu item owns the chord: it is left for the menu to take.
};

/// An item the platform puts into the application menu by itself, with no row of Contour's behind it.
enum class SystemMenuItem : std::uint8_t
{
    Hide = 0,   ///< "Hide Contour" (⌘H), which AppKit answers.
    HideOthers, ///< "Hide Others" (⌥⌘H), which AppKit answers.
};

/// Whose a menu-owned chord is: a Contour menu row's, named by its role, or an item the platform adds.
using MenuKeyOwner = std::variant<command::MenuItemRole, SystemMenuItem>;

/// A menu-owned chord: the platform's standard key where Qt has one, a fixed chord where it has none.
///
/// A fixed chord spells ⌘ as Qt::MetaModifier, as Qt names it under Qt::AA_MacDontSwapCtrlAndMeta, which
/// the app sets on macOS.
using MenuKey = std::variant<QKeySequence::StandardKey, QKeyCombination>;

/// One row of the menu-owned key table: a chord, and the menu item it is left to.
struct MenuOwnedKey
{
    MenuKeyOwner owner {}; ///< The menu item the chord belongs to.
    MenuKey key {};        ///< The chord.
};

/// The one menu-owned key table: the only chords a menu may take from the terminal.
///
/// The terminal's claim reads its keys, and the menu bar gives a role row its key from it, so the two
/// cannot drift. It lives in display/ rather than beside the menu bar in window/, because display/ may not
/// include window/. About and Zoom have no key. A row owned by a SystemMenuItem has no Contour row to
/// give a key to: the table only leaves the platform's own item its chord.
inline constexpr std::array MenuOwnedKeys {
    MenuOwnedKey { .owner = command::MenuItemRole::Preferences, .key = QKeySequence::Preferences },
    MenuOwnedKey { .owner = command::MenuItemRole::Quit, .key = QKeySequence::Quit },
    MenuOwnedKey { .owner = command::MenuItemRole::Minimize,
                   .key = QKeyCombination(Qt::MetaModifier, Qt::Key_M) },
    MenuOwnedKey { .owner = SystemMenuItem::Hide, .key = QKeyCombination(Qt::MetaModifier, Qt::Key_H) },
    MenuOwnedKey { .owner = SystemMenuItem::HideOthers,
                   .key = QKeyCombination(Qt::MetaModifier | Qt::AltModifier, Qt::Key_H) },
};

/// The key a menu row of @p role answers to.
/// @param role The menu row's role.
/// @return Its key from MenuOwnedKeys, or @c std::nullopt for a role that has none.
[[nodiscard]] inline std::optional<QKeySequence> menuKeyFor(command::MenuItemRole role)
{
    auto const row = std::ranges::find(MenuOwnedKeys, MenuKeyOwner { role }, &MenuOwnedKey::owner);
    if (row == MenuOwnedKeys.end())
        return std::nullopt;
    return std::visit([](auto const key) { return QKeySequence(key); }, row->key);
}

/// Whether @p event is the chord @p key.
/// @param event The key event.
/// @param key The chord.
/// @return True when the event's key and modifiers are the chord's, the keypad and group-switch modifiers
///         aside, as QKeyEvent::matches() sets them aside.
[[nodiscard]] inline bool matchesChord(QKeyEvent const& event, MenuKey const& key)
{
    return std::visit(core::Overloaded {
                          [&](QKeySequence::StandardKey const standard) { return event.matches(standard); },
                          [&](QKeyCombination const fixed) {
                              auto const modifiers =
                                  event.modifiers() & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
                              return QKeyCombination(modifiers, static_cast<Qt::Key>(event.key())) == fixed;
                          },
                      },
                      key);
}

/// Who a ShortcutOverride key event belongs to.
///
/// Only a global menu bar may take a chord, and only one of MenuOwnedKeys; every other chord stays with
/// the terminal, even when a menu item lists it, and whether a menu is enabled never enters into it.
/// @param event The key event Qt asks about.
/// @param placement Where this host's menu bar lives.
/// @return @c Menu for a menu-owned chord under a global menu bar, @c Terminal otherwise.
[[nodiscard]] inline ShortcutOverrideClaim shortcutOverrideClaim(QKeyEvent const& event,
                                                                 command::MenuBarPlacement placement)
{
    if (placement != command::MenuBarPlacement::Global)
        return ShortcutOverrideClaim::Terminal;
    auto const menuOwned = std::ranges::any_of(
        MenuOwnedKeys, [&](MenuOwnedKey const& row) { return matchesChord(event, row.key); });
    return menuOwned ? ShortcutOverrideClaim::Menu : ShortcutOverrideClaim::Terminal;
}

} // namespace contour::display
