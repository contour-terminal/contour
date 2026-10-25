// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuBarTable.hpp>

#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace contour::display
{

/// Who a key chord belongs to when Qt asks, ahead of delivering it, whether a shortcut may take it.
enum class ShortcutOverrideClaim : std::uint8_t
{
    Terminal = 0, ///< The terminal keeps the chord: no shortcut or menu may take it.
    Menu,         ///< A menu item owns the chord: it is left for the menu to take.
};

/// One row of the role-to-key table: the standard key a native menu row of @c role answers to.
struct MenuOwnedKey
{
    command::MenuItemRole role = command::MenuItemRole::None; ///< The menu row's role.
    QKeySequence::StandardKey key = QKeySequence::UnknownKey; ///< The platform's standard key for it.
};

/// The one role-to-key table: the only chords a menu may take from the terminal.
///
/// The terminal's claim reads its keys, and the menu bar gives a role row its key from it, so the two
/// cannot drift. It lives in display/ rather than beside the menu bar in window/, because display/ may not
/// include window/. About has no key.
inline constexpr std::array MenuOwnedKeys {
    MenuOwnedKey { .role = command::MenuItemRole::Preferences, .key = QKeySequence::Preferences },
    MenuOwnedKey { .role = command::MenuItemRole::Quit, .key = QKeySequence::Quit },
};

/// The standard key a menu row of @p role answers to.
/// @param role The menu row's role.
/// @return Its key from MenuOwnedKeys, or @c std::nullopt for a role that has none.
[[nodiscard]] inline std::optional<QKeySequence::StandardKey> standardKeyFor(
    command::MenuItemRole role) noexcept
{
    auto const row = std::ranges::find(MenuOwnedKeys, role, &MenuOwnedKey::role);
    if (row == MenuOwnedKeys.end())
        return std::nullopt;
    return row->key;
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
    auto const menuOwned =
        std::ranges::any_of(MenuOwnedKeys, [&](MenuOwnedKey const& row) { return event.matches(row.key); });
    return menuOwned ? ShortcutOverrideClaim::Menu : ShortcutOverrideClaim::Terminal;
}

} // namespace contour::display
