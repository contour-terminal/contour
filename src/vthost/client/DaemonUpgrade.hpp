// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Upgrading a running daemon in place to the client's release (live upgrade): the client stages its own
/// image into the shared store, and the daemon replaces itself with it, keeping every session running.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

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

/// Whether the daemon is known to have declined, or may be upgrading regardless.
enum class DaemonUpgradeOutcome : uint8_t
{
    Known = 0, ///< The failure is the whole story: the daemon serves on as before.
    /// The daemon could not be asked, or did not answer in time (ControlErrorCode::Timeout,
    /// ControlErrorCode::Transport): it may be replacing itself right now. Ask it again before reporting.
    Unknown,
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
    /// Whether the daemon may be upgrading anyway.
    DaemonUpgradeOutcome outcome = DaemonUpgradeOutcome::Known;
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

/// Time for the settling of an upgrade whose outcome is not known: the clock it is bounded by, and the
/// wait between two asks. A seam, so that a test advances time by hand and never sleeps.
class SettlePacer
{
  public:
    virtual ~SettlePacer() = default;

    /// @return The current time on a steady clock.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    /// Waits.
    /// @param duration How long.
    virtual void pause(std::chrono::milliseconds duration) = 0;
};

/// A SettlePacer on the system's steady clock, which sleeps the calling thread.
class SystemSettlePacer final: public SettlePacer
{
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override;
    void pause(std::chrono::milliseconds duration) override;
};

/// Upgrades the running daemon as upgradeDaemon() does, and settles an outcome that is not known.
///
/// A failure of DaemonUpgradeOutcome::Unknown -- the daemon did not answer, or the connection broke -- may
/// just be the daemon already re-execing. The daemon is then asked for its status, with the reconnect
/// backoff between the asks, until UpgradeHold ran out. An answer naming a release other than the one
/// read before the call means the upgrade happened. An answer naming the same release means it was
/// refused or rolled back: a known failure. No answer at all leaves the outcome unknown.
/// @param control The running daemon.
/// @param binary What this client's image is staged from.
/// @param pacer The clock and the waits between asks.
/// @return Nothing once the daemon runs another release than before the call, or started the upgrade;
///         otherwise why not.
[[nodiscard]] std::expected<void, DaemonUpgradeFailure> upgradeDaemonTo(DaemonControlPort& control,
                                                                        DaemonBinarySource& binary,
                                                                        SettlePacer& pacer);

/// @param failure A failed upgrade.
/// @return What the user is told: the daemon's own words where it gave any, else the text of the refusal
///         (handover::UpgradeRefusalRows) it named, else a generic sentence. Never empty.
[[nodiscard]] std::string describeFailure(DaemonUpgradeFailure const& failure);

} // namespace vthost::client
