// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonUpgrade.hpp>

#include <format>
#include <string>
#include <thread>
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
    /// @return Nothing when it started the upgrade; otherwise the refusal it named, if this build knows it,
    ///         with the daemon's own words, or the refusal's where it gave none.
    [[nodiscard]] std::expected<void, DaemonUpgradeFailure> outcomeOf(proto::UpgradeDaemonReply const& reply)
    {
        if (reply.result == 0)
            return {};
        auto const refusal = upgradeRefusalFrom(reply.result);
        auto detail = reply.detail;
        if (reply.result == proto::UpgradeDaemonReply::ExecFailedResult)
            detail = detail.empty() ? std::string { handover::ExecFailureText }
                                    : std::format("{}: {}", handover::ExecFailureText, detail);
        else if (detail.empty())
            detail = refusal ? std::string { handover::describe(*refusal) }
                             : std::format("the daemon refused the upgrade (result {})", reply.result);
        return std::unexpected(DaemonUpgradeFailure {
            .error = DaemonUpgradeError::Refused, .refusal = refusal, .detail = std::move(detail) });
    }
} // namespace

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

void SystemSettlePacer::pause(std::chrono::milliseconds duration)
{
    std::this_thread::sleep_for(duration);
}

std::expected<void, DaemonUpgradeFailure> upgradeDaemonTo(DaemonControlPort& control,
                                                          DaemonBinarySource& binary,
                                                          SettlePacer& pacer)
{
    auto const before = control.status();
    auto upgraded = upgradeDaemon(control, binary);
    if (upgraded || !before || upgraded.error().outcome == DaemonUpgradeOutcome::Known)
        return upgraded;

    // The daemon may have answered nothing because it was already replacing itself: ask again, with the
    // reconnect backoff, for as long as a client holds its reconnect for an upgrade.
    auto const deadline = pacer.now() + UpgradeHold;
    for (auto attempt = uint32_t { 1 }; pacer.now() < deadline; ++attempt)
    {
        pacer.pause(backoffDelay(BackoffSchedule {}, attempt));
        auto const status = control.status();
        if (!status)
            continue;
        if (status->releaseVersion != before->releaseVersion)
            return {};
        return std::unexpected(DaemonUpgradeFailure {
            .error = DaemonUpgradeError::Refused,
            .refusal = std::nullopt,
            .detail = std::format("the daemon is back on release {}; the upgrade did not happen",
                                  status->releaseVersion),
            .outcome = DaemonUpgradeOutcome::Known });
    }
    return upgraded;
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
