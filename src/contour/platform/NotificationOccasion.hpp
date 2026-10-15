// SPDX-License-Identifier: Apache-2.0
//
// When a notice is worth raising, by where its session is: the one rule a finished command's notice and an
// application's OSC 777, OSC 9 and OSC 99 notifications are all decided by. Header-only and constexpr, so it
// is table-tested in contour_test; the session asks the manager where it is.
#pragma once

#include <contour/config/FinishNotificationConfig.hpp>
#include <contour/platform/SessionVisibility.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

namespace contour::platform
{

/// Whether @p when asks for a notice from a session that is @p visibility.
/// @param when       The profile's `notify_on_command_finish.when`, or an application notification's
///                   occasion as notifyWhenOf() states it.
/// @param visibility Where the session is right now.
/// @return True when a notice is wanted.
[[nodiscard]] constexpr bool isNoticeWanted(config::NotifyWhen when, SessionVisibility visibility) noexcept
{
    switch (when)
    {
        case config::NotifyWhen::Never: return false;
        case config::NotifyWhen::Unfocused: return visibility != SessionVisibility::Focused;
        case config::NotifyWhen::Hidden: return visibility == SessionVisibility::Hidden;
        case config::NotifyWhen::Always: return true;
    }
    return false;
}

/// The `when` an application's notification is decided by: OSC 99's `o=` names kitty's occasions, which are
/// the finish notice's `when` values by other names. `unfocused` holds while the pane lacks keyboard focus,
/// and `invisible` only while it is off screen as well -- in another tab, or in a window that is not shown
/// (kitty's `Channel.ui_state`).
/// @param occasion The notification's occasion.
/// @return The `when` it is decided by.
[[nodiscard]] constexpr config::NotifyWhen notifyWhenOf(vtbackend::DisplayOccasion occasion) noexcept
{
    switch (occasion)
    {
        case vtbackend::DisplayOccasion::Always: return config::NotifyWhen::Always;
        case vtbackend::DisplayOccasion::Unfocused: return config::NotifyWhen::Unfocused;
        case vtbackend::DisplayOccasion::Invisible: return config::NotifyWhen::Hidden;
    }
    return config::NotifyWhen::Always;
}

/// The occasion of an OSC 777 or OSC 9 notification, neither of which can state one: raised unless the pane
/// has focus, as foot raises them by default (`desktop-notifications.inhibit-when-focused`) and Windows
/// Terminal raises OSC 777 (`TerminalPage::_SendDesktopNotification`). kitty, iTerm2 and wezterm raise them
/// always; but a notification about the pane the user is typing into tells them nothing.
inline constexpr auto UnstatedNotificationOccasion = vtbackend::DisplayOccasion::Unfocused;

} // namespace contour::platform
