// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Restarting a running daemon on the client's release: stop it for the upgrade, then start one.

#include <cstdint>
#include <expected>
#include <string>

#include <vthost/DaemonStarter.hpp>
#include <vthost/client/DaemonControlClient.hpp>

namespace vthost::client
{

/// Which half of a restart failed.
enum class DaemonRestartError : uint8_t
{
    StopFailed = 0, ///< The running daemon did not stop.
    StartFailed,    ///< No new daemon came up.
};

/// A failed restart.
struct DaemonRestartFailure
{
    DaemonRestartError code = DaemonRestartError::StopFailed; ///< Which half failed.
    std::string detail;                                       ///< Why, in words for the user.
};

/// Stops the daemon for an upgrade (one that is already gone counts as stopped), then starts one.
/// @param control The running daemon.
/// @param starter Gets the new daemon serving.
/// @return Nothing once a daemon serves, or which half failed and why.
[[nodiscard]] std::expected<void, DaemonRestartFailure> restartDaemon(DaemonControlPort& control,
                                                                      DaemonStarter& starter);

} // namespace vthost::client
