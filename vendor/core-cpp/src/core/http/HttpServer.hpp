// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The keep-alive HTTP/1.1 server: an accept loop that serves each connection in a flow of its own,
/// and the per-connection exchange of request heads, streamed bodies and responses over a
/// @c Connection in the request role. The handler is an interface, one request in and one response
/// out, so `wasi:http/handler#handle` maps onto it one to one. Refusals map to a status through one
/// table (@c errorStatusTable).

#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpExport.hpp>
#include <core/http/Message.hpp>
#include <core/net/AcceptLoop.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAdmissionControl.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/LingeringClose.hpp>
#include <core/net/SocketAddress.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace core::http
{

/// One request as a handler receives it.
struct IncomingRequest
{
    RequestHead head; ///< The request head, as read.
    /// The body, streamed from the connection; a body of none reads as one empty @c BodyEnd::Reached
    /// chunk. With `Expect: 100-continue` the first @c IBodyReader::read sends the interim
    /// `100 Continue`. **The connection's next request waits for this reader to be destroyed**: a
    /// reader still alive once the response is written closes the connection, a read of it still
    /// parked then is retired first (it answers @c HttpErrorCode::ConnectionTerminated), and every read
    /// after that is refused the same way, since the request is over.
    std::unique_ptr<IBodyReader> body;
    std::optional<net::ResolvedEndpoint> peer; ///< The client's address, where the socket knows one.
};

/// One response as a handler produces it.
struct OutgoingResponse
{
    /// The final head: a status from 200 to 599, and no framing field (@c isFramingField), which the
    /// connection writes itself. A 1xx, or a head the connection refuses, is answered 500 instead.
    ResponseHead head;
    /// The body, or null for none; its shape picks the framing (@c framingFor). It may stream the
    /// request body into the response: the server writes it before it reads anything more.
    std::unique_ptr<IBodySource> body;
};

/// What a server calls once per request.
class IHttpHandler
{
  public:
    IHttpHandler() = default;
    virtual ~IHttpHandler() = default;
    IHttpHandler(IHttpHandler const&) = delete;
    IHttpHandler& operator=(IHttpHandler const&) = delete;
    IHttpHandler(IHttpHandler&&) = delete;
    IHttpHandler& operator=(IHttpHandler&&) = delete;

    /// Answers one request. Called on the server's loop, for one request of a connection at a time.
    /// @param request The request; its body is the handler's to read, or to leave.
    /// @return The response, or a failure: the server answers a failure, and an exception, with 500
    ///         and closes -- unless the failure came from reading the request body, which is then
    ///         answered with that refusal's status (@c errorStatusTable), or not at all where it has
    ///         none (the client went away).
    [[nodiscard]] virtual async::Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) = 0;
};

/// The limits a server holds each request to by default: @c HttpLimits with the body bounded to
/// 64 MiB, since a server's peer is untrusted and an unbounded body is the one limit the pure
/// layer leaves open.
/// @return The limits.
[[nodiscard]] inline HttpLimits defaultServerLimits()
{
    return HttpLimits { .maxBodyBytes = std::uint64_t { 64 } * 1024 * 1024 };
}

/// How a server serves its connections, fixed when it starts.
///
/// **Every wait on a client is bounded by default**, because a client that sends or reads slowly
/// holds a connection, and so a slot of @c maxConnections, for as long as it is let: a head by
/// @c headTimeout, the wait for one by @c idleTimeout, a body by @c bodyBetweenBytes and
/// @c bodyTotal, and every write by @c writeTimeout. Zero turns that bound off.
struct HttpServerOptions
{
    /// The bounds of every request read: head, field and trailer sizes, and the body.
    HttpLimits limits = defaultServerLimits();
    /// A request head, from its first byte to its last (@c WireTimeouts::head); past it the answer is
    /// 408. The wait for that first byte is @c idleTimeout's, a separate budget: a keep-alive client
    /// that idles almost to @c idleTimeout still has all of this for its head.
    std::chrono::milliseconds headTimeout { 10'000 };
    /// The wait for a request's first byte, on a new connection and between requests; past it the
    /// connection closes without an answer, as an idle one (RFC 9112 section 9.5).
    std::chrono::milliseconds idleTimeout { 5'000 };
    /// Between any two reads after a head's first byte, a body's included (a slowloris trickle).
    std::chrono::milliseconds bodyBetweenBytes { 10'000 };
    /// A request body as a whole, from the end of its head to its last byte, the server's drain of it
    /// included.
    std::chrono::milliseconds bodyTotal { 60'000 };
    /// Each write to the client, a head's or a body chunk's; a client that stops reading is closed
    /// past it.
    std::chrono::milliseconds writeTimeout { 30'000 };
    /// The requests one connection serves; the last is answered with `Connection: close`.
    std::size_t maxRequestsPerConnection = 1000;
    /// How many connections @c serveHttp serves at once: one it does not admit is closed unanswered,
    /// and reported (rate-limited) as an @c net::AcceptLoopEventKind::Warning. 512 by default; null is
    /// no cap. **Shared by every copy of these options**, so servers started from one copy share one
    /// cap; give each its own policy for a cap each.
    std::shared_ptr<net::IAdmissionControl> admission = std::make_shared<net::CountingAdmissionControl>(512);
    /// The most of a request body the server reads and discards so the connection can carry the next
    /// request, when the handler left it unread; a longer one closes the connection instead.
    std::uint64_t maxDrainBytes = std::uint64_t { 64 } * 1024;
    /// How a connection is closed: half-closed, then listened to within these bounds (as
    /// @c net::HttpLimits::linger, R30), so an answer is not destroyed by a reset.
    net::LingerBounds linger { .total = std::chrono::milliseconds { 250 },
                               .maxBytes = std::size_t { 64 } * 1024,
                               .reads = 4 };
    /// Where the accept loop reports what @c net::AcceptErrorPolicy decided.
    net::AcceptLoopReporting reporting {};
};

/// One row of @c errorStatusTable: the status a refusal is answered with.
struct ErrorStatusRow
{
    HttpErrorCode code;   ///< The refusal's code.
    RefusalReason reason; ///< Its reason; @c RefusalReason::Unclassified is the code's default row.
    std::uint16_t status; ///< The status that answers it.
};

/// The statuses a server answers a refused request with: 400, 408, 411, 413, 414, 431, 501, 505,
/// and 500 for an internal failure. A code without a row -- the client closing, a write failing --
/// is not answered: the connection closes. Neither is a read timeout before any byte of a request
/// (@c RefusalReason::NothingArrived), which is an idle connection, not a slow request.
/// @return The rows; a code's rows are looked up by reason first, then as Unclassified.
[[nodiscard]] CORE_HTTP_API std::span<ErrorStatusRow const> errorStatusTable() noexcept;

/// The row of @c errorStatusTable that answers @p error.
/// @param error A refusal of a request.
/// @return The row for its code and reason, else for its code, else none.
[[nodiscard]] CORE_HTTP_API std::optional<ErrorStatusRow> errorStatusFor(HttpError const& error) noexcept;

/// The reason phrase of a status this server writes itself (RFC 9110 section 15): `100 Continue` and
/// every status of @c errorStatusTable.
/// @param status A status code.
/// @return Its phrase, or empty for a status the server never writes.
[[nodiscard]] CORE_HTTP_API std::string_view statusPhrase(std::uint16_t status) noexcept;

/// Serves connections from @p listener until it is closed, each in a flow of its own on @p loop.
///
/// The accept loop is @c net::serve's (@c net::acceptLoop): a failed accept is answered by @c
/// net::AcceptErrorPolicy, reported through @c HttpServerOptions::reporting, and only a closed or dead
/// listener (or the flow's own cancellation) ends it. Each accepted socket is handed to @c
/// serveHttpConnection in a flow @c EventLoop::spawn starts, so a slow client holds its own connection and
/// nothing else; one
/// @c HttpServerOptions::admission does not admit is closed at once.
///
/// **Connections outlive the loop that accepted them**: closing the listener ends the accept loop,
/// not the connections it started, which end with their client, their deadlines, or the loop.
/// @param loop The loop the listener and every connection run on (not owned; outlives every
///        connection).
/// @param listener The bound listener (not owned); alive when the flow is first resumed. Its owner
///        may close and destroy it afterwards, as with @c net::serve.
/// @param handler The handler (not owned; outlives every connection).
/// @param options The configuration.
/// @return A task that completes when the accept loop ends.
[[nodiscard]] CORE_HTTP_API async::Task<void> serveHttp(net::EventLoop* loop,
                                                        net::IListener* listener,
                                                        IHttpHandler* handler,
                                                        HttpServerOptions options = {});

/// Serves one connection: request after request, until either end closes it.
///
/// Per request: the wait for the head's first byte, held to @c HttpServerOptions::idleTimeout; the
/// head from that byte on, held to @c headTimeout; its framing (@c requestFraming); the handler; the
/// response, framed by @c framingFor; then, once the handler's body reader is destroyed, a drain of
/// what it left unread, up to @c maxDrainBytes. A refused head or framing is answered by
/// @c errorStatusTable without calling the handler, and the connection closes. Every close is
/// lingering (@c net::closeLingering).
///
/// **Pipelined requests are served in order, one at a time**: request N+1 is read only once
/// response N is written in full, as @c Connection's request role requires, so a client that sends
/// several at once gets their answers in order and the handler never sees two of one connection at
/// a time. HTTP/1.1 permits this (RFC 9112 section 9.3.2), and a WASI handler serves one request per
/// connection at a time.
///
/// **The connection closes, with `Connection: close` where the response can still say so**, after:
/// a request that asked for it, or HTTP/1.0 without keep-alive; the @c maxRequestsPerConnection-th
/// request; a body longer than @c maxDrainBytes left unread; `Expect: 100-continue` answered without
/// the handler reading (the client may or may not send the body); a body reader still alive when the
/// response is written; and any failure.
/// @param loop The loop the socket runs on (not owned; outlives the flow).
/// @param socket The accepted socket, owned from now on; its handshake is awaited first, within
///        @c headTimeout.
/// @param handler The handler (not owned; outlives the flow).
/// @param options The configuration; @c reporting and @c admission are the accept loop's.
/// @return A task that completes once the connection is closed.
[[nodiscard]] CORE_HTTP_API async::Task<void> serveHttpConnection(net::EventLoop* loop,
                                                                  std::unique_ptr<net::ISocket> socket,
                                                                  IHttpHandler* handler,
                                                                  HttpServerOptions options);

} // namespace core::http
