// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::readSocketOption` and `detail::writeSocketOption` -- the one interpreter of
/// @c socketOptionTable, used by every socket, listener and unconnected TCP socket that answers
/// @c SocketOption -- and the two OS calls beneath it, defined per platform beside the table.

#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>
#include <core/net/SocketOption.hpp>
#include <core/platform/Types.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <expected>
#include <span>

namespace core::net::detail
{

/// One `getsockopt`/`setsockopt` address, for an option outside the @c SocketOption vocabulary.
struct NativeOption
{
    int level = 0; ///< The option level.
    int name = 0;  ///< The option name at that level.
};

/// The options a TCP bind sets to 1 first, so that a port whose last connection is in TIME_WAIT
/// can be bound again.
///
/// `SO_REUSEADDR` on POSIX. **None on Windows** (P13 ruling R17, following WASI's `bind` and
/// wasmtime): Windows already allows that bind by default, and its `SO_REUSEADDR` would let the
/// socket take over a port another socket is listening on.
/// @return The rows, in static storage; possibly empty.
[[nodiscard]] CORE_NET_API std::span<NativeOption const> reuseAddressOptions() noexcept;

/// The kernel's value as the caller's, by the row's scale. A negative kernel value, which no option
/// here has, reads as 0 rather than wrapping.
/// @param scale The row's scale.
/// @param kernel What `getsockopt` answered.
/// @return The value in the row's unit.
[[nodiscard]] constexpr std::uint64_t fromKernel(OptionScale scale, int kernel) noexcept
{
    auto const value = static_cast<std::uint64_t>(std::max(kernel, 0));
    switch (scale)
    {
        case OptionScale::KernelDoubles: return value / 2;
        case OptionScale::Flag: return value != 0 ? 1 : 0;
        case OptionScale::Identity:
        case OptionScale::Seconds: break;
    }
    return value;
}

/// The caller's value as the kernel takes it: clamped to the row's maximum (and to `int`), whatever
/// the scale. A doubling kernel does the doubling itself, and a flag is set by any non-zero value,
/// which the clamp keeps non-zero.
/// @param row The option's row.
/// @param value The value in the row's unit.
/// @return What `setsockopt` is given.
[[nodiscard]] constexpr int toKernel(SocketOptionRow const& row, std::uint64_t value) noexcept
{
    return static_cast<int>(std::min({ value, row.maximum, std::uint64_t { INT_MAX } }));
}

/// `getsockopt` of an `int`-sized option.
/// @param socket The socket.
/// @param option Its level and name.
/// @return The value, or the classified OS error.
[[nodiscard]] CORE_NET_API std::expected<int, NetError> getIntOption(platform::NativeHandle socket,
                                                                     NativeOption option);

/// `setsockopt` of an `int`-sized option.
/// @param socket The socket.
/// @param option Its level and name.
/// @param value The value.
/// @return Nothing, or the classified OS error.
[[nodiscard]] std::expected<void, NetError> setIntOption(platform::NativeHandle socket,
                                                         NativeOption option,
                                                         int value);

/// Reads @p option from @p socket, a socket of @p family, through its row.
/// @param socket The socket.
/// @param family The socket's address family, which picks the row.
/// @param option The option.
/// @return The value in the row's unit; @c NetErrorCode::Unsupported where the platform has no
///         row; otherwise the classified OS error.
[[nodiscard]] std::expected<std::uint64_t, NetError> readSocketOption(platform::NativeHandle socket,
                                                                      AddressFamily family,
                                                                      SocketOption option);

/// Sets @p option on @p socket, a socket of @p family, through its row; a @p value above the
/// platform's `int` range is clamped to it.
/// @param socket The socket.
/// @param family The socket's address family, which picks the row.
/// @param option The option.
/// @param value The value in the row's unit.
/// @return Nothing; @c NetErrorCode::Unsupported where the platform has no row; otherwise the
///         classified OS error.
[[nodiscard]] std::expected<void, NetError> writeSocketOption(platform::NativeHandle socket,
                                                              AddressFamily family,
                                                              SocketOption option,
                                                              std::uint64_t value);

/// @c readSocketOption for a bound socket, whose family is asked of the kernel (`getsockname`):
/// a connection or a listener, which do not keep it.
/// @return As @c readSocketOption; @c NetErrorCode::Unsupported for a socket that is no IP one.
[[nodiscard]] std::expected<std::uint64_t, NetError> readSocketOption(platform::NativeHandle socket,
                                                                      SocketOption option);

/// @c writeSocketOption for a bound socket, as the overload above.
/// @return As @c writeSocketOption; @c NetErrorCode::Unsupported for a socket that is no IP one.
[[nodiscard]] std::expected<void, NetError> writeSocketOption(platform::NativeHandle socket,
                                                              SocketOption option,
                                                              std::uint64_t value);

} // namespace core::net::detail
