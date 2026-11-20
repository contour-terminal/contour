// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::session
{

/// How a session's bytes reach its terminal.
enum class SessionTransport : uint8_t
{
    LocalProcess = 0,  ///< A process this machine runs on a local PTY.
    Ssh = 1,           ///< An in-process SSH session (vtpty::SshSession): the program runs on another host.
    TmuxPane = 2,      ///< A pane of a tmux server in control mode, which may run anywhere.
    DaemonReplica = 3, ///< A replica of a session a daemon hosts, which reads files on its own machine.
};

/// What kitty graphics may read on a session's behalf.
enum class MediaAccess : uint8_t
{
    None = 0,       ///< No file: `t=f` and `t=t` are refused.
    LocalFiles = 1, ///< Files on this machine, under the media policy.
};

/// @param transport How the session's bytes arrive.
/// @return What kitty graphics may read for it: local files only for a local process, whose paths name files
///         here. Any other program names files on its own host, or is not ours to trust with ours.
[[nodiscard]] constexpr MediaAccess mediaAccessFor(SessionTransport transport) noexcept
{
    switch (transport)
    {
        case SessionTransport::LocalProcess: return MediaAccess::LocalFiles;
        case SessionTransport::Ssh:
        case SessionTransport::TmuxPane:
        case SessionTransport::DaemonReplica: break;
    }
    return MediaAccess::None;
}

} // namespace contour::session
