// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A StopDaemon's reason as it travels on the native protocol. Decoding is TOTAL: the byte arrives as a
/// peer chose it, so a value no enumerator has reads as no reason at all -- never be cast into one.

#include <cstdint>
#include <optional>

#include <vthost/WireEnum.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// @param value A proto::StopReason as the wire spells it.
/// @return The reason it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<proto::StopReason> stopReasonFrom(uint8_t value) noexcept
{
    return enumUpTo(value, proto::StopReason::UserKeepingWorkspace);
}

} // namespace vthost
