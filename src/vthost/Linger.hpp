// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a systemd user's services outlive their last login (`loginctl enable-linger`).

#include <core/Environment.hpp>

#include <cstdint>
#include <string_view>

#include <vthost/CommandRunner.hpp>
#include <vthost/ServiceControl.hpp>

namespace vthost
{

/// Whether systemd keeps a user's manager, and so their daemon, running after the last session ends.
enum class LingerState : uint8_t
{
    Unknown = 0, ///< Could not be determined: no `loginctl`, a failure, or output not understood.
    Off,         ///< `Linger=no`: the user manager stops at the last logout.
    On,          ///< `Linger=yes`: the user manager keeps running.
};

/// Asks `loginctl show-user <user> -p Linger`. Pure over @p commands; there is no platform check, the
/// caller decides whether systemd is the relevant service manager.
/// @param commands Where `loginctl` is run.
/// @param user The invoking user's login name; empty yields Unknown without running anything.
/// @return The linger state, Unknown whenever it could not be read.
[[nodiscard]] LingerState lingerStateOf(CommandRunner& commands, std::string_view user);

/// @return One sentence saying the daemon stops at logout, with the `loginctl enable-linger` command.
[[nodiscard]] std::string_view lingerAdvice();

/// What to tell the user right after a successful service install.
/// @param mode The start mode that was installed; only Logon lands in the systemd user manager.
/// @param commands Where `loginctl` is run.
/// @param environment Supplies the invoking user's login name (`USER`).
/// @return lingerAdvice() for Logon with lingering Off; otherwise empty. Without a user, or for another
///         mode, no command is run.
[[nodiscard]] std::string_view lingerHintAfterInstall(ServiceStartMode mode,
                                                      CommandRunner& commands,
                                                      core::Environment const& environment);

} // namespace vthost
