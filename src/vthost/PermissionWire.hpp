// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A status-line extension's permission answer as it travels on the native protocol: single-sourced so the
/// client's reply and the daemon's record cannot disagree, as @ref vthost/StatusWire.hpp does for the status
/// display.
///
/// Decoding is TOTAL. The byte arrives as a peer chose it, and the daemon records the answer in its grant
/// store, so a value no enumerator has must read as no answer at all -- never be cast into one.

#include <cstdint>
#include <optional>

#include <vthost/WireEnum.hpp>

namespace vthost
{

/// What a user answered to a status-line extension's permission request: `vtextension::GrantState`,
/// value for value. vthost names its own, because a build without extensions links no vtextension and still
/// speaks the protocol.
enum class GrantState : uint8_t
{
    Pending = 0, ///< Not answered: the prompt was dismissed. Nothing is recorded.
    Granted = 1, ///< Approved; remembered.
    Denied = 2,  ///< Refused; remembered.
};

/// @param value A GrantState as the wire spells it.
/// @return The answer it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<GrantState> grantStateOf(uint8_t value) noexcept
{
    return enumUpTo(value, GrantState::Denied);
}

} // namespace vthost
