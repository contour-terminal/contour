// SPDX-License-Identifier: Apache-2.0

/// @file
/// The POSIX socket-option rows, and the two `int`-option calls beneath them.
///
/// **The `#if`s below choose constants inside the data, never a branch in logic** (P13 ruling R11):
/// Linux doubles a buffer size it is given and reports the doubled value, the BSDs and macOS do
/// not, and macOS spells the keepalive idle time `TCP_KEEPALIVE` where the others say
/// `TCP_KEEPIDLE`.

#include <core/net/SocketOption.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <expected>
#include <span>

#include <netinet/in.h>
#include <netinet/tcp.h>

namespace core::net
{

namespace
{
#ifdef __linux__
    /// Linux stores twice the buffer size it is given, and `getsockopt` reports the doubled value
    /// (socket(7)).
    constexpr auto BufferScale = OptionScale::KernelDoubles;

    /// Linux caps a buffer at `rmem_max`/`wmem_max` itself, silently, so any `int` is taken.
    constexpr std::uint64_t BufferMaximum = INT_MAX;
#elifdef __APPLE__
    constexpr auto BufferScale = OptionScale::Identity;

    /// macOS refuses (`ENOBUFS`) a buffer above `kern.ipc.maxsockbuf` scaled by
    /// `MCLBYTES / (MSIZE + MCLBYTES)`: 7456540 bytes at the default 8 MiB. A documented row value
    /// rather than a runtime query, and conservative: 4 MiB stays under the default with room, at
    /// the price of clamping below a limit an administrator raised.
    constexpr std::uint64_t BufferMaximum = 4U * 1024U * 1024U;
#else
    constexpr auto BufferScale = OptionScale::Identity;

    /// FreeBSD and the other BSDs refuse above the same scaled `kern.ipc.maxsockbuf`, whose default
    /// is 2 MiB (1864135 bytes after scaling); 1 MiB stays under it, as macOS's row does.
    constexpr std::uint64_t BufferMaximum = 1024U * 1024U;
#endif

    /// The keepalive limits Linux enforces (`MAX_TCP_KEEPIDLE`, `MAX_TCP_KEEPINTVL`,
    /// `MAX_TCP_KEEPCNT`), which the BSDs and macOS take too: one bound for every POSIX system.
    constexpr std::uint64_t KeepAliveTimeMaximum = 32'767;
    constexpr std::uint64_t KeepAliveCountMaximum = 127;

    /// A hop limit is one octet on the wire.
    constexpr std::uint64_t HopLimitMaximum = 255;

#ifdef TCP_KEEPIDLE
    constexpr int KeepAliveIdle = TCP_KEEPIDLE;
#else
    constexpr int KeepAliveIdle = TCP_KEEPALIVE; // macOS: the same option, under its older name
#endif

    /// One row per option and family; the families differ only in the hop limit.
    constexpr auto Rows = std::array {
        // clang-format off
        SocketOptionRow { SocketOption::ReceiveBufferSize, AddressFamily::Ipv4, SOL_SOCKET,   SO_RCVBUF,         BufferScale, BufferMaximum },
        SocketOptionRow { SocketOption::SendBufferSize,    AddressFamily::Ipv4, SOL_SOCKET,   SO_SNDBUF,         BufferScale, BufferMaximum },
        SocketOptionRow { SocketOption::UnicastHopLimit,   AddressFamily::Ipv4, IPPROTO_IP,   IP_TTL,            OptionScale::Identity, HopLimitMaximum },
        SocketOptionRow { SocketOption::KeepAliveEnabled,  AddressFamily::Ipv4, SOL_SOCKET,   SO_KEEPALIVE,      OptionScale::Flag, 1 },
        SocketOptionRow { SocketOption::KeepAliveIdleTime, AddressFamily::Ipv4, IPPROTO_TCP,  KeepAliveIdle,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveInterval, AddressFamily::Ipv4, IPPROTO_TCP,  TCP_KEEPINTVL,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveCount,    AddressFamily::Ipv4, IPPROTO_TCP,  TCP_KEEPCNT,       OptionScale::Identity, KeepAliveCountMaximum },
        SocketOptionRow { SocketOption::ReceiveBufferSize, AddressFamily::Ipv6, SOL_SOCKET,   SO_RCVBUF,         BufferScale, BufferMaximum },
        SocketOptionRow { SocketOption::SendBufferSize,    AddressFamily::Ipv6, SOL_SOCKET,   SO_SNDBUF,         BufferScale, BufferMaximum },
        SocketOptionRow { SocketOption::UnicastHopLimit,   AddressFamily::Ipv6, IPPROTO_IPV6, IPV6_UNICAST_HOPS, OptionScale::Identity, HopLimitMaximum },
        SocketOptionRow { SocketOption::KeepAliveEnabled,  AddressFamily::Ipv6, SOL_SOCKET,   SO_KEEPALIVE,      OptionScale::Flag, 1 },
        SocketOptionRow { SocketOption::KeepAliveIdleTime, AddressFamily::Ipv6, IPPROTO_TCP,  KeepAliveIdle,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveInterval, AddressFamily::Ipv6, IPPROTO_TCP,  TCP_KEEPINTVL,     OptionScale::Seconds, KeepAliveTimeMaximum },
        SocketOptionRow { SocketOption::KeepAliveCount,    AddressFamily::Ipv6, IPPROTO_TCP,  TCP_KEEPCNT,       OptionScale::Identity, KeepAliveCountMaximum },
        // clang-format on
    };

    constexpr auto ReuseAddress =
        std::array { detail::NativeOption { .level = SOL_SOCKET, .name = SO_REUSEADDR } };
} // namespace

std::span<SocketOptionRow const> socketOptionTable() noexcept
{
    return Rows;
}

namespace detail
{
    std::span<NativeOption const> reuseAddressOptions() noexcept
    {
        return ReuseAddress;
    }

    std::expected<int, NetError> getIntOption(platform::NativeHandle socket, NativeOption option)
    {
        auto value = 0;
        auto length = static_cast<socklen_t>(sizeof(value));
        if (::getsockopt(socket, option.level, option.name, &value, &length) != 0)
            return std::unexpected(socketError(errno, "getsockopt"));
        return value;
    }

    std::expected<void, NetError> setIntOption(platform::NativeHandle socket, NativeOption option, int value)
    {
        if (::setsockopt(socket, option.level, option.name, &value, sizeof(value)) != 0)
            return std::unexpected(socketError(errno, "setsockopt"));
        return {};
    }
} // namespace detail

} // namespace core::net
