// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `HttpClient` -- HTTP/1.1 requests over kept-alive connections: the dial through an injected
/// @c net::IConnector, TLS through an injected @c net::ITlsContext, the exchange through a
/// @c Connection, a bounded pool of idle connections per origin, and every failure on the way
/// classified as an @c HttpErrorCode.

#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpExport.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ITlsContext.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace core::http
{

namespace detail
{
    struct Pool;
} // namespace detail

/// Whether a request is sent in the clear or over TLS.
enum class Scheme : std::uint8_t
{
    Http,  ///< Plain TCP.
    Https, ///< TLS, through the client's @c net::ITlsContext.
};

/// @param scheme A scheme.
/// @return Its default port: 80, or 443 for @c Scheme::Https.
[[nodiscard]] constexpr std::uint16_t defaultPort(Scheme scheme) noexcept
{
    return scheme == Scheme::Https ? std::uint16_t { 443 } : std::uint16_t { 80 };
}

/// Where a request goes: the scheme, the host and the port, which together name one origin.
struct Origin
{
    Scheme scheme = Scheme::Http; ///< In the clear or over TLS.
    /// A DNS name, an IPv4 literal, or an IPv6 literal with or without the brackets a URL authority
    /// puts around it: the dial gets it without them, the Host field with. A host with a `:` is
    /// taken for an IPv6 literal and must be one, so `host:port` is refused; the port goes in
    /// @c port. **An IPv6 zone (`fe80::1%eth0`) is refused** (@c HttpErrorCode::RequestUriInvalid): a
    /// zone is meaningful only on the host that names it, and RFC 6874 section 4 keeps it out of
    /// what is sent, so a caller strips it and dials through a connector that knows the interface.
    std::string host;
    /// The TCP port, or none for the scheme's default (80, or 443 for @c Scheme::Https). The Host
    /// field names it unless it is that default.
    std::optional<std::uint16_t> port;

    /// @return @c port, or the scheme's default when it is none.
    [[nodiscard]] std::uint16_t effectivePort() const noexcept { return port.value_or(defaultPort(scheme)); }

    auto operator<=>(Origin const&) const = default;
};

/// The deadlines of one request, on the client loop's clock.
///
/// **Each defaults to 600 seconds**, as `wasi:http`'s do in wasmtime, so a client built with no
/// options still bounds every wait. **A zero duration means no deadline** (R31): the client bounds
/// that wait by nothing, rather than by a deadline that fires at once.
struct HttpTimeouts
{
    /// What each deadline is unless set: wasmtime's `wasi:http` default.
    static constexpr auto DefaultTimeout = std::chrono::milliseconds { std::chrono::seconds { 600 } };

    /// The dial, name resolution included (@c net::DialOptions::connectTimeout), and then the TLS
    /// handshake, each on its own: @c HttpErrorCode::ConnectionTimeout. A dial that runs out of it
    /// while still resolving the name is a ConnectionTimeout too, never @c HttpErrorCode::DnsTimeout:
    /// @c net::IConnector reports both as @c net::NetErrorCode::Timeout and tells them apart only in
    /// words. **Bounding the handshake is this client's own rule** -- wasmtime leaves the TLS connect
    /// unbounded -- so an https request may take up to twice this before its first request byte.
    /// Zero leaves the dial to the platform's default, which may be minutes, and the handshake
    /// unbounded.
    std::chrono::milliseconds connect { DefaultTimeout };
    /// From the start of the exchange -- the request head's write and the response head's read start
    /// together, once the connection is up -- to the response's first byte:
    /// @c HttpErrorCode::ResponseTimeout. **The upload counts toward it**, as `wasi:http`'s
    /// first-byte timeout does in wasmtime, so a body slower than this fails the request, and a server
    /// that never reads the body is bounded by it. Restarted for each interim (1xx) response.
    std::chrono::milliseconds firstByte { DefaultTimeout };
    /// Between any two reads after the first, the response body's included:
    /// @c HttpErrorCode::ConnectionReadTimeout.
    std::chrono::milliseconds betweenBytes { DefaultTimeout };
};

/// How an @c HttpClient is configured, fixed at construction.
struct HttpClientOptions
{
    /// The most idle connections kept per origin for reuse; a connection that comes back past it is
    /// closed. Zero keeps none.
    std::size_t maxIdlePerOrigin = 4;
    /// The most connections open to one origin at once, idle ones included. A request that finds
    /// every one of them lent out is @c HttpErrorCode::ConnectionLimitReached, at once and without a
    /// dial; one that finds an idle connection it cannot use closes that to make room.
    std::size_t maxConnectionsPerOrigin = 16;
    /// The most connections open at once to all origins together, idle ones included: the bound on
    /// the file descriptors the client holds, however many ports or names it is sent to. A request
    /// past it closes the idle connection that came back first, of any origin, to make room; with
    /// none idle it is @c HttpErrorCode::ConnectionLimitReached, at once and without a dial. It does
    /// not wait for a connection to come free: WASI's outgoing handler has no queue, and a refusal the
    /// caller sees beats a wait the caller cannot bound.
    std::size_t maxConnectionsTotal = 256;
    /// How long an idle connection is kept, on the loop's clock. One past it is never reused, and is
    /// closed by the next request to any origin (there is no timer of its own). **Zero means no
    /// deadline** (R31): idle connections are then bounded by @c maxIdlePerOrigin alone.
    std::chrono::milliseconds idleTimeout { 30'000 };
    /// The bounds every response is held to (@c Connection).
    HttpLimits limits {};
    /// The deadlines of a request that names none of its own.
    HttpTimeouts timeouts {};
};

/// One request to send.
struct OutgoingRequest
{
    Origin origin; ///< Where it goes.
    /// The method, the target, the version and the caller's fields. The client writes `Host` from
    /// @c origin, and @c Connection the framing fields from @c body; a caller's `Host` or framing
    /// field (@c isFramingField) is refused before the dial.
    RequestHead head;
    std::unique_ptr<IBodySource>
        body; ///< The body, or null for none; its shape picks the framing (@c framingFor).
    std::optional<HttpTimeouts> timeouts; ///< This request's deadlines, or none for the client's.
};

/// The answer to a request: the final response's head, and its body to read.
struct IncomingResponse
{
    ResponseHead head; ///< The final (non-1xx) response's head.
    /// The body, framed by the head. It holds the connection until the body's end: read to
    /// @c BodyEnd::Reached on a connection that stays @c Reuse::Yes, the connection goes back to the
    /// client's pool (a bodiless response's at once); dropped before that, failed, or outliving the
    /// client, it closes the connection. Its trailers stay readable after the connection has gone
    /// back. Loop-affine, like the client: read and drop it on the client's loop.
    std::unique_ptr<IBodyReader> body;
};

/// Sends HTTP/1.1 requests over connections it keeps alive between them, one request at a time on
/// each (no pipelining).
///
/// A request runs in four steps, and each step's failures have a code of their own:
///
/// 1. **Checked before the dial**: an empty or malformed host is @c HttpErrorCode::RequestUriInvalid;
///    a caller's `Host` or framing field is @c HttpErrorCode::ProtocolError; @c Scheme::Https without
///    a TLS context is @c HttpErrorCode::ConfigurationError. Nothing is dialled for any of them.
/// 2. **A connection**: an idle one to the same origin (scheme, host case-insensitively, effective
///    port) opened under the same read deadlines, the most recently used first; else, within
///    @c HttpClientOptions::maxConnectionsPerOrigin (@c HttpErrorCode::ConnectionLimitReached past
///    it, and past @c HttpClientOptions::maxConnectionsTotal over all origins with no idle connection
///    to close for room), a dial through the connector, bounded by @c HttpTimeouts::connect. A name that does
///    not resolve, temporarily or for good, is @c HttpErrorCode::DnsError; a refusal
///    @c HttpErrorCode::ConnectionRefused; an unreachable host @c HttpErrorCode::DestinationUnavailable;
///    the deadline @c HttpErrorCode::ConnectionTimeout.
/// 3. **TLS**, for @c Scheme::Https: @c net::ITlsContext::wrapClient binds the connection to the
///    origin's host, then the handshake runs at once, bounded by @c HttpTimeouts::connect again, before
///    any request byte. A context that cannot verify a per-connection name -- every context from the
///    legacy @c makeTlsClientContext(caPem, expectedHostName, …), trust-on-first-use included (R23) --
///    is @c HttpErrorCode::ConfigurationError, never a connection that is not verified for the host. A
///    refused handshake is the @c net::TlsFailure's @c TlsCertificateError, @c TlsProtocolError or
///    @c TlsAlertReceived, with the failure attached (@c HttpError::tls).
/// 4. **The exchange**: the request head and body are written while the response head is read
///    (@c async::whenAll), the response timeout running from the start. Interim (1xx)
///    responses are skipped, up to 32 of them, and a 33rd is @c HttpErrorCode::ProtocolError; a 101
///    is too, since no request asks to upgrade. A failed read closes the connection and stops the write, so a
///    write parked on the socket or on a body source that honours its flow's stop token ends at once, and
///    answers the read's failure. A failed write answers its own failure, closing first, unless the peer cut
///    the connection (@c HttpErrorCode::ConnectionTerminated): then the read goes on, so a server that
///    refuses an upload and closes is still heard (a 413). Any failure on a connection whose TLS
///    session reports a @c net::TlsFailure is that failure's code.
///
/// **A stale pooled connection is retried once.** A server may close a kept-alive connection while
/// it sits idle, and the client learns of it only by using it. When a connection from the pool fails
/// before the first response head arrived (no interim response either) with a transport failure --
/// @c HttpErrorCode::ConnectionTerminated for the server's FIN or a reset, or
/// @c HttpErrorCode::InternalError for the OS error a write after that FIN draws (EPIPE,
/// WSAECONNABORTED), reported by the write or by the read that follows it, which the kernel and the
/// timing choose between -- the request is sent again on
/// a freshly dialled connection, but only when that cannot repeat an effect: its method is
/// idempotent (RFC 9110 section 9.2.2: GET, HEAD, OPTIONS, TRACE, PUT, DELETE) and it hands over no
/// body, its source null or of known length zero without trailers. Every other failure, and any
/// failure of a connection dialled for the request (the retry's included), is the answer. A reset
/// that cuts a pooled connection inside the first head counts as before it; RFC 9110 lets an
/// idempotent request be retried after any failure before its response is read.
///
/// The request completes once its body is written and the final head is read. Loop-affine:
/// construct it, call @c send and await the result, and read and drop every response body on the
/// loop's thread. Destroying the client closes its idle connections; a response body still held
/// keeps its own connection and closes it at its end.
class CORE_HTTP_API HttpClient
{
  public:
    /// @param loop The loop every connection is pinned to, and whose clock the deadlines are measured
    ///        on (not owned; outlives this, every request and every response body).
    /// @param connector How connections are dialled (not owned; outlives this). Its sockets must be
    ///        pinned to @p loop.
    /// @param tls The client context for @c Scheme::Https (not owned; outlives this), or null for a
    ///        client that sends in the clear only.
    /// @param options The configuration.
    HttpClient(net::EventLoop& loop,
               net::IConnector& connector,
               net::ITlsContext* tls,
               HttpClientOptions options = {});
    ~HttpClient();
    HttpClient(HttpClient const&) = delete;
    HttpClient& operator=(HttpClient const&) = delete;
    /// Moves the client, its pool included. **A moved-from client may only be destroyed or assigned
    /// to**: @c send and @c idleConnections on it are a contract violation (`Require`). **Neither
    /// client may be moved while a @c send of it is running**: the running request holds the client
    /// it was started on. Response bodies already handed out are unaffected; they hold the pool, not
    /// the client.
    HttpClient(HttpClient&&) noexcept;
    /// @copydoc HttpClient(HttpClient&&)
    HttpClient& operator=(HttpClient&&) noexcept;

    /// Sends @p request and reads the final response's head.
    ///
    /// **It completes only once the whole request body is written**, the final head read or not.
    /// Three consequences follow, until the head is delivered before the upload ends (a carry to
    /// P14):
    /// - An early final response (a 413 to an upload) is not returned while the server stops
    ///   reading the rest: the send waits for the write, bounded only by @c HttpTimeouts::firstByte
    ///   if no response byte came, and otherwise by the caller's stop token.
    /// - A read failure stops a write parked on a body source only if the source honours its flow's
    ///   stop token; one that ignores it delays the answer until it hands over its next chunk.
    /// - Full duplex deadlocks: a body source that waits on the response body never gets it, since
    ///   the response is returned only after the body source has finished.
    /// @param request The request, owned by the call.
    /// @return The response, its body still to read, or the failure, classified as the class says.
    /// @throws async::OperationCancelled if the awaiting flow's stop token is stopped while it runs.
    [[nodiscard]] async::Task<HttpResult<IncomingResponse>> send(OutgoingRequest request);

    /// @return How many idle connections the client holds for reuse, every origin's, not counting
    ///         one past @c HttpClientOptions::idleTimeout.
    [[nodiscard]] std::size_t idleConnections() const noexcept;

  private:
    net::EventLoop* _loop;
    net::IConnector* _connector;
    net::ITlsContext* _tls;
    HttpClientOptions _options;
    /// The idle connections and the count lent out, per origin; shared so that a response body can
    /// give its connection back, and hold it only weakly.
    std::shared_ptr<detail::Pool> _pool;
};

} // namespace core::http
