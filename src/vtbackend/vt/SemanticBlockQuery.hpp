// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/vt/Sequence.hpp>

#include <cstddef>
#include <cstdint>

namespace vtbackend
{

/// Where SBQUERY (`CSI > Ps ; Pn ; T1 ; T2 ; T3 ; T4 ; Ph b`) carries the two halves of a block id.
inline constexpr std::size_t SBQueryIdLowParameter = 1;  ///< Pn: the id's low 16 bits.
inline constexpr std::size_t SBQueryIdHighParameter = 6; ///< Ph: its high 16 bits; after the token, so that
                                                         ///< the token stays at parameters 2..5.

/// The block id an `SBQUERY 4` names: `Ph * 65536 + Pn`, Ph defaulting to 0.
///
/// A CSI parameter saturates at 65535, so a reader splits an id itself: Pn = id & 0xFFFF, Ph = id >> 16.
/// @param seq The SBQUERY sequence.
/// @return The id; zero, which no block has, when Pn is absent.
[[nodiscard]] inline CommandBlockId sbQueryBlockId(Sequence const& seq) noexcept
{
    auto const low = seq.paramOr<uint32_t>(SBQueryIdLowParameter, 0);
    auto const high = seq.paramOr<uint32_t>(SBQueryIdHighParameter, 0);
    return CommandBlockId { (high << 16) | (low & 0xFFFF) };
}

} // namespace vtbackend
