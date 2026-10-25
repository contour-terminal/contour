// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/EndReason.hpp>

#include <cstdint>

#include <vthost/client/ConnectionState.hpp>

namespace contour::window
{

/// What a pane shows over and around its terminal.
enum class PaneState : uint8_t
{
    Live = 0,     ///< The session runs; the pane is the terminal alone.
    Reconnecting, ///< The daemon connection is being sought again; the grid is frozen and dimmed.
    Ended,        ///< The session ended and the pane waits for Restart or Close in its footer.
};

/// Decides a pane's state from its session's end and its host connection.
/// @param end        How the pane's session ended; None while it runs.
/// @param connection The state of the daemon connection the session is mirrored over (Connected for a
///                   local one).
/// @return Ended once the session ended, whatever the connection; else Reconnecting while the connection
///         is sought again; else Live.
[[nodiscard]] constexpr PaneState paneStateOf(session::EndReason end,
                                              vthost::client::ConnectionState connection) noexcept
{
    if (end != session::EndReason::None)
        return PaneState::Ended;
    if (connection == vthost::client::ConnectionState::Reconnecting)
        return PaneState::Reconnecting;
    return PaneState::Live;
}

} // namespace contour::window
