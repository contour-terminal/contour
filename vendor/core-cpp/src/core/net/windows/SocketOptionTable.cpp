// SPDX-License-Identifier: Apache-2.0

/// @file
/// The Windows socket-option rows, and the two `int`-option calls beneath them.
///
/// The keepalive intervals and count are the per-option `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and
/// `TCP_KEEPCNT` (Windows 10 1709 and later), not `SIO_KEEPALIVE_VALS`: that ioctl sets the flag and
/// both intervals together, and nothing reads them back. `armKeepAlive` keeps the ioctl, which is
/// its one-call contract.

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/SocketOption.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>

#include <array>
#include <climits>
#include <cstdint>
#include <expected>
#include <span>

namespace core::net
{

namespace
{
    /// Windows takes any `int` buffer size.
    constexpr std::uint64_t BufferMaximum = INT_MAX;

    /// Windows takes larger keepalive times and counts up to 255; these are Linux's limits, which
    /// a Windows build run under wine meets through the Linux kernel, and which no caller of a
    /// WASI socket can tell from a clamp.
    constexpr std::uint64_t KeepAliveTimeMaximum = 32'767;
    constexpr std::uint64_t KeepAliveCountMaximum = 127;

    /// A hop limit is one octet on the wire.
    constexpr std::uint64_t HopLimitMaximum = 255;

    /// One row per option and family; the families differ only in the hop limit. Windows reports
    /// buffer sizes as they were set.
    constexpr auto Rows = std::array {
        // clang-format off
        SocketOptionRow { SocketOption::ReceiveBufferSize, AddressFamily::Ipv4, SOL_SOCKET,   SO_RCVBUF,         OptionScale::Identity, BufferMaximum },
        SocketOptionRow { SocketOption::SendBufferSize,    AddressFamily::Ipv4, SOL_SOCKET,   SO_SNDBUF,         OptionScale::Identity, BufferMaximum },
        SocketOptionRow { SocketOption::UnicastHopLimit,   AddressFamily::Ipv4, IPPROTO_IP,   IP_TTL,            OptionScale::Identity, HopLimitMaximum },
        SocketOptionRow { SocketOption::KeepAliveEnabled,  AddressFamily::Ipv4, SOL_SOCKET,   SO_KEEPALIVE,      OptionScale::Flag, 1 },
        SocketOptionRow { SocketOption::KeepAliveIdleTime, AddressFamily::Ipv4, IPPROTO_TCP,  TCP_KEEPIDLE,      OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveInterval, AddressFamily::Ipv4, IPPROTO_TCP,  TCP_KEEPINTVL,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveCount,    AddressFamily::Ipv4, IPPROTO_TCP,  TCP_KEEPCNT,       OptionScale::Identity, KeepAliveCountMaximum },
        SocketOptionRow { SocketOption::ReceiveBufferSize, AddressFamily::Ipv6, SOL_SOCKET,   SO_RCVBUF,         OptionScale::Identity, BufferMaximum },
        SocketOptionRow { SocketOption::SendBufferSize,    AddressFamily::Ipv6, SOL_SOCKET,   SO_SNDBUF,         OptionScale::Identity, BufferMaximum },
        SocketOptionRow { SocketOption::UnicastHopLimit,   AddressFamily::Ipv6, IPPROTO_IPV6, IPV6_UNICAST_HOPS, OptionScale::Identity, HopLimitMaximum },
        SocketOptionRow { SocketOption::KeepAliveEnabled,  AddressFamily::Ipv6, SOL_SOCKET,   SO_KEEPALIVE,      OptionScale::Flag, 1 },
        SocketOptionRow { SocketOption::KeepAliveIdleTime, AddressFamily::Ipv6, IPPROTO_TCP,  TCP_KEEPIDLE,      OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveInterval, AddressFamily::Ipv6, IPPROTO_TCP,  TCP_KEEPINTVL,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveCount,    AddressFamily::Ipv6, IPPROTO_TCP,  TCP_KEEPCNT,       OptionScale::Identity, KeepAliveCountMaximum },
        // clang-format on
    };
} // namespace

std::span<SocketOptionRow const> socketOptionTable() noexcept
{
    return Rows;
}

namespace detail
{
    std::span<NativeOption const> reuseAddressOptions() noexcept
    {
        // None, by P13 ruling R17: Windows binds a port in TIME_WAIT by default, and its
        // SO_REUSEADDR would let this socket take over a port another socket listens on.
        return {};
    }

    std::expected<int, NetError> getIntOption(platform::NativeHandle socket, NativeOption option)
    {
        auto value = 0;
        auto length = static_cast<int>(sizeof(value));
        if (::getsockopt(reinterpret_cast<SOCKET>(socket),
                         option.level,
                         option.name,
                         reinterpret_cast<char*>(&value),
                         &length)
            != 0)
            return std::unexpected(socketError(lastSocketError(), "getsockopt"));
        return value;
    }

    std::expected<void, NetError> setIntOption(platform::NativeHandle socket, NativeOption option, int value)
    {
        if (::setsockopt(reinterpret_cast<SOCKET>(socket),
                         option.level,
                         option.name,
                         reinterpret_cast<char const*>(&value),
                         static_cast<int>(sizeof(value)))
            != 0)
            return std::unexpected(socketError(lastSocketError(), "setsockopt"));
        return {};
    }
} // namespace detail

} // namespace core::net
