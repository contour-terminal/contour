// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::socketEndpoint` -- `getsockname` and `getpeername` into a @c ResolvedEndpoint, the one
/// place each platform asks a socket for an address. Defined per platform, beside the platform's
/// other socket calls (`posix/SocketsPosix.cpp`, `windows/SocketsWin32.cpp`). Private to `core::net`.

#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/platform/Types.hpp>

#include <cstdint>
#include <expected>

namespace core::net::detail
{

/// Which end of a socket an address is asked of.
enum class SocketSide : std::uint8_t
{
    Local,  ///< This end: `getsockname`.
    Remote, ///< The peer: `getpeername`.
};

/// Asks @p socket for the address of its @p side.
/// @param socket An open socket; the caller has already refused a closed one.
/// @param side Which end's address.
/// @return The endpoint, or the classified OS error of the call.
[[nodiscard]] std::expected<ResolvedEndpoint, NetError> socketEndpoint(platform::NativeHandle socket,
                                                                       SocketSide side);

} // namespace core::net::detail
