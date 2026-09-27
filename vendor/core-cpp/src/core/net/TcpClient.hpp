// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The client half of a TCP conversation: dial, then send and receive whole messages.
///
/// **There is one TCP client, and this is it** (`.agent/rules/async-and-net.md`). fastcached grew
/// three answers to this job, and the one nobody maintained is the one that broke: it resolved no
/// hostnames, had no bounds, no SIGPIPE protection, and did not compile on POSIX at all
/// ([fastcached#84](https://github.com/LASTRADA-Software/fastcached/issues/84)).
///
/// Origin: fastcached `src/FastCache/Net/TcpClient.{hpp,cpp}`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core::net
{

/// Dials @p host : @p port on the calling thread and hands back a socket bounded in both
/// directions.
///
/// A convenience over @c BlockingConnector for the common case, not a replacement for it: a caller
/// that needs to inject a resolver or a clock uses the connector directly. **Never suspends**, so a
/// synchronous caller drives it with `core::async::syncRun`, as it drives @c sendAll and
/// @c receiveExactly over the socket this hands back.
///
/// Splitting a `"host:port"` string is deliberately NOT here: that is a grammar -- `rfind(':')`
/// finds the wrong colon in `[::1]:7000` -- and a caller does it before calling.
/// @param host Hostname or literal address, unbracketed. By value, for the coroutine-frame reason
///        @c IConnector::connect records.
/// @param port TCP port in host byte order.
/// @param connectTimeout The whole dial's budget; non-positive leaves the platform default, which
///        can run to minutes.
/// @param ioTimeout How long each later send or receive may block; non-positive leaves it unbounded.
/// @return The connected socket, or why the attempt did not succeed.
[[nodiscard]] async::Task<SocketResult> connectTcp(std::string host,
                                                   std::uint16_t port,
                                                   std::chrono::milliseconds connectTimeout,
                                                   std::chrono::milliseconds ioTimeout);

/// Writes every byte of @p bytes, looping over partial writes.
///
/// **"Keep going until the buffer is done, and say whether it finished" is one rule**, and it had
/// been written out separately at each caller upstream. Sound under `syncRun` over a blocking
/// socket, which answers every awaitable inline; over a loop socket it must be AWAITED by another
/// coroutine, because there the awaitable really does suspend.
/// @param socket The connected socket; must not be null. A pointer, because a coroutine parameter
///        must not be a reference: the frame outlives the call expression.
/// @param bytes The payload; must outlive the task, which is the socket contract's rule.
/// @return True when every byte was written; false on any error, or on a write that took nothing.
[[nodiscard]] async::Task<bool> sendAll(ISocket* socket, std::span<std::byte const> bytes);

/// Reads exactly @p count bytes, looping over partial reads.
/// @param socket The connected socket; must not be null.
/// @param count How many bytes are required. Zero yields an empty vector WITHOUT touching the
///        socket: a zero-length read would answer `0`, which on this interface means the peer
///        finished sending, so a zero-length payload would read as a peer that closed.
/// @return The bytes, or nullopt if the peer closed or failed before they all arrived.
[[nodiscard]] async::Task<std::optional<std::vector<std::byte>>> receiveExactly(ISocket* socket,
                                                                                std::size_t count);

} // namespace core::net
