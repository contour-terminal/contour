// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>

#include <vtbackend/core/Color.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace contour::command
{

/// Whether the user gave a tab a colour of their own, as opposed to one an application assigned.
enum class UserColorPresence : std::uint8_t
{
    Absent = 0, ///< No colour from the user.
    Present,    ///< The user chose one; "Reset" has something to drop.
};

/// The world, as a tab's context menu needs to see it.
struct TabContextMenuState
{
    int tabIndex = 0; ///< The right-clicked tab's position in its window.
    int tabCount = 0; ///< Tabs in its window.
    /// The colours the "Tab Color" submenu offers, in order (`SessionModel::colorPalette()`).
    std::vector<vtbackend::RGBColor> palette {};
    /// The tab's effective colour, whoever assigned it.
    std::optional<vtbackend::RGBColor> color {};
    /// Whether the user gave the tab its colour.
    UserColorPresence userColor = UserColorPresence::Absent;
};

/// The tab's context menu for @p state.
/// @param state The tab the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildTabContextMenu(TabContextMenuState const& state);

/// The world, as the new-tab menu needs to see it.
struct NewTabMenuState
{
    std::vector<std::string> profileNames {}; ///< Every configured profile, in a stable order.
};

/// The new-tab menu for @p state: one row per profile, then "Settings".
/// @param state The configured profiles.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildNewTabMenu(NewTabMenuState const& state);

} // namespace contour::command
