// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a daemon keeps its workspace, and what its last snapshot keeps as it ends. Light on purpose:
/// DaemonConfig carries it, and DaemonSpawn.hpp reaches every `contour` translation unit that starts or talks
/// to a daemon, none of which should compile the session host for it (WorkspacePersistence.hpp is the rest).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <vthost/DaemonLifecycle.hpp>
#include <vthost/SnapshotSchedule.hpp>

namespace vthost
{

/// Whether a snapshot keeps what a pane printed.
enum class ScrollbackPersistence : std::uint8_t
{
    Off = 0, ///< Layout and directories only; a restored pane starts blank.
    Tail,    ///< Also the newest rows of each pane's history.
};

/// How a daemon keeps its workspace.
struct WorkspacePersistenceConfig
{
    std::filesystem::path snapshotPath {}; ///< Empty: persistence off (a bare DaemonConfig{}, tests).
    ScrollbackPersistence scrollback = ScrollbackPersistence::Tail; ///< Whether panes' history is kept.
    std::size_t scrollbackRows = 1000;                              ///< The most rows kept per pane.
    std::vector<std::string> rerunPrograms {}; ///< The programs a restored pane may re-run.
    std::chrono::seconds interval { 30 };      ///< How often a changed workspace is written.
    SchedulePolicy schedule {};                ///< The kill grace and the layout debounce.
};

/// What the daemon's last snapshot holds as it ends.
enum class ShutdownWorkspace : std::uint8_t
{
    Keep = 0,    ///< The live workspace, to be restored at the next start.
    Discard = 1, ///< Nothing: `contour daemon stop`.
};

/// What a daemon's last snapshot keeps, by what ended it. Only a plain `contour daemon stop` discards: a
/// signal is how a reboot ends the daemon, a last session's end may be a reboot killing the shells (one
/// ended by `exit` has already written an empty workspace), an idle or upgrade end is not the user's
/// choice, and `--keep-snapshot` asks to keep it.
/// @param cause What asked for the shutdown.
/// @return Discard for ShutdownCause::StopRequested, Keep for every other cause.
[[nodiscard]] constexpr ShutdownWorkspace workspaceFor(ShutdownCause cause) noexcept
{
    switch (cause)
    {
        case ShutdownCause::StopRequested: return ShutdownWorkspace::Discard;
        case ShutdownCause::Signal:
        case ShutdownCause::LastSessionClosed:
        case ShutdownCause::Idle:
        case ShutdownCause::UpgradeRestart:
        case ShutdownCause::StopRequestedKeepingWorkspace: return ShutdownWorkspace::Keep;
    }
    return ShutdownWorkspace::Keep;
}

} // namespace vthost
