// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a daemon workspace is on display. A pure vocabulary, so every row is testable without a socket
/// or a session.

#include <cstdint>
#include <string_view>

namespace vthost
{

/// Whether some connection has a workspace selected.
enum class WorkspaceVisibility : uint8_t
{
    Hidden = 0, ///< No connection selects it.
    Shown = 1,  ///< At least one connection selects it.
};

/// How an attached connection ended, as the host takes it.
enum class DetachKind : uint8_t
{
    Dropped = 0, ///< Lost with no word: the window is reconnecting, so its workspace is held for a while.
    Closed,      ///< Ended on purpose (the client said Goodbye): its workspace is free at once.
};

/// @param visibility A visibility.
/// @return Its name in the control API: `shown` or `hidden`.
[[nodiscard]] constexpr std::string_view nameOf(WorkspaceVisibility visibility) noexcept
{
    switch (visibility)
    {
        case WorkspaceVisibility::Hidden: return "hidden";
        case WorkspaceVisibility::Shown: return "shown";
    }
    return "hidden";
}

} // namespace vthost
