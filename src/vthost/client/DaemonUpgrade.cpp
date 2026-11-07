// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonUpgrade.hpp>

#include <condition_variable>
#include <format>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>

#include <vthost/DaemonControlWire.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

namespace
{
    /// @param error Why a control request got no answer.
    /// @return The failure it is: NotRunning for an absent daemon, else Refused, saying why.
    [[nodiscard]] DaemonUpgradeFailure failureOf(ControlError const& error)
    {
        auto const isUnknown =
            error.code == ControlErrorCode::Timeout || error.code == ControlErrorCode::Transport;
        return DaemonUpgradeFailure { .error = error.code == ControlErrorCode::NotRunning
                                                   ? DaemonUpgradeError::NotRunning
                                                   : DaemonUpgradeError::Refused,
                                      .refusal = std::nullopt,
                                      .detail = error.toString(),
                                      .outcome = isUnknown ? DaemonUpgradeOutcome::Unknown
                                                           : DaemonUpgradeOutcome::Known };
    }

    /// @param reply The daemon's answer to an UpgradeDaemon.
    /// @return Nothing when it started the upgrade; otherwise the failure it names (upgradeFailureOf()).
    [[nodiscard]] std::expected<void, DaemonUpgradeFailure> outcomeOf(proto::UpgradeDaemonReply const& reply)
    {
        if (reply.result == 0)
            return {};
        return std::unexpected(upgradeFailureOf(reply));
    }
} // namespace

DaemonUpgradeFailure upgradeFailureOf(proto::UpgradeDaemonReply const& reply)
{
    auto const refusal = upgradeRefusalFrom(reply.result);
    auto detail = reply.detail;
    if (reply.result == proto::UpgradeDaemonReply::ExecFailedResult)
        detail = detail.empty() ? std::string { handover::ExecFailureText }
                                : std::format("{}: {}", handover::ExecFailureText, detail);
    else if (detail.empty())
        detail = refusal ? std::string { handover::describe(*refusal) }
                         : std::format("the daemon refused the upgrade (result {})", reply.result);
    return DaemonUpgradeFailure { .error = DaemonUpgradeError::Refused,
                                  .refusal = refusal,
                                  .detail = std::move(detail) };
}

std::expected<void, DaemonUpgradeFailure> upgradeDaemon(DaemonControlPort& control,
                                                        DaemonBinarySource& binary)
{
    auto const status = control.status();
    if (!status)
        return std::unexpected(failureOf(status.error()));
    if (!status->capabilities.contains(proto::Capability::LiveUpgrade))
        return std::unexpected(DaemonUpgradeFailure {
            .error = DaemonUpgradeError::Refused,
            .refusal = std::nullopt,
            .detail =
                std::format("the daemon (release {}) cannot upgrade in place", status->releaseVersion) });

    // Held until the daemon answered: by then it leases the image itself, and no prune can take it.
    auto const staged = binary.imageForSpawn();
    if (staged.origin == ImageOrigin::StagingFailed)
        return std::unexpected(DaemonUpgradeFailure { .error = DaemonUpgradeError::StageFailed,
                                                      .refusal = std::nullopt,
                                                      .detail = "this release's image could not be staged" });
    return control.upgrade(staged.imageId).transform_error(failureOf).and_then(outcomeOf);
}

std::chrono::steady_clock::time_point SystemSettlePacer::now() const
{
    return std::chrono::steady_clock::now();
}

SettleWait SystemSettlePacer::pause(std::chrono::milliseconds duration)
{
    // Nothing is ever notified but the stop: the predicate is the stop request alone.
    auto mutex = std::mutex {};
    auto wakeup = std::condition_variable_any {};
    auto lock = std::unique_lock { mutex };
    auto const stopped = wakeup.wait_for(lock, _stop, duration, [] { return false; });
    return stopped || _stop.stop_requested() ? SettleWait::Cancelled : SettleWait::Elapsed;
}

namespace
{
    /// How far a status read after the upgrade was asked for settles it.
    using Settled = std::optional<std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure>>;

    /// @param now The daemon's status once another image serves.
    /// @return The success it is, naming the release.
    [[nodiscard]] Settled succeededOn(DaemonStatus const& now)
    {
        return std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> { DaemonUpgradeSuccess {
            .release = now.releaseVersion } };
    }

    /// @param record A record that names a failure.
    /// @return The known failure it states.
    [[nodiscard]] Settled failedBy(proto::DaemonUpgradeRecord const& record)
    {
        return std::unexpected(upgradeFailureOf(
            proto::UpgradeDaemonReply { .result = record.lastFailure, .detail = record.lastFailureDetail }));
    }

    /// @param before The daemon's status before the upgrade was asked for; nullopt where it could not be
    /// read.
    /// @param now Its status since.
    /// @return Success once another image serves, the known failure the daemon recorded for this request, or
    ///         nullopt while neither shows yet.
    [[nodiscard]] Settled settledBy(std::optional<DaemonStatus> const& before, DaemonStatus const& now)
    {
        if (!before)
        {
            // No baseline: the record alone, if it states one, is all there is to go by.
            if (!now.upgrade)
                return std::nullopt;
            if (now.upgrade->lastFailure != 0)
                return failedBy(*now.upgrade);
            return succeededOn(now);
        }
        if (before->upgrade && now.upgrade)
        {
            // Judged by the generation, never the release: a same-release upgrade changes only the
            // generation.
            if (now.upgrade->generation > before->upgrade->generation)
                return succeededOn(now);
            // Only a request accepted since counts: an earlier one's failure is still recorded, and says
            // nothing of this one.
            if (now.upgrade->attempts > before->upgrade->attempts && now.upgrade->lastFailure != 0)
                return failedBy(*now.upgrade);
            return std::nullopt;
        }
        // The record came or went with the image: another image serves.
        if (before->upgrade.has_value() != now.upgrade.has_value())
            return succeededOn(now);
        // A daemon from before the record: its release is all there is to go by.
        if (now.releaseVersion != before->releaseVersion)
            return succeededOn(now);
        return std::unexpected(DaemonUpgradeFailure {
            .error = DaemonUpgradeError::Refused,
            .refusal = std::nullopt,
            .detail = std::format("the daemon is back on release {}; the upgrade did not happen",
                                  now.releaseVersion),
            .outcome = DaemonUpgradeOutcome::Known });
    }
} // namespace

std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(DaemonControlPort& control,
                                                                          DaemonControlPort& settling,
                                                                          DaemonBinarySource& binary,
                                                                          SettlePacer& pacer)
{
    return upgradeDaemonTo(control, settling, binary, pacer, std::nullopt);
}

std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(
    DaemonControlPort& control,
    DaemonControlPort& settling,
    DaemonBinarySource& binary,
    SettlePacer& pacer,
    std::optional<DaemonStatus> const& known)
{
    auto baseline = known;
    if (!baseline)
    {
        // Read twice at most: with no baseline, a stale failure cannot be told from this upgrade's.
        auto before = settling.status();
        if (!before)
            before = settling.status();
        baseline = before ? std::optional<DaemonStatus> { *before } : std::nullopt;
    }

    auto const started = upgradeDaemon(control, binary);
    auto provisional = DaemonUpgradeFailure {};
    if (started)
    {
        // A daemon from before the record cannot say how its upgrade ended: its "started" is all there is.
        if (baseline && !baseline->upgrade)
            return DaemonUpgradeSuccess {};
        // Provisional: the daemon may yet refuse as it quiesces, or fail to exec. Unknown until it says.
        provisional = DaemonUpgradeFailure { .error = DaemonUpgradeError::Refused,
                                             .refusal = std::nullopt,
                                             .detail = "the daemon started the upgrade, but did not say how "
                                                       "it ended",
                                             .outcome = DaemonUpgradeOutcome::Unknown };
    }
    else if (started.error().outcome == DaemonUpgradeOutcome::Known)
        return std::unexpected(started.error());
    else
        provisional = started.error();

    // The daemon serves again once it re-executed or rolled back: ask it, with the reconnect backoff, for as
    // long as a client holds its reconnect for an upgrade.
    auto const deadline = pacer.now() + UpgradeHold;
    for (auto const attempt: std::views::iota(uint32_t { 1 }))
    {
        if (pacer.now() >= deadline
            || pacer.pause(backoffDelay(BackoffSchedule {}, attempt)) == SettleWait::Cancelled)
            break;
        auto const status = settling.status();
        if (!status)
            continue;
        if (auto settled = settledBy(baseline, *status))
            return *std::move(settled);
    }
    return std::unexpected(std::move(provisional));
}

std::expected<DaemonUpgradeSuccess, DaemonUpgradeFailure> upgradeDaemonTo(DaemonControlPort& control,
                                                                          DaemonBinarySource& binary,
                                                                          SettlePacer& pacer)
{
    return upgradeDaemonTo(control, control, binary, pacer);
}

std::string describeFailure(DaemonUpgradeFailure const& failure)
{
    if (!failure.detail.empty())
        return failure.detail;
    if (failure.refusal)
        return std::string { handover::describe(*failure.refusal) };
    return "the daemon was not upgraded";
}

} // namespace vthost::client
