// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `probeHttpStatus` and `httpHealthProbe` — ask an HTTP endpoint for its status, on the calling
/// thread, without `curl`: what a container `HEALTHCHECK` or a `--healthcheck` entrypoint runs.
///
/// Origin: fastcached `src/FastCache/Net/HealthProbe.{hpp,cpp}`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), rewritten over @c BlockingConnector and
/// @c BlockingSocket rather than its own raw socket calls, so it inherits their SIGPIPE suppression,
/// close-on-exec and error classification instead of carrying a fourth copy of each.

#include <core/net/NetError.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

namespace core::net
{

/// How long a probe may take in each of its phases -- the dial, the request, the response --
/// unless the caller says otherwise: generous for loopback, bounded so a peer that accepts and then
/// never answers cannot hang a health check that has no timeout of its own around it.
inline constexpr std::chrono::milliseconds DefaultProbeTimeout { 3000 };

/// Connects to @p host : @p port, sends `GET <path> HTTP/1.0`, and reads the status code.
///
/// **The status line is parsed strictly** (`HTTP/1.x <3 digits>` then a space or the line end): a
/// search for `" 200 "` anywhere in the response would accept a 5xx page whose body mentions
/// "expected 200 OK" and report an unhealthy peer healthy.
/// @param host Target host, unbracketed. A view: nothing here outlives the call, since it blocks.
/// @param port Target TCP port.
/// @param path Request path, e.g. `/healthz`.
/// @param timeout Bound on the dial and on each send and receive.
/// @return The status code, or why none was read: the dial's or the socket's error, or
///         @c NetErrorCode::Unsupported for a response that is not an HTTP/1.x status line.
[[nodiscard]] std::expected<unsigned, NetError> probeHttpStatus(
    std::string_view host,
    std::uint16_t port,
    std::string_view path,
    std::chrono::milliseconds timeout = DefaultProbeTimeout);

/// @return Whether @p host : @p port answered @p path with status 200 -- @c probeHttpStatus
///         reduced to the one bit a health check exits on.
[[nodiscard]] bool httpHealthProbe(std::string_view host,
                                   std::uint16_t port,
                                   std::string_view path,
                                   std::chrono::milliseconds timeout = DefaultProbeTimeout);

} // namespace core::net
