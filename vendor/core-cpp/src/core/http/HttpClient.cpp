// SPDX-License-Identifier: Apache-2.0
#include <core/http/HttpClient.hpp>

#include <core/Assert.hpp>
#include <core/Clock.hpp>
#include <core/async/AsTask.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/http/Connection.hpp>
#include <core/http/detail/Syntax.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/WithTimeout.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace core::http
{

namespace
{

    using namespace std::chrono_literals;

    // ---------------------------------------------------------------------------------------------
    // Failures before the exchange, classified once: a row per code, each table with its default.

    struct NetCodeRow
    {
        net::NetErrorCode net;
        HttpErrorCode http;
    };

    /// A failed dial, or a handshake the transport cut (no TLS failure behind it). Every code not
    /// listed is @c HttpErrorCode::InternalError, with the socket's words as detail.
    constexpr auto TransportErrorRows = std::array {
        NetCodeRow { .net = net::NetErrorCode::NameNotFound, .http = HttpErrorCode::DnsError },
        NetCodeRow { .net = net::NetErrorCode::ResolverTemporaryFailure, .http = HttpErrorCode::DnsError },
        // A permanent resolver failure, and any lookup failure nothing classified further.
        NetCodeRow { .net = net::NetErrorCode::AddressError, .http = HttpErrorCode::DnsError },
        NetCodeRow { .net = net::NetErrorCode::ConnRefused, .http = HttpErrorCode::ConnectionRefused },
        NetCodeRow { .net = net::NetErrorCode::HostUnreach, .http = HttpErrorCode::DestinationUnavailable },
        NetCodeRow { .net = net::NetErrorCode::Timeout, .http = HttpErrorCode::ConnectionTimeout },
        NetCodeRow { .net = net::NetErrorCode::ConnReset, .http = HttpErrorCode::ConnectionTerminated },
        NetCodeRow { .net = net::NetErrorCode::Eof, .http = HttpErrorCode::ConnectionTerminated },
    };

    /// @c net::ITlsContext::wrapClient's refusals. Every code not listed is
    /// @c HttpErrorCode::InternalError.
    constexpr auto WrapErrorRows = std::array {
        // The context cannot verify a per-connection name: a legacy or trust-on-first-use one (R23).
        NetCodeRow { .net = net::NetErrorCode::Unsupported, .http = HttpErrorCode::ConfigurationError },
        // A name the wire cannot carry: the origin's host is no host.
        NetCodeRow { .net = net::NetErrorCode::AddressError, .http = HttpErrorCode::RequestUriInvalid },
    };

    [[nodiscard]] HttpError classify(std::span<NetCodeRow const> rows, net::NetError const& error)
    {
        auto const row = std::ranges::find(rows, error.code, &NetCodeRow::net);
        return detail::refusal(row != rows.end() ? row->http : HttpErrorCode::InternalError,
                               error.toString());
    }

    struct TlsFailureRow
    {
        net::TlsFailureKind kind;
        HttpErrorCode http;
    };

    /// What a TLS session's failure is, by its kind; one row per @c net::TlsFailureKind.
    constexpr auto TlsFailureRows = std::array {
        TlsFailureRow { .kind = net::TlsFailureKind::Protocol, .http = HttpErrorCode::TlsProtocolError },
        TlsFailureRow { .kind = net::TlsFailureKind::Certificate,
                        .http = HttpErrorCode::TlsCertificateError },
        TlsFailureRow { .kind = net::TlsFailureKind::AlertReceived, .http = HttpErrorCode::TlsAlertReceived },
    };

    /// @return The TLS failure behind @p socket's connection as an error with it attached, if its
    ///         session reports one.
    [[nodiscard]] std::optional<HttpError> tlsFailureOf(net::ISocket const& socket)
    {
        auto const* session = socket.tlsSession();
        if (session == nullptr)
            return std::nullopt;
        auto failure = session->failure();
        if (!failure.has_value())
            return std::nullopt;
        auto const row = std::ranges::find(TlsFailureRows, failure->kind, &TlsFailureRow::kind);
        auto error = detail::refusal(
            row != TlsFailureRows.end() ? row->http : HttpErrorCode::TlsProtocolError, failure->message);
        error.tls = std::move(*failure);
        return error;
    }

    // ---------------------------------------------------------------------------------------------
    // The origin as the dial, TLS and the Host field each want it.

    /// The host without the brackets an IPv6 literal may come with: what the dial and TLS take.
    [[nodiscard]] std::string_view bareHost(std::string_view host) noexcept
    {
        if (host.size() >= 2 && host.starts_with('[') && host.ends_with(']'))
            return host.substr(1, host.size() - 2);
        return host;
    }

    /// Whether @p host can only be read one way: a host with a `:` is an IPv6 literal, so it may hold
    /// nothing but hex digits, `:` and the `.` of an embedded IPv4 address, and at least two `:`.
    /// That refuses `host:port` (the port belongs in @c Origin::port) and a zone (`%eth0`, RFC 6874
    /// section 4). Anything else is left to the Host field's authority check.
    [[nodiscard]] bool isHostSyntax(std::string_view host) noexcept
    {
        if (!host.contains(':'))
            return true;
        auto const ipv6Octet = [](char octet) {
            return detail::allIn(std::string_view { &octet, 1 }, detail::CharClass::Hex) || octet == ':'
                   || octet == '.';
        };
        return std::ranges::all_of(host, ipv6Octet) && std::ranges::count(host, ':') >= 2;
    }

    /// The Host field's value (RFC 9110 section 7.2): the host, bracketed if it is an IPv6 literal,
    /// and the port unless it is the scheme's default.
    [[nodiscard]] std::string authorityOf(Origin const& origin)
    {
        auto const host = bareHost(origin.host);
        auto authority = host.contains(':') ? std::format("[{}]", host) : std::string { host };
        if (origin.effectivePort() != defaultPort(origin.scheme))
            authority += std::format(":{}", origin.effectivePort());
        return authority;
    }

    /// The request head as written: `Host` first, then the caller's fields, which may hold neither a
    /// `Host` nor a framing field.
    [[nodiscard]] HttpResult<RequestHead> headFor(Origin const& origin, RequestHead head)
    {
        if (bareHost(origin.host).empty())
            return std::unexpected(
                detail::refusal(HttpErrorCode::RequestUriInvalid, "the origin has no host"));
        if (!isHostSyntax(bareHost(origin.host)))
            return std::unexpected(detail::refusal(HttpErrorCode::RequestUriInvalid,
                                                   std::format("'{}' is not a host", origin.host)));
        auto const authority = authorityOf(origin);
        if (!detail::isAuthority(authority, detail::PortRule::Optional))
            return std::unexpected(detail::refusal(HttpErrorCode::RequestUriInvalid,
                                                   std::format("'{}' is not a host", origin.host)));
        auto fields = Fields {};
        if (!fields.append("Host", authority).has_value())
            return std::unexpected(detail::refusal(HttpErrorCode::RequestUriInvalid,
                                                   std::format("'{}' is not a host", origin.host)));
        for (auto const& field: head.fields.entries())
        {
            if (isFramingField(field.name) || detail::equalsIgnoringCase(field.name, "Host"))
                return std::unexpected(detail::refusal(
                    HttpErrorCode::ProtocolError, "the client writes this field itself", field.name));
            // Validated when the caller appended it.
            std::ignore = fields.append(field.name, field.value);
        }
        head.fields = std::move(fields);
        return head;
    }

    // ---------------------------------------------------------------------------------------------
    // The exchange: the request written while the response head is read.

    /// Whether a failed request write still leaves the response worth reading: only when the peer
    /// cut the connection, which a server that refuses an upload does after answering it.
    [[nodiscard]] bool wasCutByPeer(HttpError const& error) noexcept
    {
        return error.code == HttpErrorCode::ConnectionTerminated;
    }

    /// Writes the request head and its body. A failure the peer did not cause closes the connection,
    /// so a head read still waiting on the peer ends too.
    async::Task<void> writeRequest(Connection* connection,
                                   RequestHead head,
                                   IBodySource* body,
                                   Framing framing,
                                   std::optional<HttpResult<void>>* out)
    {
        auto wrote = co_await connection->writeRequestHead(std::move(head), framing);
        if (wrote.has_value())
            wrote = co_await connection->writeBody(body, framing);
        if (!wrote.has_value() && !wasCutByPeer(wrote.error()))
            connection->close();
        *out = std::move(wrote);
    }

    /// Closed by the read half when it fails, which stops the write half: never pushed to.
    using ReadFailure = async::AsyncQueue<std::uint8_t>;

    /// Returns once @p readFailure is closed; stopped (OperationCancelled) otherwise.
    async::Task<void> untilClosed(ReadFailure* readFailure)
    {
        std::ignore = co_await readFailure->pop();
    }

    /// The write half: @c writeRequest, stopped when the read half fails. A write parked on the socket
    /// ends with the close the read half makes; one parked on a body source ends here, through its
    /// flow's stop token, if the source honours it. A stopped write leaves @p out empty.
    async::Task<void> writeUnlessReadFails(Connection* connection,
                                           RequestHead head,
                                           IBodySource* body,
                                           Framing framing,
                                           std::optional<HttpResult<void>>* out,
                                           ReadFailure* readFailure)
    {
        auto arms = std::vector<async::Task<void>> {};
        arms.push_back(writeRequest(connection, std::move(head), body, framing, out));
        arms.push_back(untilClosed(readFailure));
        std::ignore = co_await async::whenAny(std::move(arms));
    }

    /// Whether a response had begun when its exchange failed: an interim head counts, since the server
    /// had then read the request.
    enum class ResponseSeen : std::uint8_t
    {
        None, ///< Not a head of it arrived.
        Some, ///< An interim head did.
    };

    /// What the read half answers.
    struct ReadAnswer
    {
        std::optional<HttpResult<IncomingResponse>> response;
        ResponseSeen seen = ResponseSeen::None;
    };

    /// The most interim (1xx) responses read before the final one; one more is a ProtocolError. Each
    /// costs a read and nothing else, so without it a server could keep a request reading for ever.
    constexpr auto MaxInterimResponses = 32;

    /// Reads response heads until the final one, and frames its body. A failure closes the connection
    /// and @p readFailure, so a request write still in progress ends too.
    async::Task<void> readFinalHead(Connection* connection,
                                    std::string method,
                                    ReadAnswer* out,
                                    ReadFailure* readFailure)
    {
        auto answer = HttpResult<IncomingResponse> {};
        auto interim = 0;
        while (true)
        {
            auto head = co_await connection->readResponseHead();
            if (!head.has_value())
            {
                answer = std::unexpected(std::move(head.error()));
                break;
            }
            out->seen = ResponseSeen::Some;
            if (head->status == 101)
            {
                answer = std::unexpected(detail::refusal(HttpErrorCode::ProtocolError,
                                                         "a 101 to a request that asked for no upgrade"));
                break;
            }
            if (head->status < 200 && ++interim > MaxInterimResponses)
            {
                answer = std::unexpected(detail::refusal(
                    HttpErrorCode::ProtocolError,
                    std::format("more than {} interim responses before the final one", MaxInterimResponses)));
                break;
            }
            if (head->status < 200)
                continue; // interim: the final response follows
            auto framing = responseFraming(method, head->version, head->status, head->fields);
            if (!framing.has_value())
            {
                answer = std::unexpected(std::move(framing.error()));
                break;
            }
            auto reader = connection->bodyReader(*framing);
            answer = IncomingResponse { .head = std::move(*head), .body = std::move(reader) };
            break;
        }
        if (!answer.has_value())
        {
            connection->close();
            readFailure->close();
        }
        out->response = std::move(answer);
    }

    /// One exchange's answer, and whether any of the response had arrived when it failed.
    struct Exchanged
    {
        HttpResult<IncomingResponse> answer;
        ResponseSeen seen = ResponseSeen::None;
    };

    /// The answer of an exchange whose two halves have finished: the write's own failure first, since
    /// the read's is then the close it caused; else the read's, which is the response. A write the
    /// read's failure stopped has no answer of its own.
    [[nodiscard]] Exchanged answerOf(std::optional<HttpResult<void>> wrote, ReadAnswer read)
    {
        Require(read.response.has_value());
        if (wrote.has_value() && !wrote->has_value() && !wasCutByPeer(wrote->error()))
            return Exchanged { .answer = std::unexpected(std::move(wrote->error())), .seen = read.seen };
        return Exchanged { .answer = std::move(*read.response), .seen = read.seen };
    }

    /// Writes @p head and @p body on @p connection while the response head is read (step 4).
    async::Task<Exchanged> exchange(net::EventLoop* loop,
                                    Connection* connection,
                                    RequestHead head,
                                    IBodySource* body)
    {
        auto const framing = framingFor(body);
        auto method = std::string { head.method.text() };
        auto wrote = std::optional<HttpResult<void>> {};
        auto read = ReadAnswer {};
        auto readFailure = ReadFailure { *loop, async::AsyncQueueOptions {} };
        auto halves = std::vector<async::Task<void>> {};
        halves.push_back(
            writeUnlessReadFails(connection, std::move(head), body, framing, &wrote, &readFailure));
        halves.push_back(readFinalHead(connection, std::move(method), &read, &readFailure));
        co_await async::whenAll(std::move(halves));
        auto exchanged = answerOf(std::move(wrote), std::move(read));
        if (!exchanged.answer.has_value())
            if (auto failure = tlsFailureOf(connection->socket()); failure.has_value())
                exchanged.answer = std::unexpected(std::move(*failure));
        co_return exchanged;
    }

    /// Bounds @p handshake by @p timeout unless it is zero (R31).
    async::Task<std::optional<net::ResultAwaitable<void>::Result>> boundedHandshake(
        net::EventLoop* loop, net::ISocket* socket, std::chrono::milliseconds timeout)
    {
        if (timeout == 0ms)
            co_return co_await socket->handshakeIfNeeded();
        co_return co_await net::withTimeout(loop, async::asTask(socket->handshakeIfNeeded()), timeout);
    }

    /// A connection's read deadlines for a request's @p timeouts. The write deadline is none: WASI has
    /// no write timeout, so a peer that stops reading is bounded by @c HttpTimeouts::firstByte and the
    /// caller's stop token.
    [[nodiscard]] WireTimeouts wireTimeoutsOf(HttpTimeouts const& timeouts) noexcept
    {
        return WireTimeouts { .firstByte = timeouts.firstByte,
                              .betweenBytes = timeouts.betweenBytes,
                              .write = 0ms };
    }

    /// What a dial needs of the client, by value for the coroutine's frame.
    struct Dialler
    {
        net::EventLoop* loop;
        net::IConnector* connector;
        net::ITlsContext* tls;
        HttpLimits limits;
    };

    /// A fresh connection to @p origin (steps 2 and 3): the dial, then for @c Scheme::Https TLS bound
    /// to its host, the handshake settled before any request byte.
    async::Task<HttpResult<Connection>> dial(Dialler dialler, Origin origin, HttpTimeouts timeouts)
    {
        auto const host = std::string { bareHost(origin.host) };
        auto options = net::DialOptions {};
        options.connectTimeout = timeouts.connect;
        auto dialled = co_await dialler.connector->connect(host, origin.effectivePort(), options);
        if (!dialled.has_value())
            co_return std::unexpected(classify(TransportErrorRows, dialled.error()));
        auto socket = std::move(*dialled);
        if (origin.scheme == Scheme::Https)
        {
            auto wrapped = dialler.tls->wrapClient(
                std::move(socket), *dialler.loop, net::TlsPeer { .serverName = host });
            if (!wrapped.has_value())
                co_return std::unexpected(classify(WrapErrorRows, wrapped.error()));
            socket = std::move(*wrapped);
            auto const handshake = co_await boundedHandshake(dialler.loop, socket.get(), timeouts.connect);
            if (!handshake.has_value())
                co_return std::unexpected(detail::refusal(
                    HttpErrorCode::ConnectionTimeout,
                    std::format("the TLS handshake did not finish within {} ms", timeouts.connect.count())));
            if (!handshake->has_value())
            {
                if (auto failure = tlsFailureOf(*socket); failure.has_value())
                    co_return std::unexpected(std::move(*failure));
                co_return std::unexpected(classify(TransportErrorRows, handshake->error()));
            }
        }
        co_return Connection {
            *dialler.loop, std::move(socket), MessageRole::Response, dialler.limits, wireTimeoutsOf(timeouts)
        };
    }

    // ---------------------------------------------------------------------------------------------
    // The retry rule: which requests may be sent twice, and which failures say the server had closed
    // a pooled connection before the request came.

    /// The idempotent methods (RFC 9110 section 9.2.2): sent twice, they have the effect of once.
    constexpr auto IdempotentMethods = std::array<std::string_view, 6> {
        "GET", "HEAD", "OPTIONS", "TRACE", "PUT", "DELETE",
    };

    /// The failures of a pooled connection, before any head of the response, that mean the server
    /// had closed it while it sat idle. Which one a close surfaces as, and from which half of the
    /// exchange, depends on the kernel and the timing (measured; each row has the test that needs it):
    constexpr auto StaleConnectionCodes = std::array {
        // The head read saw the server's FIN, or a reset (ECONNRESET, WSAECONNRESET): the in-memory
        // half-closed rows.
        HttpErrorCode::ConnectionTerminated,
        // An OS error nothing classifies further, from either half: the request write found the
        // reset already in (EPIPE from sendmsg on Linux, the loopback case; WSAECONNABORTED on
        // Windows), or the write went out after the FIN, drew the reset, and the read reports it
        // (Linux answers a reset in CLOSE_WAIT with EPIPE; the in-memory closed rows). A bodiless
        // request has no body source, so on this path nothing but the socket answers InternalError.
        // Narrowing this to the write half would leave the read-side EPIPE unretried.
        HttpErrorCode::InternalError,
    };

    /// Whether a request may be sent again: its method is idempotent and it hands over no body.
    enum class Replay : std::uint8_t
    {
        Refused,
        Allowed,
    };

    [[nodiscard]] Replay replayOf(RequestHead const& head, IBodySource const* body) noexcept
    {
        auto const bodiless =
            body == nullptr
            || (body->knownLength() == std::uint64_t { 0 } && body->trailerIntent() == TrailerIntent::None);
        auto const idempotent = std::ranges::contains(IdempotentMethods, head.method.text());
        return bodiless && idempotent ? Replay::Allowed : Replay::Refused;
    }

    /// Where a request's connection came from.
    enum class Provenance : std::uint8_t
    {
        Dialled, ///< Opened for it.
        Pooled,  ///< Reused from the pool.
    };

    /// Whether @p exchanged failed as a stale connection does; that it was a pooled one is the
    /// caller's half of the rule.
    [[nodiscard]] bool isStale(Exchanged const& exchanged) noexcept
    {
        return !exchanged.answer.has_value() && exchanged.seen == ResponseSeen::None
               && std::ranges::contains(StaleConnectionCodes, exchanged.answer.error().code);
    }

} // namespace

// -------------------------------------------------------------------------------------------------
// The pool.

namespace detail
{

    /// Every origin's connections. Loop-affine, as the client.
    struct Pool
    {
        /// An idle connection, with the read deadlines it was opened under and when it expires.
        struct IdleConnection
        {
            Connection connection;
            WireTimeouts timeouts;
            SteadyTimePoint since;                  ///< When it came back: the oldest goes first.
            std::optional<SteadyTimePoint> expires; ///< None: no idle timeout.
        };

        /// One origin's connections: those idle, oldest first, and how many are lent out.
        struct OriginConnections
        {
            std::deque<IdleConnection> idle;
            std::size_t lent = 0;
        };

        IClock const* clock;
        std::size_t maxIdlePerOrigin;
        std::size_t maxConnectionsPerOrigin;
        std::size_t maxConnectionsTotal;
        std::chrono::milliseconds idleTimeout;
        std::map<Origin, OriginConnections> origins;

        [[nodiscard]] static bool hasExpired(IdleConnection const& idle, SteadyTimePoint now) noexcept
        {
            return idle.expires.has_value() && *idle.expires <= now;
        }

        /// Closes @p connections' idle connections past their deadline.
        void sweep(OriginConnections& connections) const
        {
            auto const now = clock->now();
            for (auto& idle: connections.idle)
                if (hasExpired(idle, now))
                    idle.connection.close();
            std::erase_if(connections.idle,
                          [now](IdleConnection const& idle) { return hasExpired(idle, now); });
        }

        /// Closes every idle connection past its deadline, and forgets an origin with nothing left.
        void sweep()
        {
            for (auto& [origin, connections]: origins)
                sweep(connections);
            forgetEmpty();
        }

        /// @return Every connection open, idle or lent out, to every origin.
        [[nodiscard]] std::size_t openCount() const noexcept
        {
            auto count = std::size_t { 0 };
            for (auto const& [origin, connections]: origins)
                count += connections.lent + connections.idle.size();
            return count;
        }

        /// Closes the idle connection that came back first, of any origin.
        /// @return Whether there was one to close.
        [[nodiscard]] bool evictOldestIdle()
        {
            auto* oldest = static_cast<OriginConnections*>(nullptr);
            for (auto& [origin, connections]: origins)
                if (!connections.idle.empty()
                    && (oldest == nullptr || connections.idle.front().since < oldest->idle.front().since))
                    oldest = &connections;
            if (oldest == nullptr)
                return false;
            oldest->idle.front().connection.close();
            oldest->idle.pop_front();
            return true;
        }

        void forgetEmpty()
        {
            std::erase_if(origins, [](auto const& entry) {
                return entry.second.idle.empty() && entry.second.lent == 0;
            });
        }

        [[nodiscard]] std::size_t idleCount() const noexcept
        {
            auto const now = clock->now();
            auto count = std::size_t { 0 };
            for (auto const& [origin, connections]: origins)
                count += static_cast<std::size_t>(std::ranges::count_if(
                    connections.idle, [now](IdleConnection const& idle) { return !hasExpired(idle, now); }));
            return count;
        }
    };

} // namespace detail

namespace
{

    /// The pool's key for @p origin: one origin however its host is cased and whether its port is
    /// named or the scheme's default.
    [[nodiscard]] Origin poolKeyOf(Origin const& origin)
    {
        auto host = std::string { bareHost(origin.host) };
        std::ranges::transform(host, host.begin(), detail::toLowerAscii);
        return Origin { .scheme = origin.scheme, .host = std::move(host), .port = origin.effectivePort() };
    }

    [[nodiscard]] bool sameDeadlines(WireTimeouts const& a, WireTimeouts const& b) noexcept
    {
        return a.firstByte == b.firstByte && a.betweenBytes == b.betweenBytes && a.write == b.write;
    }

    /// Whether a lease still counts against its origin's cap.
    enum class Place : std::uint8_t
    {
        Released,
        Held,
    };

    /// One connection's place under its origin's cap, from the request that took it to the end of its
    /// response body. It holds the pool weakly: once the client is gone, nothing goes back.
    class Lease
    {
      public:
        Lease(std::weak_ptr<detail::Pool> pool, Origin key, WireTimeouts timeouts):
            _pool { std::move(pool) }, _key { std::move(key) }, _timeouts { timeouts }
        {
        }

        ~Lease() { release(); }

        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;

        Lease(Lease&& other) noexcept:
            _pool { std::move(other._pool) },
            _key { std::move(other._key) },
            _timeouts { other._timeouts },
            _place { std::exchange(other._place, Place::Released) }
        {
        }

        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other)
            {
                release();
                _pool = std::move(other._pool);
                _key = std::move(other._key);
                _timeouts = other._timeouts;
                _place = std::exchange(other._place, Place::Released);
            }
            return *this;
        }

        /// Puts @p connection back among its origin's idle connections if it may carry another
        /// exchange, the client is alive and the idle cap has room; closes it otherwise. Releases the
        /// place either way.
        void giveBack(Connection connection)
        {
            auto const pool = _pool.lock();
            auto* const connections =
                _place == Place::Held && pool != nullptr && connection.reusable() == Reuse::Yes
                    ? &pool->origins[_key]
                    : nullptr;
            if (connections != nullptr)
                pool->sweep(*connections); // an expired idle connection holds no room under the cap
            if (connections != nullptr && connections->idle.size() < pool->maxIdlePerOrigin)
            {
                auto const now = pool->clock->now();
                auto expires = std::optional<SteadyTimePoint> {};
                if (pool->idleTimeout != 0ms)
                    expires = now + pool->idleTimeout;
                connections->idle.push_back(
                    detail::Pool::IdleConnection { .connection = std::move(connection),
                                                   .timeouts = _timeouts,
                                                   .since = now,
                                                   .expires = expires });
            }
            else
                connection.close();
            release();
        }

        /// Gives up the place, the connection having been closed or never opened.
        void release() noexcept
        {
            if (std::exchange(_place, Place::Released) == Place::Released)
                return;
            if (auto const pool = _pool.lock(); pool != nullptr)
            {
                if (auto const found = pool->origins.find(_key); found != pool->origins.end())
                {
                    --found->second.lent;
                    if (found->second.lent == 0 && found->second.idle.empty())
                        pool->origins.erase(found);
                }
            }
        }

      private:
        std::weak_ptr<detail::Pool> _pool;
        Origin _key;
        WireTimeouts _timeouts;
        Place _place = Place::Held;
    };

    /// A place under the cap, and the idle connection that came with it, if one could be reused.
    struct Acquired
    {
        Lease lease;
        std::optional<Connection> idle;
    };

    /// Takes a place under @p key's cap for a request under @p timeouts, with the most recently used
    /// idle connection opened under the same deadlines, if there is one.
    [[nodiscard]] HttpResult<Acquired> acquire(std::shared_ptr<detail::Pool> const& pool,
                                               Origin const& key,
                                               WireTimeouts timeouts)
    {
        pool->sweep();
        auto& connections = pool->origins[key];
        auto& idle = connections.idle;
        auto const reusable = [timeouts](detail::Pool::IdleConnection const& candidate) {
            return sameDeadlines(candidate.timeouts, timeouts)
                   && candidate.connection.reusable() == Reuse::Yes;
        };
        auto found = std::ranges::find_if(idle.rbegin(), idle.rend(), reusable);
        auto taken = std::optional<Connection> {};
        if (found != idle.rend())
        {
            taken = std::move(found->connection);
            idle.erase(std::next(found).base());
        }
        else if (connections.lent + idle.size() >= pool->maxConnectionsPerOrigin)
        {
            if (idle.empty())
            {
                if (connections.lent == 0)
                    pool->origins.erase(key);
                return std::unexpected(
                    detail::refusal(HttpErrorCode::ConnectionLimitReached,
                                    std::format("all {} connections to the origin are in use",
                                                pool->maxConnectionsPerOrigin)));
            }
            idle.front().connection.close(); // under other deadlines: makes room
            idle.pop_front();
        }
        if (!taken.has_value() && pool->openCount() >= pool->maxConnectionsTotal && !pool->evictOldestIdle())
        {
            if (connections.lent == 0 && idle.empty())
                pool->origins.erase(key);
            return std::unexpected(detail::refusal(
                HttpErrorCode::ConnectionLimitReached,
                std::format("all {} connections of the client are in use", pool->maxConnectionsTotal)));
        }
        ++connections.lent;
        return Acquired { .lease = Lease { pool, key, timeouts }, .idle = std::move(taken) };
    }

    // ---------------------------------------------------------------------------------------------
    // The response body that gives its connection back.

    /// What a @c PooledBodyReader and its reads in flight share.
    struct LentConnection
    {
        std::unique_ptr<IBodyReader> inner;
        /// Until it goes back or is closed.
        std::optional<Connection> connection;
        Lease lease;
        /// Taken at the body's end, before the connection went back: a reuse starts another body.
        std::optional<HttpResult<Fields>> trailers;

        /// At the body's end: hands the connection to @c Lease::giveBack, which pools it if it may
        /// carry another exchange and closes it otherwise (`Connection: close`, an until-close body),
        /// so a body that is over holds no place under the cap.
        void settle()
        {
            if (!connection.has_value())
                return;
            auto handed = std::move(*connection);
            connection.reset();
            lease.giveBack(std::move(handed));
        }

        /// Closes the connection and gives up its place.
        void closeConnection() noexcept
        {
            if (connection.has_value())
            {
                connection->close();
                connection.reset();
            }
            lease.release();
        }
    };

    /// The response body the client hands out: the connection's own reader, which gives the
    /// connection back to the pool at the body's end, or closes it if the body ends otherwise.
    class PooledBodyReader final: public IBodyReader
    {
      public:
        PooledBodyReader(std::unique_ptr<IBodyReader> inner, Connection connection, Lease lease):
            _state { std::make_shared<LentConnection>(LentConnection { .inner = std::move(inner),
                                                                       .connection = std::move(connection),
                                                                       .lease = std::move(lease),
                                                                       .trailers = {} }) }
        {
            // A bodiless response is over with its head: nothing would read it to its end.
            if (_state->connection->reusable() == Reuse::Yes)
            {
                _state->trailers = Fields {};
                _state->settle();
            }
        }

        ~PooledBodyReader() override { _state->closeConnection(); }

        PooledBodyReader(PooledBodyReader const&) = delete;
        PooledBodyReader& operator=(PooledBodyReader const&) = delete;
        PooledBodyReader(PooledBodyReader&&) = delete;
        PooledBodyReader& operator=(PooledBodyReader&&) = delete;

        [[nodiscard]] async::Task<HttpResult<BodyChunk>> read(std::size_t maxBytes) override
        {
            return readFrom(_state, maxBytes);
        }

        [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override { return trailersOf(_state); }

      private:
        static async::Task<HttpResult<BodyChunk>> readFrom(std::shared_ptr<LentConnection> state,
                                                           std::size_t maxBytes)
        {
            auto chunk = co_await state->inner->read(maxBytes);
            if (!chunk.has_value())
                state->closeConnection();
            else if (chunk->end == BodyEnd::Reached && state->connection.has_value())
            {
                state->trailers = co_await state->inner->trailers();
                state->settle();
            }
            co_return chunk;
        }

        static async::Task<HttpResult<Fields>> trailersOf(std::shared_ptr<LentConnection> state)
        {
            if (state->trailers.has_value())
                co_return std::move(*std::exchange(state->trailers, HttpResult<Fields> { Fields {} }));
            co_return co_await state->inner->trailers();
        }

        std::shared_ptr<LentConnection> _state;
    };

} // namespace

HttpClient::HttpClient(net::EventLoop& loop,
                       net::IConnector& connector,
                       net::ITlsContext* tls,
                       HttpClientOptions options):
    _loop { &loop },
    _connector { &connector },
    _tls { tls },
    _options { options },
    _pool { std::make_shared<detail::Pool>(
        detail::Pool { .clock = &loop.clock(),
                       .maxIdlePerOrigin = options.maxIdlePerOrigin,
                       .maxConnectionsPerOrigin = options.maxConnectionsPerOrigin,
                       .maxConnectionsTotal = options.maxConnectionsTotal,
                       .idleTimeout = options.idleTimeout,
                       .origins = {} }) }
{
}

HttpClient::~HttpClient() = default;
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

async::Task<HttpResult<IncomingResponse>> HttpClient::send(OutgoingRequest request)
{
    Require(_pool != nullptr); // not a moved-from client
    // 1. Everything that needs no connection, refused before one is taken.
    auto head = headFor(request.origin, std::move(request.head));
    if (!head.has_value())
        co_return std::unexpected(std::move(head.error()));
    if (request.origin.scheme == Scheme::Https && _tls == nullptr)
        co_return std::unexpected(
            detail::refusal(HttpErrorCode::ConfigurationError, "an https request to a client without TLS"));
    auto const timeouts = request.timeouts.value_or(_options.timeouts);
    auto const dialler =
        Dialler { .loop = _loop, .connector = _connector, .tls = _tls, .limits = _options.limits };

    // 2. and 3. A pooled connection, or a fresh one under the origin's cap.
    auto acquired = acquire(_pool, poolKeyOf(request.origin), wireTimeoutsOf(timeouts));
    if (!acquired.has_value())
        co_return std::unexpected(std::move(acquired.error()));
    auto connection = std::move(acquired->idle);
    auto const provenance = connection.has_value() ? Provenance::Pooled : Provenance::Dialled;
    if (!connection.has_value())
    {
        auto dialled = co_await dial(dialler, request.origin, timeouts);
        if (!dialled.has_value())
            co_return std::unexpected(std::move(dialled.error()));
        connection.emplace(std::move(*dialled));
    }

    // 4. The exchange, sent again once on a fresh connection if a pooled one was stale.
    auto const replay = replayOf(*head, request.body.get());
    auto again = replay == Replay::Allowed && provenance == Provenance::Pooled ? std::optional { *head }
                                                                               : std::nullopt;
    auto exchanged = co_await exchange(_loop, &*connection, std::move(*head), request.body.get());
    if (again.has_value() && isStale(exchanged))
    {
        connection->close();
        auto dialled = co_await dial(dialler, request.origin, timeouts);
        if (!dialled.has_value())
            co_return std::unexpected(std::move(dialled.error()));
        connection.emplace(std::move(*dialled));
        // The body handed over nothing, but its source was asked for it: an empty one of the same
        // framing stands in for it.
        auto empty = request.body != nullptr ? std::make_unique<BufferBodySource>(std::string {}) : nullptr;
        exchanged = co_await exchange(_loop, &*connection, std::move(*again), empty.get());
    }
    if (!exchanged.answer.has_value())
        co_return std::unexpected(std::move(exchanged.answer.error()));
    auto response = std::move(*exchanged.answer);
    response.body = std::make_unique<PooledBodyReader>(
        std::move(response.body), std::move(*connection), std::move(acquired->lease));
    co_return response;
}

std::size_t HttpClient::idleConnections() const noexcept
{
    Require(_pool != nullptr); // not a moved-from client
    return _pool->idleCount();
}

} // namespace core::http
