// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace contour::window
{

/// Whether a top-level window is on screen.
enum class WindowVisibility : uint8_t
{
    Hidden = 0, ///< Hidden or closed, possibly not yet destroyed.
    Shown,      ///< Visible.
};

/// Whether a window is a transient child of another (a dialog, a popup).
enum class WindowTransience : uint8_t
{
    Independent = 0, ///< A window of its own.
    Transient,       ///< Belongs to another window.
};

/// What the last-window rule needs to know about one top-level window.
struct WindowFacts
{
    WindowVisibility visibility = WindowVisibility::Hidden;      ///< Whether it is on screen.
    WindowTransience transience = WindowTransience::Independent; ///< Whether another window owns it.
};

/// Qt's quit-on-last-window-closed rule: only a visible window that is not transient keeps the
/// application alive; hidden or closed windows not yet destroyed, and dialogs, do not.
/// @param windows The application's top-level windows.
/// @return True if at least one of them keeps the application alive.
[[nodiscard]] constexpr bool anyWindowKeepsAppAlive(std::span<WindowFacts const> windows) noexcept
{
    return std::ranges::any_of(windows, [](WindowFacts const& window) {
        return window.visibility == WindowVisibility::Shown
               && window.transience == WindowTransience::Independent;
    });
}

} // namespace contour::window
