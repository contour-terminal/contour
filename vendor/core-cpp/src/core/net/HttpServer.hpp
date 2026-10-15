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

#include <core/async/Task.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/LingeringClose.hpp>
#include <core/net/NetError.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::net
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

    /// How a connection refused over a request it did not finish reading is closed: through
    /// @c closeLingering, so the refusal is followed by a FIN rather than destroyed by a reset.
    ///
    /// A quarter of a second, 64 KiB, four reads -- not fastcached's two seconds, because
    /// fastcached lingers in a flow of the connection's own and @c serve does not: **every
    /// refused request holds this server's accept loop for up to `linger.total`**. A peer that
    /// reads the refusal and closes costs one round trip, which a quarter second covers well past
    /// a LAN; one that keeps sending, or never closes, meets the bound, and each such request
    /// stalls every later client for that long.
    LingerBounds linger { .total = std::chrono::milliseconds { 250 },
                          .maxBytes = std::size_t { 64 } * 1024,
                          .reads = 4 };
};

/// Where @c serve's accept loop reports what @c AcceptErrorPolicy decided (`AcceptPolicy.hpp`).
struct AcceptLoopReporting
{
    std::string surface = "http"; ///< What the loop serves, as its reports name it.
    /// Told each report: a failed accept the loop went on past (rate-limited), the loop becoming
    /// degraded on a long run of unclassified failures, recovering from one, and giving up on a dead
    /// listener. @c AcceptLoopHealth::record is written to be handed these. **When empty, each
    /// report's line goes to @c reportDiagnostic instead**, so a loop that degrades or gives up is
    /// never silent by default -- the shape of the incident `AcceptPolicy.hpp` opens with.
    std::function<void(AcceptLoopEvent const&)> onEvent;
};

/// Serves connections from @p listener until it is closed, dispatching each request
/// to @p handler. Connections are handled in sequence on the loop thread (a slow
/// handler stalls the accept loop), which suits the single-threaded model this
/// layer targets.
///
/// **A failed accept is answered by @c AcceptErrorPolicy, and only a closed or dead listener ends
/// the loop.** A connection that failed before it was handed over is accepted past; exhaustion
/// (@c NetErrorCode::ResourceExhausted: out of descriptors, buffers or memory) backs off on
/// @p loop's timer, up to @c AcceptErrorPolicy::MaxBackoff, and serves again; a failure nothing
/// classifies is backed off on too, for as long as it lasts, and a long run of them is reported as
/// the loop being degraded. Before 0.6.0 any failed accept ended the loop with the port left open,
/// so one `EMFILE` stopped a server for good while its port kept accepting handshakes into a
/// backlog nobody drained. A listener the loop finds dead (@c NetErrorCode::BadHandle) is CLOSED
/// before the loop reports giving up, so its port refuses rather than queues.
///
/// **When it returns.** At its next accept after the listener is closed, and AT ONCE when the close
/// lands while the loop is backing off: the backoff races @c IListener::closeToken. When the
/// listener is dead. And when the flow is cancelled, whether that finds the loop parked in an
/// accept or in a backoff -- in both it returns rather than throwing.
///
/// **The listener's lifetime.** @p listener must be alive when @c serve is first resumed. After
/// that its owner may close it and destroy it at any time on @p loop's thread, or with the loop
/// stopped: @c serve keeps the listener's close token, which outlives it and is fired by its
/// destructor, and asks it before every call into the listener, so it never touches one that is
/// gone. A listener destroyed while an accept is parked in it is the listener's own contract
/// (core-cpp's resolve that accept without touching themselves).
///
/// Each connection's @c ISocket::handshakeIfNeeded is awaited before its request is
/// read, and a connection whose handshake fails is dropped without an answer. **Neither
/// the handshake nor the request read has a deadline of its own**, so in this sequential
/// loop one client that connects and says nothing stalls every later connection; bound a
/// connection with @c ISocket::setReceiveDeadline on the accepted socket, or serve from
/// behind something that does.
///
/// **A refused connection closes lingering**: a request answered with a 413 or a 400 may
/// still have bytes the server never read, and a bare close over them is a reset that destroys
/// the refusal in flight ([core-cpp#35](https://github.com/contour-terminal/core-cpp/issues/35)).
/// So the refusal is followed by @c closeLingering, bounded by @c HttpLimits::linger, and **each
/// refused request holds the accept loop for up to `linger.total`** (250 ms by default) while it
/// does. A request that was read in full and answered is closed by its destructor, which is a FIN.
/// @param loop The loop @p listener runs on, never null: its timer waits out a backoff, and its
///        clock paces the reports (not owned). Another loop would resume the flow on its own
///        thread, away from the listener's.
/// @param listener The bound listener to accept from (not owned).
/// @param handler The request handler.
/// @param limits Parsing limits applied to every request.
/// @param reporting Where the accept loop's reports go.
/// @return A task that completes when serving stops.
[[nodiscard]] async::Task<void> serve(EventLoop* loop,
                                      IListener* listener,
                                      HttpHandler handler,
                                      HttpLimits limits = {},
                                      AcceptLoopReporting reporting = {});

/// Reads and parses a single HTTP/1.1 request from @p socket. Exposed so the
/// framing can be tested without a listener.
/// @param socket The connection to read from (not owned).
/// @param limits Parsing limits.
/// @return The parsed request, or a @c NetError: @c Eof if the peer closed before a
///         complete request, @c MessageTooLarge if a limit was exceeded, or
///         @c SystemError for a malformed request line.
[[nodiscard]] async::Task<std::expected<HttpRequest, NetError>> readRequest(ISocket* socket,
                                                                            HttpLimits limits = {});

/// Writes @p response to @p socket as an HTTP/1.1 response, adding `Content-Length`,
/// `Connection: close`, and a default `Content-Type` when the handler set none.
/// @param socket The connection to write to (not owned).
/// @param response The response to serialize (taken by value: a coroutine must not
///        hold a reference parameter across a suspend point).
/// @return The bytes written, or a @c NetError on failure.
[[nodiscard]] async::Task<IoResult> writeResponse(ISocket* socket, HttpResponse response);

} // namespace core::net
