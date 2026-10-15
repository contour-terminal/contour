// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How an enum-ish byte off the native wire becomes an enumerator: in one place, which fails closed.
///
/// A peer chooses every byte it sends, so a byte cast straight into its enum is how a value no
/// enumerator has reaches a switch that assumes otherwise -- a `core::unreachable()` in the attached
/// client. Every *Wire.hpp decoder of a contiguous enum goes through enumUpTo() instead of spelling the
/// bound check again; a sparse one (vtbackend::ContextSignal) tests membership itself.

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace vthost
{

/// @param value An enumerator as the wire spells it.
/// @param last The last enumerator of a contiguous enum that starts at zero.
/// @return @p value as an enumerator of the enum, or nullopt. An enumerator added past @p last fails
///         CLOSED (reads as unknown) until @p last moves.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr std::optional<E> enumUpTo(uint8_t value, E last) noexcept
{
    return std::cmp_less_equal(value, std::to_underlying(last)) ? std::optional { static_cast<E>(value) }
                                                                : std::nullopt;
}

} // namespace vthost
