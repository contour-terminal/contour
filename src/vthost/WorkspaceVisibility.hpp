// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a daemon workspace is on display, and if not, whether it was put away or just left. A pure
/// decision, so every row of the table is testable without a socket or a session.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Whether a workspace is shown by a client, and what a later launch makes of it when it is not.
enum class WorkspaceVisibility : uint8_t
{
    Left = 0, ///< No client shows it and none switched away from it: a launch brings it back.
    Shown,    ///< At least one client shows it.
    Hidden,   ///< A client switched away from it and none shows it since: a launch leaves it alone.
};

/// Whether a window showed a restored workspace when the daemon's previous run ended (SessionSnapshot's
/// visibility), and none has shown it since.
enum class ShownBeforeRestart : uint8_t
{
    No = 0,  ///< Not restored, left before the restart, or shown again since.
    Yes = 1, ///< Its window went with the previous run, and a reconnect of that window takes it back.
};

/// Decides a workspace's visibility.
///
/// Shown if any client follows it; else Hidden if the last release was SwitchedAway; else Left. A follow
/// clears the recorded release (SessionHost), so a hidden workspace shown again and then dropped by a quit
/// (no release sent) is Left, not Hidden.
/// @param followers How many connections show the workspace now.
/// @param lastRelease Why the workspace was last released; nullopt when never, or followed since.
/// @return The workspace's visibility.
[[nodiscard]] constexpr WorkspaceVisibility visibilityOf(
    std::size_t followers, std::optional<proto::WorkspaceRelease> lastRelease) noexcept
{
    if (followers > 0)
        return WorkspaceVisibility::Shown;
    if (lastRelease == proto::WorkspaceRelease::SwitchedAway)
        return WorkspaceVisibility::Hidden;
    return WorkspaceVisibility::Left;
}

/// @param visibility A visibility.
/// @return Its name in the control API: `left`, `shown` or `hidden`.
[[nodiscard]] constexpr std::string_view nameOf(WorkspaceVisibility visibility) noexcept
{
    switch (visibility)
    {
        case WorkspaceVisibility::Left: return "left";
        case WorkspaceVisibility::Shown: return "shown";
        case WorkspaceVisibility::Hidden: return "hidden";
    }
    return "left";
}

} // namespace vthost
