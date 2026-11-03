// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonUpgrade.hpp>

#include <format>
#include <string>
#include <utility>

#include <vthost/DaemonControlWire.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

namespace
{
    /// @param error Why a control request got no answer.
    /// @return The failure it is: NotRunning for an absent daemon, else Refused, saying why.
    [[nodiscard]] DaemonUpgradeFailure failureOf(ControlError const& error)
    {
        return DaemonUpgradeFailure { .error = error.code == ControlErrorCode::NotRunning
                                                   ? DaemonUpgradeError::NotRunning
                                                   : DaemonUpgradeError::Refused,
                                      .refusal = std::nullopt,
                                      .detail = error.toString() };
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
        if (detail.empty())
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

} // namespace vthost::client
