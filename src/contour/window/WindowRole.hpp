// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a window is for. Qt-free: the roles are a table, so a new kind of window is a new row.

#include <contour/config/ChromeDensity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace contour::window
{

/// What a window is for.
enum class WindowRole : uint8_t
{
    Normal = 0,    ///< A terminal window like any other.
    QuickTerminal, ///< The hotkey's terminal, sliding down from the top of the screen.
};

/// The number of WindowRole enumerators, derived from the last one.
inline constexpr size_t WindowRoleCount = std::to_underlying(WindowRole::QuickTerminal) + 1;

/// How a window sits among the others.
enum class WindowLevel : uint8_t
{
    Normal = 0, ///< An ordinary top-level window.
    Panel,      ///< A tool window that does not take the application's activation with it.
};

/// Whether a window's chrome density is the configured one or forced.
enum class ChromeDensityOverride : uint8_t
{
    Configured = 0, ///< The `chrome_density` setting.
    Minimal,        ///< Minimal whatever the setting says.
};

/// Whether a window's position outlives it.
enum class PositionMemory : uint8_t
{
    Remembered = 0, ///< Saved when it closes, restored for the first window.
    Placed,         ///< Always placed by whoever opens it, never saved or restored.
};

/// What a window role means.
struct WindowRoleRow
{
    WindowRole role {};               ///< The role.
    WindowLevel level {};             ///< How the window sits.
    ChromeDensityOverride density {}; ///< Which chrome density the window takes.
    PositionMemory memory {};         ///< Whether its position is remembered.
};

/// One row per role, in enumerator order.
inline constexpr auto WindowRoleRows = std::array {
    WindowRoleRow { .role = WindowRole::Normal,
                    .level = WindowLevel::Normal,
                    .density = ChromeDensityOverride::Configured,
                    .memory = PositionMemory::Remembered },
    WindowRoleRow { .role = WindowRole::QuickTerminal,
                    .level = WindowLevel::Panel,
                    .density = ChromeDensityOverride::Minimal,
                    .memory = PositionMemory::Placed },
};

static_assert(WindowRoleRows.size() == WindowRoleCount, "WindowRoleRows must have a row for every role");

/// Finds a role's row.
/// @param role The role.
/// @return Its row.
[[nodiscard]] constexpr WindowRoleRow const& windowRoleRow(WindowRole role) noexcept
{
    return WindowRoleRows[std::to_underlying(role)];
}

/// The chrome density a window of @p role takes.
/// @param role       The window's role.
/// @param configured The `chrome_density` setting.
/// @return Minimal for a role that forces it, else @p configured.
[[nodiscard]] constexpr config::ChromeDensity chromeDensityFor(WindowRole role,
                                                               config::ChromeDensity configured) noexcept
{
    switch (windowRoleRow(role).density)
    {
        case ChromeDensityOverride::Configured: return configured;
        case ChromeDensityOverride::Minimal: return config::ChromeDensity::Minimal;
    }
    return configured;
}

} // namespace contour::window
