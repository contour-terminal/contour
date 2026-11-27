// SPDX-License-Identifier: Apache-2.0
#include <core/http/HttpServer.hpp>

#include <core/async/AsyncQueue.hpp>
#include <core/async/Cancellation.hpp>
#include <core/http/Connection.hpp>
#include <core/http/detail/Syntax.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketDeadline.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace core::http
{

namespace
{

    using namespace std::chrono_literals;

    /// The statuses refusals are answered with (RFC 9110 section 15, RFC 9112). A request's every
    /// syntax and framing refusal is a 400 but the two RFC 9110 names apart, which the reason column
    /// carries; a size is 413, 414 or 431 by what was too large; a read deadline is 408.
    constexpr auto ErrorStatusRows = std::array {
        ErrorStatusRow { HttpErrorCode::ProtocolError, RefusalReason::Unclassified, 400 },
        ErrorStatusRow { HttpErrorCode::ProtocolError, RefusalReason::UnsupportedVersion, 505 },
        ErrorStatusRow { HttpErrorCode::ProtocolError, RefusalReason::UnimplementedCoding, 501 },
        ErrorStatusRow { HttpErrorCode::RequestMethodInvalid, RefusalReason::Unclassified, 400 },
        ErrorStatusRow { HttpErrorCode::RequestUriInvalid, RefusalReason::Unclassified, 400 },
        ErrorStatusRow { HttpErrorCode::RequestUriTooLong, RefusalReason::Unclassified, 414 },
        ErrorStatusRow { HttpErrorCode::RequestHeaderSectionSize, RefusalReason::Unclassified, 431 },
        ErrorStatusRow { HttpErrorCode::RequestHeaderSize, RefusalReason::Unclassified, 431 },
        ErrorStatusRow { HttpErrorCode::RequestTrailerSectionSize, RefusalReason::Unclassified, 431 },
        ErrorStatusRow { HttpErrorCode::RequestTrailerSize, RefusalReason::Unclassified, 431 },
        ErrorStatusRow { HttpErrorCode::RequestBodySize, RefusalReason::Unclassified, 413 },
        ErrorStatusRow { HttpErrorCode::RequestLengthRequired, RefusalReason::Unclassified, 411 },
        ErrorStatusRow { HttpErrorCode::ConnectionReadTimeout, RefusalReason::Unclassified, 408 },
        ErrorStatusRow { HttpErrorCode::InternalError, RefusalReason::Unclassified, 500 },
    };

    /// A refusal a server closes on without an answer, whatever its code's row says.
    struct UnansweredRow
    {
        HttpErrorCode code;
        RefusalReason reason;
    };

    /// An idle connection: no byte of a request arrived before its deadline (RFC 9112 section 9.5).
    constexpr auto UnansweredRows = std::array {
        UnansweredRow { HttpErrorCode::ConnectionReadTimeout, RefusalReason::NothingArrived },
    };

    /// A status and its reason phrase.
    struct StatusPhrase
    {
        std::uint16_t status;
        std::string_view phrase;
    };

    /// The phrases of the statuses the server writes itself (RFC 9110 section 15).
    constexpr auto StatusPhrases = std::array {
        StatusPhrase { 100, "Continue" },
        StatusPhrase { 400, "Bad Request" },
        StatusPhrase { 408, "Request Timeout" },
        StatusPhrase { 411, "Length Required" },
        StatusPhrase { 413, "Content Too Large" },
        StatusPhrase { 414, "URI Too Long" },
        StatusPhrase { 431, "Request Header Fields Too Large" },
        StatusPhrase { 500, "Internal Server Error" },
        StatusPhrase { 501, "Not Implemented" },
        StatusPhrase { 505, "HTTP Version Not Supported" },
    };

    /// The row a handler's own failure is answered with.
    [[nodiscard]] ErrorStatusRow internalErrorRow() noexcept
    {
        return *std::ranges::find(ErrorStatusRows, HttpErrorCode::InternalError, &ErrorStatusRow::code);
    }

    /// A head of @p status with its phrase.
    [[nodiscard]] ResponseHead statusHead(std::uint16_t status)
    {
        auto head = ResponseHead {};
        head.status = status;
        head.reason = ReasonPhrase::parse(statusPhrase(status)).value_or(ReasonPhrase {});
        return head;
    }

    // ---------------------------------------------------------------------------------------------
    // One exchange: what the server and the handler's body reader share about one request.

    /// Where `Expect: 100-continue` is.
    enum class Continue : std::uint8_t
    {
        NotExpected, ///< The request did not ask, or cannot be answered with one (HTTP/1.0, no body).
        Expected,    ///< Asked for, and not yet sent: the first body read sends it.
        Sending,     ///< A body read is writing it.
        Sent,        ///< Written.
        Moot,        ///< The final response is under way, so none may follow.
    };

    /// Where the request body is.
    enum class BodyState : std::uint8_t
    {
        Unread,  ///< Not yet at its end.
        Reached, ///< At its end.
        Failed,  ///< A read of it failed; the read side is over.
    };

    /// What a body read hands the server when it returns: only that it did.
    struct ReadReturned
    {
    };

    /// The state of one request a handler's body reader shares with the server, which may outlive the
    /// server's flow when the handler keeps the reader: the server severs it when the request is over.
    struct Exchange
    {
        Connection* connection; ///< The connection; null once severed, after which every read is refused.
        Continue expect;        ///< Where `Expect: 100-continue` is.
        BodyState body = BodyState::Unread;       ///< Where the body is.
        std::uint64_t consumed = 0;               ///< Body bytes read so far, the handler's and the drain's.
        std::optional<HttpError> readFailure;     ///< What ended the body, which answers a failed handler.
        std::size_t readsInFlight = 0;            ///< Body reads started and not yet returned.
        async::AsyncQueue<ReadReturned> returned; ///< Told each time the last read in flight returns.

        Exchange(Connection* owner, net::EventLoop& loop, Continue expects):
            connection { owner },
            expect { expects },
            returned { loop,
                       async::AsyncQueueOptions { .capacity = 1,
                                                  .overflow = async::AsyncQueueOverflow::DropOldest } }
        {
        }

        /// Records @p error as what ended the body, the first time, and hands it back.
        [[nodiscard]] HttpError fail(HttpError error)
        {
            if (body != BodyState::Failed)
            {
                body = BodyState::Failed;
                readFailure = error;
            }
            return error;
        }

        /// No read reaches the connection from now on.
        void sever() noexcept { connection = nullptr; }
    };

    /// Counts one body read in flight for as long as it lives, on every way out of it.
    class ReadInFlight
    {
      public:
        explicit ReadInFlight(Exchange* exchange) noexcept: _exchange { exchange }
        {
            ++_exchange->readsInFlight;
        }
        ~ReadInFlight()
        {
            if (--_exchange->readsInFlight == 0)
                std::ignore = _exchange->returned.push(ReadReturned {});
        }
        ReadInFlight(ReadInFlight const&) = delete;
        ReadInFlight& operator=(ReadInFlight const&) = delete;
        ReadInFlight(ReadInFlight&&) = delete;
        ReadInFlight& operator=(ReadInFlight&&) = delete;

      private:
        Exchange* _exchange;
    };

    /// The refusal of a read of a request that is over.
    [[nodiscard]] HttpError requestOver()
    {
        return detail::refusal(HttpErrorCode::ConnectionTerminated,
                               "the request is over: its connection moved on");
    }

    /// The request body as the handler reads it: the connection's reader, with the interim response
    /// sent on the first read, each failure recorded for the server, and every read refused once the
    /// server has severed the exchange.
    class ServerBodyReader final: public IBodyReader
    {
      public:
        ServerBodyReader(std::shared_ptr<Exchange> exchange, std::unique_ptr<IBodyReader> inner):
            _exchange { std::move(exchange) }, _inner { std::move(inner) }
        {
        }

        ~ServerBodyReader() override = default;
        ServerBodyReader(ServerBodyReader const&) = delete;
        ServerBodyReader& operator=(ServerBodyReader const&) = delete;
        ServerBodyReader(ServerBodyReader&&) = delete;
        ServerBodyReader& operator=(ServerBodyReader&&) = delete;

        [[nodiscard]] async::Task<HttpResult<BodyChunk>> read(std::size_t maxBytes) override
        {
            return readThrough(_exchange, _inner.get(), maxBytes);
        }

        [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override
        {
            return trailersThrough(_exchange, _inner.get());
        }

      private:
        static async::Task<HttpResult<BodyChunk>> readThrough(std::shared_ptr<Exchange> exchange,
                                                              IBodyReader* inner,
                                                              std::size_t maxBytes)
        {
            if (exchange->connection == nullptr)
                co_return std::unexpected(requestOver());
            auto const inFlight = ReadInFlight { exchange.get() };
            if (exchange->expect == Continue::Expected)
            {
                exchange->expect = Continue::Sending;
                auto wrote = co_await exchange->connection->writeResponseHead(statusHead(100), Framing {});
                if (!wrote.has_value())
                {
                    exchange->expect = Continue::Moot;
                    co_return std::unexpected(exchange->fail(std::move(wrote.error())));
                }
                exchange->expect = Continue::Sent;
                if (exchange->connection == nullptr) // severed while the interim response was written
                    co_return std::unexpected(requestOver());
            }
            auto chunk = co_await inner->read(maxBytes);
            if (!chunk.has_value())
                co_return std::unexpected(exchange->fail(std::move(chunk.error())));
            exchange->consumed += chunk->bytes.size();
            if (chunk->end == BodyEnd::Reached && exchange->body == BodyState::Unread)
                exchange->body = BodyState::Reached;
            co_return chunk;
        }

        static async::Task<HttpResult<Fields>> trailersThrough(std::shared_ptr<Exchange> exchange,
                                                               IBodyReader* inner)
        {
            if (exchange->connection == nullptr)
                co_return std::unexpected(requestOver());
            auto trailers = co_await inner->trailers();
            if (!trailers.has_value())
                co_return std::unexpected(exchange->fail(std::move(trailers.error())));
            co_return trailers;
        }

        std::shared_ptr<Exchange> _exchange;
        std::unique_ptr<IBodyReader> _inner;
    };

    /// A body source with its trailers dropped: a chunked body sent to an HTTP/1.0 client, which reads
    /// until close and has no trailer section.
    class WithoutTrailers final: public IBodySource
    {
      public:
        explicit WithoutTrailers(IBodySource* inner): _inner { inner } {}

        [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override
        {
            return _inner->knownLength();
        }
        [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::None; }
        [[nodiscard]] async::Task<HttpResult<BodyChunk>> next() override { return _inner->next(); }
        [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override { return none(); }

      private:
        static async::Task<HttpResult<Fields>> none() { co_return Fields {}; }

        IBodySource* _inner;
    };

    // ---------------------------------------------------------------------------------------------
    // The exchange, request by request.

    /// Whether a connection carries another request.
    enum class Next : std::uint8_t
    {
        Another,
        Close,
    };

    /// Whether a request is the last its connection serves.
    enum class LastRequest : std::uint8_t
    {
        No,
        Yes,
    };

    /// How a drain of an unread body ended.
    enum class Drained : std::uint8_t
    {
        Clean, ///< The body reached its end within the bound.
        Close, ///< It is longer than the bound, or failed: the connection closes.
    };

    /// Whether @p head asks for `100 Continue` before its body, and may get one.
    [[nodiscard]] Continue continueFor(RequestHead const& head, Framing const& framing)
    {
        auto const expects = std::ranges::any_of(head.fields.get("Expect"), [](std::string_view value) {
            return detail::equalsIgnoringCase(value, "100-continue");
        });
        auto const hasBody =
            framing.kind != BodyFraming::None && (framing.kind != BodyFraming::Length || framing.length != 0);
        return expects && hasBody && head.version == HttpVersion::Http11 ? Continue::Expected
                                                                         : Continue::NotExpected;
    }

    /// Answers with @p row's status and `Connection: close`, if there is a row and the write side can
    /// still take it; a write refused or failed is the end of the connection either way.
    async::Task<void> answer(Connection* connection, std::optional<ErrorStatusRow> row)
    {
        if (!row.has_value())
            co_return;
        std::ignore = co_await connection->writeResponseHead(
            statusHead(row->status),
            Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close });
    }

    /// Reads and discards what the handler left of the body, up to @p maxBytes; a known remainder
    /// past them is not read at all.
    async::Task<Drained> drain(std::shared_ptr<Exchange> exchange,
                               Connection* connection,
                               Framing framing,
                               std::uint64_t maxBytes,
                               std::size_t chunkBytes)
    {
        if (exchange->body != BodyState::Unread)
            co_return exchange->body == BodyState::Reached ? Drained::Clean : Drained::Close;
        if (framing.kind == BodyFraming::Length && framing.length - exchange->consumed > maxBytes)
            co_return Drained::Close;
        auto reader = ServerBodyReader { exchange, connection->bodyReader(framing) };
        auto const start = exchange->consumed;
        while (true)
        {
            auto chunk = co_await reader.read(chunkBytes);
            if (!chunk.has_value() || exchange->consumed - start > maxBytes)
                co_return Drained::Close;
            if (chunk->end == BodyEnd::Reached)
                co_return Drained::Clean;
        }
    }

    /// Runs the flow @p start starts; an exception it throws, but for a cancellation of the flow,
    /// becomes its @c HttpErrorCode::InternalError, named after @p what.
    template <typename Start>
    std::invoke_result_t<Start> guarded(Start start, char const* what)
    {
        auto thrown = std::string {};
        try
        {
            co_return co_await start();
        }
        catch (async::OperationCancelled const&)
        {
            throw;
        }
        catch (std::exception const& error)
        {
            thrown = error.what();
        }
        catch (...)
        {
            thrown = "an exception that is no std::exception";
        }
        co_return std::unexpected(
            detail::refusal(HttpErrorCode::InternalError, std::format("{} threw: {}", what, thrown)));
    }

    /// Serves one request of @p connection; @p current receives its exchange, which the caller severs.
    async::Task<Next> serveOne(net::EventLoop* loop,
                               Connection* connection,
                               IHttpHandler* handler,
                               HttpServerOptions const* options,
                               std::optional<net::ResolvedEndpoint> peer,
                               LastRequest last,
                               std::shared_ptr<Exchange>* current)
    {
        auto head = co_await connection->readRequestHead();
        if (!head.has_value())
        {
            co_await answer(connection, errorStatusFor(head.error()));
            co_return Next::Close;
        }
        auto const framing = requestFraming(head->version, head->fields);
        if (!framing.has_value())
        {
            co_await answer(connection, errorStatusFor(framing.error()));
            co_return Next::Close;
        }
        auto inner = connection->bodyReader(*framing);
        // Zero bytes read nothing and report where the body is: a declared length past maxBodyBytes
        // is refused here, before the handler sees the request.
        auto const probe = co_await inner->read(0);
        if (!probe.has_value())
        {
            co_await answer(connection, errorStatusFor(probe.error()));
            co_return Next::Close;
        }
        auto const version = head->version;
        auto exchange = std::make_shared<Exchange>(connection, *loop, continueFor(*head, *framing));
        *current = exchange;
        if (probe->end == BodyEnd::Reached)
            exchange->body = BodyState::Reached;

        auto result = co_await guarded(
            [handler,
             request =
                 IncomingRequest { .head = std::move(*head),
                                   .body = std::make_unique<ServerBodyReader>(exchange, std::move(inner)),
                                   .peer = peer }]() mutable { return handler->handle(std::move(request)); },
            "the handler");

        // A read the handler left running is writing the interim response: no final one can follow.
        if (exchange->expect == Continue::Sending)
            co_return Next::Close;
        auto const continueUnsent = exchange->expect == Continue::Expected;
        exchange->expect = Continue::Moot;
        if (!result.has_value())
        {
            co_await answer(connection,
                            exchange->readFailure.has_value() ? errorStatusFor(*exchange->readFailure)
                                                              : std::optional { internalErrorRow() });
            co_return Next::Close;
        }
        auto response = std::move(*result);
        // A 1xx is no final response, and a status past 999 fits no status line: either is the
        // handler's failure, answered 500.
        if (response.head.status < 200 || response.head.status > 999)
        {
            co_await answer(connection, internalErrorRow());
            co_return Next::Close;
        }

        // Unread, with the client maybe waiting for the `100 Continue` it never got, the body may or
        // may not follow: nothing can tell its bytes from the next request's.
        auto persistence =
            last == LastRequest::Yes || framing->persistence == Persistence::Close || continueUnsent
                ? Persistence::Close
                : Persistence::KeepAlive;
        // The handler is done with the body: drain it now, so the response can say whether it closes.
        if (persistence == Persistence::KeepAlive && !connection->hasLiveBodyReader())
        {
            auto const drained = co_await drain(
                exchange, connection, *framing, options->maxDrainBytes, options->limits.maxReadChunk);
            if (drained == Drained::Close)
                persistence = Persistence::Close;
        }

        auto replyFraming = framingFor(response.body.get());
        replyFraming.persistence = persistence;
        auto* source = response.body.get();
        auto trailerless = std::optional<WithoutTrailers> {};
        if (version == HttpVersion::Http10 && replyFraming.kind == BodyFraming::Chunked)
        {
            replyFraming.kind = BodyFraming::UntilClose; // HTTP/1.0 has no chunked coding
            source = &trailerless.emplace(source);
        }
        if (auto wrote = co_await connection->writeResponseHead(std::move(response.head), replyFraming);
            !wrote.has_value())
        {
            // Refused before the first wire byte (a framing field of the handler's), the connection is
            // as it was and takes the 500; failed after it, the write side refuses this one too.
            co_await answer(connection, internalErrorRow());
            co_return Next::Close;
        }
        // The head is out: a failed or throwing body has nothing but the close to say it failed.
        if (auto wrote = co_await guarded(
                [connection, source, replyFraming]() { return connection->writeBody(source, replyFraming); },
                "the response body");
            !wrote.has_value())
            co_return Next::Close;
        trailerless.reset();
        response.body.reset(); // a source streaming the request body holds its reader

        if (persistence == Persistence::Close || connection->hasLiveBodyReader())
            co_return Next::Close;
        auto const drained = co_await drain(
            exchange, connection, *framing, options->maxDrainBytes, options->limits.maxReadChunk);
        if (drained == Drained::Close)
            co_return Next::Close;
        co_return connection->reusable() == Reuse::Yes ? Next::Another : Next::Close;
    }

    /// Ends @p exchange before its connection is closed: no read reaches the connection after this,
    /// and one still parked is retired and has returned, so the close's own reads do not meet it. A
    /// read writing the interim response cannot be retired: the connection is closed under it.
    async::Task<void> settle(std::shared_ptr<Exchange> exchange, Connection* connection)
    {
        exchange->sever();
        if (exchange->expect == Continue::Sending)
        {
            connection->close();
            co_return;
        }
        if (exchange->readsInFlight == 0)
            co_return;
        connection->socket().cancelRead();
        while (exchange->readsInFlight > 0)
            if (!(co_await exchange->returned.pop()).has_value())
                co_return;
    }

    /// Severs the exchange it holds when the connection's flow ends or is destroyed.
    class Severing
    {
      public:
        Severing() = default;
        ~Severing()
        {
            if (current != nullptr)
                current->sever();
        }
        Severing(Severing const&) = delete;
        Severing& operator=(Severing const&) = delete;
        Severing(Severing&&) = delete;
        Severing& operator=(Severing&&) = delete;

        std::shared_ptr<Exchange> current; ///< The exchange of the request being served, or the last.
    };

    // ---------------------------------------------------------------------------------------------
    // The accept loop's side: admission, and the report of a refusal.

    /// Reports connections closed at the admission cap: the first, then at most one a second, with
    /// how many were closed since, so a flood is visible without a line per connection.
    class CapReporter
    {
      public:
        CapReporter(net::EventLoop* loop, net::AcceptLoopReporting const* reporting):
            _loop { loop }, _reporting { reporting }
        {
        }

        /// One connection was closed at the cap.
        void refused()
        {
            ++_unreported;
            auto const now = _loop->clock().now();
            if (_last.has_value() && now - *_last < 1s)
                return;
            _last = now;
            net::reportAcceptLoopEvent(
                *_reporting,
                net::AcceptLoopEvent {
                    .surface = _reporting->surface,
                    .line = std::format("{}: {} connection(s) past the admission cap closed unanswered",
                                        _reporting->surface,
                                        std::exchange(_unreported, 0)),
                    .error = net::makeNetError(net::NetErrorCode::ResourceExhausted, 0, "admission cap"),
                    .kind = net::AcceptLoopEventKind::Warning });
        }

      private:
        net::EventLoop* _loop;
        net::AcceptLoopReporting const* _reporting;
        std::optional<platform::SteadyTimePoint> _last;
        std::uint64_t _unreported = 0;
    };

    /// A connection's hold on its server's admission policy; the lease ends before the policy goes.
    struct Admission
    {
        std::shared_ptr<net::IAdmissionControl> policy;
        std::optional<net::AdmissionLease> lease;
    };

    async::Task<void> serveAdmitted(net::EventLoop* loop,
                                    std::unique_ptr<net::ISocket> socket,
                                    IHttpHandler* handler,
                                    HttpServerOptions options,
                                    Admission admission)
    {
        auto const held = std::move(admission);
        co_await serveHttpConnection(loop, std::move(socket), handler, std::move(options));
    }

    /// The flow of a connection not admitted: nothing, once its socket is closed.
    async::Task<void> refusedConnection()
    {
        co_return;
    }

} // namespace

std::span<ErrorStatusRow const> errorStatusTable() noexcept
{
    return ErrorStatusRows;
}

std::optional<ErrorStatusRow> errorStatusFor(HttpError const& error) noexcept
{
    if (std::ranges::any_of(UnansweredRows, [&](UnansweredRow const& row) {
            return row.code == error.code && row.reason == error.reason;
        }))
        return std::nullopt;
    auto const matches = [&](RefusalReason reason) {
        return std::ranges::find_if(ErrorStatusRows, [&](ErrorStatusRow const& row) {
            return row.code == error.code && row.reason == reason;
        });
    };
    if (auto const exact = matches(error.reason); exact != ErrorStatusRows.end())
        return *exact;
    if (auto const fallback = matches(RefusalReason::Unclassified); fallback != ErrorStatusRows.end())
        return *fallback;
    return std::nullopt;
}

std::string_view statusPhrase(std::uint16_t status) noexcept
{
    auto const found = std::ranges::find(StatusPhrases, status, &StatusPhrase::status);
    return found != StatusPhrases.end() ? found->phrase : std::string_view {};
}

async::Task<void> serveHttpConnection(net::EventLoop* loop,
                                      std::unique_ptr<net::ISocket> socket,
                                      IHttpHandler* handler,
                                      HttpServerOptions options)
{
    {
        // A transport that negotiates (TLS) finishes before the first request byte; a client that
        // never finishes the handshake is held to the head's bound, and one that fails it is dropped.
        auto target = net::SocketDeadlineTarget { .socket = socket.get(), .expired = false };
        auto const deadline = net::armSocketDeadline(loop, options.headTimeout, &target);
        if (auto const handshaken = co_await socket->handshakeIfNeeded(); !handshaken.has_value())
            co_return;
    }
    auto const remote = socket->remoteEndpoint();
    auto const peer = remote.has_value() ? std::optional { *remote } : std::nullopt;
    auto connection = Connection { *loop,
                                   std::move(socket),
                                   MessageRole::Request,
                                   options.limits,
                                   WireTimeouts { .firstByte = options.idleTimeout,
                                                  .betweenBytes = options.bodyBetweenBytes,
                                                  .write = options.writeTimeout,
                                                  .head = options.headTimeout,
                                                  .body = options.bodyTotal } };
    auto severing = Severing {};
    auto served = std::size_t { 0 };
    auto next = Next::Another;
    while (next == Next::Another)
    {
        ++served;
        auto const last = options.maxRequestsPerConnection != 0 && served >= options.maxRequestsPerConnection
                              ? LastRequest::Yes
                              : LastRequest::No;
        if (severing.current != nullptr)
            severing.current->sever(); // the last request's reader is gone: it has nothing in flight
        next = co_await serveOne(loop, &connection, handler, &options, peer, last, &severing.current);
    }
    if (severing.current != nullptr)
        co_await settle(severing.current, &connection);
    std::ignore = co_await net::closeLingering(&connection.socket(), loop, options.linger);
}

async::Task<void> serveHttp(net::EventLoop* loop,
                            net::IListener* listener,
                            IHttpHandler* handler,
                            HttpServerOptions options)
{
    // `options` and the reporter live in this frame for as long as the accept loop runs, and each
    // spawned connection takes a copy of the options of its own.
    auto reporter = CapReporter { loop, &options.reporting };
    co_await net::acceptLoop(
        loop,
        listener,
        options.reporting,
        net::ConnectionFlow::Spawned,
        [loop, handler, serving = &options, capped = &reporter](std::unique_ptr<net::ISocket> socket) {
            auto admission = Admission { .policy = serving->admission, .lease = std::nullopt };
            if (admission.policy != nullptr)
            {
                admission.lease = admission.policy->tryAdmit();
                if (!admission.lease.has_value())
                {
                    socket.reset(); // past the cap: closed unanswered
                    capped->refused();
                    return refusedConnection();
                }
            }
            return serveAdmitted(loop, std::move(socket), handler, *serving, std::move(admission));
        });
}

} // namespace core::http
