// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ParkId`: the name of one piece of work parked on an @c EventLoop, and its hash.
///
/// **A header of its own because every socket needs the name and none needs the table.** An
/// awaitable holds the id of the park it may cancel, so `IoAwaitable.hpp` -- and through it
/// `ISocket.hpp`, which every consumer of `core::net` includes -- needs `ParkId` complete. It used to
/// get it from `detail/ParkTable.hpp`, and the table, the deadline heap, the backend contract and
/// `<ranges>` came along for one strong integer: 17,281 of the 130,784 preprocessed lines a
/// translation unit including `ISocket.hpp` paid, 13%
/// ([core-cpp#43](https://github.com/contour-terminal/core-cpp/issues/43), measured with
/// `clang++ -E`; the issue's 99.4% from `clang++ -H` counted headers the rest of the interface
/// includes too).
///
/// **The id IS the generation check.** No id is handed out twice in a loop's life -- most come from
/// one 64-bit counter, and a socket operation's from its resident slot's own generation -- so a
/// cancel request that arrives after its park is gone finds nothing, however many parks have been
/// made since. See `detail/ParkTable.hpp`.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace core::net
{

/// Names one piece of work parked on an @c EventLoop, for the loop's own bookkeeping and for
/// @c EventLoop::requestCancel, which any thread may call.
///
/// A strong struct rather than an `enum class` because it is an opaque, monotonically-allocated
/// handle id — a wide value space that never wraps in a session — and not an enumeration of named
/// cases. (`performance-enum-size` refuses a `std::uint64_t` enumeration here, and it is an error
/// in this tree.)
struct ParkId
{
    std::uint64_t value = 0; ///< The park's id; 0 means none.

    /// @return True if two ids name the same park.
    [[nodiscard]] friend constexpr bool operator==(ParkId, ParkId) noexcept = default;

    /// @return True if this id names a live park (non-zero).
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

    /// @return The sentinel for no park, which is what a failed registration reports.
    [[nodiscard]] static constexpr ParkId invalid() noexcept { return ParkId { 0 }; }
};

} // namespace core::net

namespace std
{

/// Hash specialization so @c ParkId can key an unordered container: the loop maps a park's id to
/// the park itself. Declared HERE, between the type and its first use, because a specialization
/// that arrives after the container is instantiated is not the one the container picked up.
template <>
struct hash<core::net::ParkId>
{
    /// @param park The id to hash.
    /// @return The hash of its underlying value.
    [[nodiscard]] std::size_t operator()(core::net::ParkId park) const noexcept
    {
        return std::hash<std::uint64_t> {}(park.value);
    }
};

} // namespace std
