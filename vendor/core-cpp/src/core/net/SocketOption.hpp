// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SocketOption` -- the options a TCP socket, a connection and a listener set and read back -- and
/// the per-platform table that says which `getsockopt`/`setsockopt` each one is.
///
/// **Behaviour is the table, and the code interprets it.** Each row names one option for one
/// address family: its level, its name, and how the kernel's number relates to the caller's. A
/// platform difference is a different row in that platform's `SocketOptionTable.cpp`, never a
/// branch: Linux's doubled buffer sizes are a row's @c OptionScale::KernelDoubles, the hop limit is
/// `IP_TTL` in one family's row and `IPV6_UNICAST_HOPS` in the other's.

#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetExport.hpp>

#include <cstdint>
#include <span>

namespace core::net
{

/// An option of a TCP socket that a caller sets and reads back.
///
/// Values are unsigned integers in the unit the option's row states: bytes for the buffers, hops
/// for the hop limit, whole seconds for the keepalive intervals, a count of probes, and 0 or 1 for
/// the keepalive flag.
enum class SocketOption : std::uint8_t
{
    ReceiveBufferSize, ///< The kernel receive buffer, in bytes, as the caller asked for it.
    SendBufferSize,    ///< The kernel send buffer, in bytes, likewise.
    UnicastHopLimit,   ///< `IP_TTL` for IPv4, `IPV6_UNICAST_HOPS` for IPv6.
    KeepAliveEnabled,  ///< Whether the socket probes a silent peer: 0 or 1.
    KeepAliveIdleTime, ///< Idle time before the first probe, in whole seconds.
    KeepAliveInterval, ///< Time between probes, in whole seconds.
    KeepAliveCount,    ///< Unanswered probes before the connection is declared dead.
    Last,              ///< The number of options; not an option.
};

/// How a row's kernel value relates to the caller's.
enum class OptionScale : std::uint8_t
{
    Identity,      ///< The kernel reports what was set.
    KernelDoubles, ///< The kernel stores and reports twice what was set (Linux's `SO_RCVBUF`, `SO_SNDBUF`).
    Seconds,       ///< Whole seconds, the kernel's unit on every platform; a caller converts to it.
    Flag,          ///< A boolean: any non-zero value sets it, and it reads back as 0 or 1, whatever bit
                   ///< the kernel reports it with (macOS answers `SO_KEEPALIVE` with the option's own value).
};

/// One option, for one address family, on one platform.
struct SocketOptionRow
{
    SocketOption option = SocketOption::Last;   ///< The option this row implements.
    AddressFamily family = AddressFamily::Ipv4; ///< The family of socket it applies to.
    int level = 0;                              ///< The `getsockopt`/`setsockopt` level.
    int name = 0;                               ///< The option name at that level.
    OptionScale scale = OptionScale::Identity;  ///< How the kernel's value relates to the caller's.

    /// The largest value, in the caller's unit, the kernel takes without refusing it. A larger one
    /// is clamped to this before it reaches the kernel, so setting an option never fails for being
    /// too large: Linux refuses a keepalive count above 127 and macOS a buffer above its
    /// `kern.ipc.maxsockbuf`, where WASI asks for a silent clamp.
    std::uint64_t maximum = 0;
};

/// The platform's option rows: exactly one per option and family.
/// @return The rows, in static storage.
[[nodiscard]] CORE_NET_API std::span<SocketOptionRow const> socketOptionTable() noexcept;

} // namespace core::net
