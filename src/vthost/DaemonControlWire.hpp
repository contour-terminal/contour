// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A StopDaemon's reason, and an UpgradeDaemon's outcome, as they travel on the native protocol. Decoding is
/// TOTAL: the byte arrives as a peer chose it, so a value no enumerator has reads as no reason at all --
/// never be cast into one.

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <vthost/WireEnum.hpp>
#include <vthost/handover/UpgradeError.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// @param value A proto::StopReason as the wire spells it.
/// @return The reason it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<proto::StopReason> stopReasonFrom(uint8_t value) noexcept
{
    return enumUpTo(value, proto::StopReason::UserKeepingWorkspace);
}

/// @return The reply to an UpgradeDaemon that started the upgrade.
[[nodiscard]] inline proto::UpgradeDaemonReply upgradeStartedReply()
{
    return proto::UpgradeDaemonReply { .result = 0, .detail = {} };
}

/// @param refusal A refusal.
/// @return The reply that reports it: `refusal + 1`, since zero is "started".
[[nodiscard]] inline proto::UpgradeDaemonReply upgradeRefusedReply(handover::UpgradeRefusal refusal,
                                                                   std::string detail)
{
    return proto::UpgradeDaemonReply { .result = static_cast<uint8_t>(std::to_underlying(refusal) + 1),
                                       .detail = std::move(detail) };
}

/// @param error Why an upgrade did not happen.
/// @return The reply that reports it: a refusal as upgradeRefusedReply() spells it, an exec failure as
///         UpgradeDaemonReply::ExecFailedResult with the cause as the detail.
[[nodiscard]] inline proto::UpgradeDaemonReply upgradeFailureReply(handover::UpgradeError const& error)
{
    return std::visit(
        [](auto const& failure) {
            using Failure = std::decay_t<decltype(failure)>;
            if constexpr (std::is_same_v<Failure, handover::UpgradeRefusal>)
                return upgradeRefusedReply(failure, std::string { handover::describe(failure) });
            else
                return proto::UpgradeDaemonReply { .result = proto::UpgradeDaemonReply::ExecFailedResult,
                                                   .detail = failure.cause.message() };
        },
        error);
}

/// @param result An UpgradeDaemonReply's result, as the wire spells it.
/// @return The refusal it names; nullopt for "started", for the exec failure, and for a refusal this build
///         does not know.
[[nodiscard]] constexpr std::optional<handover::UpgradeRefusal> upgradeRefusalFrom(uint8_t result) noexcept
{
    if (result == 0)
        return std::nullopt;
    return enumUpTo(static_cast<uint8_t>(result - 1), handover::lastRefusal());
}

} // namespace vthost
