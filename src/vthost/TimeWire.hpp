// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Instants and spans as the native protocol spells them: signed 64-bit NANOSECONDS — wall-clock
/// instants since the Unix epoch, steady-clock spans as plain lengths.
///
/// One unit for every clock and platform, because the two ends need not share one: system_clock ticks
/// in 100 ns on MSVC, 1 µs in libc++ and 1 ns in libstdc++. Nanoseconds are at least as fine as all
/// three, so an instant crossing between two builds on one platform is exact.
///
/// A conversion here only changes the unit, and holds a value back only where changing the unit would
/// overflow. The range a terminal computes with is the terminal's to enforce, at the seams that take an
/// instant or a span from elsewhere (vtbackend::heldInstant, vtbackend::heldSpan): there every producer
/// is held to it, not only this protocol.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ratio>

namespace vthost
{

/// A local wall-clock instant as the wire spells it.
///
/// Where the instant ticks coarser than a nanosecond -- system_clock does, by 100 ns on MSVC -- the
/// conversion is a multiplication, which an instant past 2262 overflows, and a broken local clock can
/// report one. So the instant is held, in its own ticks and BEFORE it is converted, to what an int64_t
/// count of nanoseconds can name: one outside lands on the edge it overshot.
/// @tparam Ticks The instant's tick: this host's system_clock::duration, or any other no finer than a
///         nanosecond.
/// @param when A wall-clock instant, as this host's clock reads it.
/// @return It in nanoseconds since the Unix epoch, or the nearer edge of what they can count.
template <typename Ticks>
[[nodiscard]] constexpr int64_t toWireTime(std::chrono::sys_time<Ticks> when) noexcept
{
    static_assert(std::ratio_greater_equal_v<typename Ticks::period, std::nano>,
                  "A tick finer than a nanosecond cannot hold the edges of what nanoseconds count.");
    constexpr auto Earliest = std::chrono::ceil<Ticks>(std::chrono::nanoseconds::min());
    constexpr auto Latest = std::chrono::floor<Ticks>(std::chrono::nanoseconds::max());
    auto const held = std::clamp(when.time_since_epoch(), Earliest, Latest);
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(held).count());
}

/// The inverse of @ref toWireTime, truncated to this platform's system_clock period.
///
/// Every int64_t count of nanoseconds converts: a system_clock tick is never finer than a nanosecond, so
/// the conversion is a division, if anything.
/// @param nanoseconds Nanoseconds since the Unix epoch, as a peer sent them.
/// @return The instant they name.
[[nodiscard]] constexpr std::chrono::system_clock::time_point fromWireTime(int64_t nanoseconds) noexcept
{
    return std::chrono::system_clock::time_point {
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds { nanoseconds })
    };
}

/// @param span A steady-clock length (a command's duration).
/// @return It in nanoseconds.
[[nodiscard]] constexpr int64_t toWireDuration(std::chrono::steady_clock::duration span) noexcept
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(span).count());
}

/// The inverse of @ref toWireDuration: every int64_t count of nanoseconds converts, for the reason
/// fromWireTime() gives.
/// @param nanoseconds A length in nanoseconds, as a peer sent it.
/// @return It as a steady-clock duration.
[[nodiscard]] constexpr std::chrono::steady_clock::duration fromWireDuration(int64_t nanoseconds) noexcept
{
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds { nanoseconds });
}

} // namespace vthost
