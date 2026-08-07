// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A minimal async HTTP/1.1 server built on the coroutine socket layer. Parses a
/// request off an @c ISocket, invokes a handler, and writes the response back.
/// Handlers are plain `std::function`s, so the layer is fully testable against an
/// in-memory transport with no listener and no real socket.
///
/// Scope: `Content-Length` bodies only. Chunked transfer-encoding, keep-alive and
/// pipelining are deliberately absent — every response carries `Connection: close`
/// and the connection is dropped afterwards. Add them when a caller needs them
/// rather than speculatively.

#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coro/Task.hpp>
#include <net/IListener.hpp>
#include <net/ISocket.hpp>
#include <net/IoResult.hpp>

namespace net
{

/// Default cap on the request head (request line + headers, delimiter excluded).
/// A peer that never terminates its headers must not grow the buffer unbounded.
inline constexpr std::size_t DefaultMaxRequestHeadBytes = std::size_t { 64 } * 1024;

/// Default cap on a request body, independent of what `Content-Length` claims.
inline constexpr std::size_t DefaultMaxRequestBodyBytes = std::size_t { 8 } * 1024 * 1024;

/// A parsed HTTP request (the subset the server exposes to handlers).
struct HttpRequest
{
    std::string method;                                       ///< "GET", "POST", …
    std::string path;                                         ///< Request target, e.g. "/index".
    std::string version;                                      ///< "HTTP/1.1".
    std::vector<std::pair<std::string, std::string>> headers; ///< Header name/value pairs, in order.
    std::string body;                                         ///< Request body (may be empty).

    /// Looks up a header value (case-insensitive name match).
    /// @param name The header name to find.
    /// @return The first matching header's value, or "" if absent.
    [[nodiscard]] std::string header(std::string_view name) const;
};

/// @param status An HTTP status code.
/// @return The registered reason phrase for @p status, or "Unknown" if the code is
///         not one this server names. Never empty — a bare status line with an
///         empty reason phrase is malformed.
[[nodiscard]] std::string_view reasonPhrase(int status) noexcept;

/// An HTTP response a handler produces.
struct HttpResponse
{
    int status = 200;                                         ///< HTTP status code.
    std::string reason = "OK";                                ///< Reason phrase.
    std::vector<std::pair<std::string, std::string>> headers; ///< Extra headers (Content-Length is added).
    std::string body;                                         ///< Response body.

    /// @param text The body text.
    /// @return A 200 OK response with @p text as a text/plain body.
    [[nodiscard]] static HttpResponse ok(std::string text);

    /// @param status The status code; its reason phrase comes from @c reasonPhrase.
    /// @param text The body text.
    /// @return A response with the given status and a text/plain body.
    [[nodiscard]] static HttpResponse withStatus(int status, std::string text);
};

/// A request handler: maps a request to a response.
using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;

/// Limits applied while parsing a request. Grouped into a struct so a new limit is
/// a new field rather than another parameter at every call site.
struct HttpLimits
{
    std::size_t maxHeadBytes = DefaultMaxRequestHeadBytes; ///< Cap on request line + headers.
    std::size_t maxBodyBytes = DefaultMaxRequestBodyBytes; ///< Cap on the body.
};

/// Serves connections from @p listener until it is closed, dispatching each request
/// to @p handler. Connections are handled in sequence on the loop thread (a slow
/// handler stalls the accept loop), which suits the single-threaded model this
/// layer targets. Returns when the listener is closed or the flow is cancelled.
/// @param listener The bound listener to accept from (not owned).
/// @param handler The request handler.
/// @param limits Parsing limits applied to every request.
/// @return A task that completes when serving stops.
[[nodiscard]] coro::Task<void> serve(IListener* listener, HttpHandler handler, HttpLimits limits = {});

/// Reads and parses a single HTTP/1.1 request from @p socket. Exposed so the
/// framing can be tested without a listener.
/// @param socket The connection to read from (not owned).
/// @param limits Parsing limits.
/// @return The parsed request, or a @c NetError: @c Eof if the peer closed before a
///         complete request, @c MessageTooLarge if a limit was exceeded, or
///         @c Other for a malformed request line.
[[nodiscard]] coro::Task<std::expected<HttpRequest, NetError>> readRequest(ISocket* socket,
                                                                           HttpLimits limits = {});

/// Writes @p response to @p socket as an HTTP/1.1 response, adding `Content-Length`,
/// `Connection: close`, and a default `Content-Type` when the handler set none.
/// @param socket The connection to write to (not owned).
/// @param response The response to serialize (taken by value: a coroutine must not
///        hold a reference parameter across a suspend point).
/// @return The bytes written, or a @c NetError on failure.
[[nodiscard]] coro::Task<IoResult> writeResponse(ISocket* socket, HttpResponse response);

} // namespace net
