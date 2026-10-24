// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Why a hosted session ended. A session the user closed, or whose shell said `exit`, is gone for good; one
/// a signal ended (the system stopping every shell before the daemon on shutdown, or Windows ending its
/// console children at logoff) is a casualty of the machine going down, and a workspace snapshot keeps it.

#include <vtpty/Process.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace vthost
{

/// Why a session ended.
enum class SessionEnd : std::uint8_t
{
    Exited = 0,    ///< The program ended on its own (`exit`), or how it ended is unknown.
    Requested = 1, ///< A client closed the pane or tab.
    Killed = 2,    ///< A signal ended it, or Windows' console-close status: a logoff or shutdown.
};

/// @param end The cause.
/// @return A lower-case word for a log line.
[[nodiscard]] constexpr std::string_view describe(SessionEnd end) noexcept
{
    switch (end)
    {
        case SessionEnd::Exited: return "exited";
        case SessionEnd::Requested: return "closed on request";
        case SessionEnd::Killed: return "killed";
    }
    return "ended";
}

/// STATUS_CONTROL_C_EXIT, the status a Windows console child ends with when its console is closed by a
/// logoff or shutdown, as well as by Ctrl+C.
inline constexpr int WindowsControlCExitCode = static_cast<int>(0xC000013AU);

/// Classifies how a session's child ended.
/// @param status What the child's wait returned; nullopt when it could not be asked, or was still running.
/// @return Killed for a signal or the Windows console-close status, else Exited.
[[nodiscard]] constexpr SessionEnd sessionEndOf(
    std::optional<vtpty::Process::ExitStatus> const& status) noexcept
{
    if (!status)
        return SessionEnd::Exited;
    if (std::holds_alternative<vtpty::Process::SignalExit>(*status))
        return SessionEnd::Killed;
    return std::get<vtpty::Process::NormalExit>(*status).exitCode == WindowsControlCExitCode
               ? SessionEnd::Killed
               : SessionEnd::Exited;
}

} // namespace vthost
