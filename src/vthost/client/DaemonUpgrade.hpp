// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Upgrading a running daemon in place to the client's release (live upgrade): the client stages its own
/// image into the shared store, and the daemon replaces itself with it, keeping every session running.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
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
    /// ControlErrorCode::Transport), or started the upgrade and did not say how it ended: it may be replacing
    /// itself right now. Ask it again before reporting.
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

/// How a SettlePacer's wait ended.
enum class SettleWait : uint8_t
{
    Elapsed = 0, ///< It waited as long as it was asked to.
    Cancelled,   ///< It was asked to stop: the settling ends, its outcome unknown.
};

/// Time for the settling of an upgrade: the clock it is bounded by, and the wait between two asks. A seam,
/// so that a test advances time by hand and never sleeps.
class SettlePacer
{
  public:
    virtual ~SettlePacer() = default;

    /// @return The current time on a steady clock.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    /// Waits, unless asked to stop.
    /// @param duration How long.
    /// @return Elapsed once it waited @p duration; Cancelled, at once, when asked to stop.
    [[nodiscard]] virtual SettleWait pause(std::chrono::milliseconds duration) = 0;
};

/// A SettlePacer on the system's steady clock, which sleeps the calling thread until its time is up or its
/// stop token is asked to stop: the thread a GUI runs it on is joined at shutdown, which must not wait out
/// the settling.
class SystemSettlePacer final: public SettlePacer
{
  public:
    /// @param stop Ends every wait at once once stop is requested; a default token never does.
    explicit SystemSettlePacer(std::stop_token stop = {}): _stop(std::move(stop)) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const override;
    [[nodiscard]] SettleWait pause(std::chrono::milliseconds duration) override;

  private:
    std::stop_token _stop;
};

/// A daemon that was upgraded.
struct DaemonUpgradeSuccess
{
    /// The release the daemon serves now, as its status said after the upgrade; empty where none was read
    /// (a daemon from before the upgrade record, whose "started" is taken at its word).
    std::string release = {};
};

/// Upgrades the running daemon as upgradeDaemon() does, and settles how the upgrade ended.
///
/// The daemon's "started" is provisional: it is sent before the daemon quiesces, which may still refuse
/// (proto::DaemonUpgradeRecord), and the exec may fail. The connection is gone by then, so the daemon's
/// status is asked for, with the reconnect backoff between the asks, until UpgradeHold ran out. So is the
/// status of a daemon that did not answer at all (DaemonUpgradeOutcome::Unknown), which may just be
/// re-execing.
///
/// A daemon that states its upgrade record is judged by it, never by its release, so that an upgrade to the
/// same release is told apart from none: a generation past the one read before the call means the new image
/// serves; a recorded failure at the same generation is a known failure, with the daemon's reason. A daemon
/// from before the record is judged by its release: another one than before means the upgrade happened, the
/// same one that it did not -- and its "started" is taken at its word, as nothing better can be had.
///
/// A failure the record states counts as this upgrade's only if the daemon accepted more requests
/// (proto::DaemonUpgradeRecord::attempts) than it had before this one, so that a failure an earlier request
/// left behind is never taken for it. Where the status before the call could not be read (even twice), there
/// is no baseline: the daemon's "started" stays provisional, and the first status that states a record
/// settles it -- the record's failure, if it names one, else success; a status without a record settles
/// nothing.
/// @param control The running daemon, asked for the upgrade itself.
/// @param settling The running daemon, asked for its status before and after: a port of a short timeout
///        serves, as none of these answers waits on anything.
/// @param binary What this client's image is staged from.
/// @param pacer The clock and the waits between asks.
/// @return The release the daemon serves once it runs the new image; otherwise why not. Nothing conclusive
///         by the end of the hold, or a pacer that was cancelled, leaves the outcome
///         DaemonUpgradeOutcome::Unknown.
[[nodiscard]] std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(
    DaemonControlPort& control, DaemonControlPort& settling, DaemonBinarySource& binary, SettlePacer& pacer);

/// upgradeDaemonTo() with the daemon's status before the call already in hand.
/// @param control The running daemon, asked for the upgrade itself.
/// @param settling The running daemon, asked for its status after.
/// @param binary What this client's image is staged from.
/// @param pacer The clock and the waits between asks.
/// @param known The daemon's status as just read, the baseline an upgrade is judged against; nullopt reads it
///        (twice at most) as the other overloads do.
/// @return As the other overloads.
[[nodiscard]] std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(
    DaemonControlPort& control,
    DaemonControlPort& settling,
    DaemonBinarySource& binary,
    SettlePacer& pacer,
    std::optional<DaemonStatus> const& known);

/// upgradeDaemonTo() asking @p control for everything.
[[nodiscard]] std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(
    DaemonControlPort& control, DaemonBinarySource& binary, SettlePacer& pacer);

/// @param reply A daemon's answer naming a failure (`result` not 0), or the last failure its status records
///        (proto::DaemonUpgradeRecord), spelled the same way.
/// @return The failure, known: the refusal it names if this build knows it, with the daemon's words, or the
///         refusal's own where it gave none; an exec failure introduced by handover::ExecFailureText.
[[nodiscard]] DaemonUpgradeFailure upgradeFailureOf(proto::UpgradeDaemonReply const& reply);

/// @param failure A failed upgrade.
/// @return What the user is told: the daemon's own words where it gave any, else the text of the refusal
///         (handover::UpgradeRefusalRows) it named, else a generic sentence. Never empty.
[[nodiscard]] std::string describeFailure(DaemonUpgradeFailure const& failure);

} // namespace vthost::client
