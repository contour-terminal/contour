// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::reserveSlot` -- one slot of a bounded budget of calls that are running and cannot be
/// interrupted: the lookups an @c ExecutorAddressResolver hands out, and the chain verifications a
/// verifying TLS context's sockets start.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace core::net::detail
{

/// Whether a call got one of the budget's slots.
enum class SlotReservation : std::uint8_t
{
    Refused,  ///< The budget was full; nothing was taken.
    Reserved, ///< One slot is the caller's, to be given back exactly once.
};

/// Takes one slot of @p inFlight's budget, if one is free.
///
/// A loop of compare-exchange rather than an add and a take-back, so that a full budget is never
/// exceeded even for a moment -- several loops may draw on one budget at once.
/// @param inFlight The slots taken.
/// @param maxInFlight How many there are.
/// @return Whether one was taken.
[[nodiscard]] inline SlotReservation reserveSlot(std::atomic<std::size_t>& inFlight,
                                                 std::size_t maxInFlight) noexcept
{
    auto current = inFlight.load(std::memory_order_relaxed);
    do
    {
        if (current >= maxInFlight)
            return SlotReservation::Refused;
    } while (!inFlight.compare_exchange_weak(
        current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
    return SlotReservation::Reserved;
}

} // namespace core::net::detail
