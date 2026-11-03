// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Upgrading a running daemon in place to the client's release (live upgrade): the client stages its own
/// image into the shared store, and the daemon replaces itself with it, keeping every session running.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <vthost/DaemonBinary.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/handover/UpgradeError.hpp>

namespace vthost::client
{

/// Why a daemon was not upgraded.
enum class DaemonUpgradeError : uint8_t
{
    /// The daemon refused, offers no live upgrade, or its exec into the new image failed: it serves on as
    /// before. Also the daemon could not be asked, or did not answer in time (ControlErrorCode::Timeout,
    /// ControlErrorCode::Transport): then the outcome is unknown, and DaemonUpgradeFailure::detail says
    /// which.
    Refused = 0,
    StageFailed, ///< This release's image could not be staged; the daemon was not asked.
    NotRunning,  ///< No daemon listens at the endpoint.
};

/// A daemon that was not upgraded, and why.
struct DaemonUpgradeFailure
{
    DaemonUpgradeError error = DaemonUpgradeError::Refused; ///< What kind of failure.
    /// The refusal the daemon named; set iff @ref error is Refused and the daemon's answer names a refusal
    /// this build knows. Unset for an exec failure (proto::UpgradeDaemonReply::ExecFailedResult), a refusal
    /// only a newer daemon knows, and a daemon that could not be asked.
    std::optional<handover::UpgradeRefusal> refusal = std::nullopt;
    std::string detail = {}; ///< Why, in words for the user: the daemon's own where it gave any.
};

/// Upgrades the running daemon in place to this client's release.
///
/// Asks the daemon's status first, so that nothing is staged for a daemon that is not there or offers no live
/// upgrade; then stages this client's image (DaemonBinarySource::imageForSpawn()) and sends
/// `UpgradeDaemon{imageId}` -- an empty id where this client runs in place, naming the daemon's own installed
/// executable. The image's lease is held until the daemon answered, by when the daemon holds its own.
/// @param control The running daemon. A port of DefaultUpgradeTimeout, as the answer waits on the new
///        image's readiness probe.
/// @param binary What this client's image is staged from.
/// @return Nothing once the daemon started the upgrade: it closes every connection and serves again as the
///         same daemon shortly. Otherwise why not.
[[nodiscard]] std::expected<void, DaemonUpgradeFailure> upgradeDaemon(DaemonControlPort& control,
                                                                      DaemonBinarySource& binary);

} // namespace vthost::client
