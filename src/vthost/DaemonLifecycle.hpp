// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When a daemon ends, and why it did: its lifecycle policy and the causes every shutdown carries. A
/// dependency-free header, so the CLI, the status reply and the daemon all read one vocabulary.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace vthost
{

/// Whether, and when, a daemon ends without being asked to.
enum class DaemonLifecycle : std::uint8_t
{
    /// Keeps serving with zero sessions — a user-started or service-managed daemon, whose whole point is
    /// to still be there when the next client attaches.
    Persistent = 0,
    /// Terminates once the last hosted session is gone (tmux spells this `exit-empty`).
    ExitWhenEmpty,
    /// Terminates once it has had no session and no client for a grace period — a daemon started on
    /// demand, which belongs to the clients that needed it.
    ExitWhenIdle,
};

/// The lifecycles' command-line spellings, single-sourced: the CLI parser, the spawn arguments and the
/// status text all read this one table.
inline constexpr auto DaemonLifecycleNames = std::to_array<std::pair<std::string_view, DaemonLifecycle>>({
    { "persistent", DaemonLifecycle::Persistent },
    { "exit-with-last-session", DaemonLifecycle::ExitWhenEmpty },
    { "exit-when-idle", DaemonLifecycle::ExitWhenIdle },
});

/// @param lifecycle The lifecycle to name.
/// @return Its command-line spelling.
[[nodiscard]] constexpr std::string_view nameOf(DaemonLifecycle lifecycle) noexcept
{
    for (auto const& [name, value]: DaemonLifecycleNames)
        if (value == lifecycle)
            return name;
    return "persistent";
}

/// @param name A command-line spelling, as written by a user.
/// @return The lifecycle it names, or nullopt if it names none, so a caller can report the typo.
[[nodiscard]] constexpr std::optional<DaemonLifecycle> daemonLifecycleFrom(std::string_view name) noexcept
{
    for (auto const& [candidate, value]: DaemonLifecycleNames)
        if (candidate == name)
            return value;
    return std::nullopt;
}

/// How long an ExitWhenIdle daemon waits, with no session and no client, before it ends.
inline constexpr auto DefaultIdleExitGrace = std::chrono::seconds { 10 };

/// What asked a daemon to shut down. Every trigger names one, and the single shutdown log line states it.
enum class ShutdownCause : std::uint8_t
{
    Signal = 0,        ///< SIGINT/SIGTERM (POSIX) or a console-control event (Windows).
    LastSessionClosed, ///< An ExitWhenEmpty daemon's last hosted session went away.
    Idle,              ///< An ExitWhenIdle daemon's grace period passed with nothing to serve.
    StopRequested,     ///< A Control connection asked it to stop.
    UpgradeRestart,    ///< A Control connection asked it to stop, to be replaced by a newer release.
};

/// The causes' log text, single-sourced.
inline constexpr auto ShutdownCauseNames = std::to_array<std::pair<std::string_view, ShutdownCause>>({
    { "signal received", ShutdownCause::Signal },
    { "the last hosted session closed", ShutdownCause::LastSessionClosed },
    { "idle", ShutdownCause::Idle },
    { "stop requested", ShutdownCause::StopRequested },
    { "restart for upgrade", ShutdownCause::UpgradeRestart },
});

/// @param cause The cause to name.
/// @return Its log text.
[[nodiscard]] constexpr std::string_view nameOf(ShutdownCause cause) noexcept
{
    for (auto const& [name, value]: ShutdownCauseNames)
        if (value == cause)
            return name;
    return "shutdown requested";
}

} // namespace vthost
