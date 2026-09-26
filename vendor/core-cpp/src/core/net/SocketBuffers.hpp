// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SocketBufferSizes` -- the kernel send and receive buffers a stream socket asks for.

#include <cstddef>
#include <optional>

namespace core::net
{

/// The kernel buffer sizes a stream socket asks for; each one left unset keeps the kernel's own.
///
/// Configured at construction -- @c ListenOptions::buffers for every socket a listener accepts,
/// @c DialOptions::buffers for one dial -- and applied before the connection exists: to the
/// listening socket before `listen`, whose accepted sockets inherit them, and to a dialled socket
/// before `connect`. The TCP window scale is announced in the handshake, and tcp(7) asks for the
/// sizes to be set first. fastcached sizes both
/// to 1 MiB so that a large reply leaves in one `sendmsg`; nothing here picks a size for anyone.
///
/// **A request, not a promise.** The kernel rounds it, may report more than was asked (Linux
/// reports twice the value set, for its bookkeeping), and caps an unprivileged request at
/// `net.core.wmem_max` / `net.core.rmem_max` on Linux. Setting a size also stops Linux from
/// autotuning that buffer. Best-effort, like the connected-socket options: a socket that refuses a
/// size is still bound or dialled rather than failing over a tuning option.
struct SocketBufferSizes
{
    /// `SO_SNDBUF`, in bytes; unset leaves the kernel's value untouched.
    std::optional<std::size_t> send {};

    /// `SO_RCVBUF`, in bytes; unset leaves the kernel's value untouched.
    std::optional<std::size_t> receive {};
};

} // namespace core::net
