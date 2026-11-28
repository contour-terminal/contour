// SPDX-License-Identifier: Apache-2.0
//
// `Connection` over an in-memory socket pair on a hand-driven loop: heads and bodies in both
// directions, the framing fields the connection writes itself, what a peer that closes or falls
// silent is reported as in each role, the window a streamed body is held to, and when a connection
// may carry another message. Every flow runs on a `TestLoop` over a `ManualClock`, so nothing here
// waits on anything but the loop the case drives, and a flow that never finishes leaves its answer
// empty instead of hanging the case.
#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Connection.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpTestSupport.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using core::async::Task;
using core::http::BodyChunk;
using core::http::BodyEnd;
using core::http::BodyFraming;
using core::http::BufferBodySource;
using core::http::Connection;
using core::http::Fields;
using core::http::Framing;
using core::http::HttpErrorCode;
using core::http::HttpLimits;
using core::http::HttpResult;
using core::http::HttpVersion;
using core::http::IBodySource;
using core::http::MessageRole;
using core::http::Method;
using core::http::Persistence;
using core::http::RequestHead;
using core::http::RequestTarget;
using core::http::ResponseHead;
using core::http::Reuse;
using core::http::TrailerIntent;
using core::http::WireTimeouts;
using core::http::test::into;
using core::http::test::readAll;
using core::http::test::ReadBody;
using core::http::test::readUntilClosed;
using core::http::test::SlowBodySource;
using core::http::test::writeAndClose;
using core::http::test::writeOnly;
using core::net::ISocket;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::testing::InMemorySocketPair;
using core::net::testing::SocketDecorator;
using core::net::testing::TestLoop;
using core::platform::ManualClock;

using namespace std::chrono_literals;

namespace
{

/// A loop driven by hand, over a clock moved by hand, and a pair whose directions each hold at most
/// 16 bytes in flight, so every body larger than that streams in steps.
struct Wire
{
    ManualClock clock {};
    TestLoop loop { clock };
    InMemorySocketPair pair = InMemorySocketPair::create(16);
};

/// @return A request head, its fields appended in order; a field the factories refuse fails the case.
RequestHead requestHead(std::string_view method,
                        std::string_view target,
                        std::vector<std::pair<std::string, std::string>> const& fields,
                        HttpVersion version = HttpVersion::Http11)
{
    auto head = RequestHead {};
    auto parsedMethod = Method::parse(method);
    REQUIRE(parsedMethod.has_value());
    head.method = std::move(*parsedMethod);
    auto parsedTarget = RequestTarget::parse(target);
    REQUIRE(parsedTarget.has_value());
    head.target = std::move(*parsedTarget);
    head.version = version;
    for (auto const& [name, value]: fields)
        REQUIRE(head.fields.append(name, value).has_value());
    return head;
}

/// @return A response head of @p status with @p fields.
ResponseHead responseHead(std::uint16_t status,
                          std::vector<std::pair<std::string, std::string>> const& fields)
{
    auto head = ResponseHead {};
    head.status = status;
    for (auto const& [name, value]: fields)
        REQUIRE(head.fields.append(name, value).has_value());
    return head;
}

/// @return @p fields as one list; an invalid field fails the case.
Fields fieldsOf(std::vector<std::pair<std::string, std::string>> const& fields)
{
    auto out = Fields {};
    for (auto const& [name, value]: fields)
        REQUIRE(out.append(name, value).has_value());
    return out;
}

/// @return @p size bytes of a repeating, position-dependent pattern, so a lost or reordered byte shows.
std::string patternOf(std::size_t size)
{
    auto text = std::string(size, '\0');
    auto index = std::size_t { 0 };
    for (auto& octet: text)
        octet = static_cast<char>('a' + (index++ % 26));
    return text;
}

/// What a server end read of one request: the head, the body and its trailers.
struct ServedRequest
{
    std::optional<RequestHead> head;
    ReadBody body;
    std::optional<Fields> trailers;
    std::optional<HttpErrorCode> error;
};

/// The server end of one exchange: reads a request head, frames it, and reads the body to its end.
Task<ServedRequest> serveOne(Connection* connection, std::size_t maxBytes)
{
    auto out = ServedRequest {};
    auto head = co_await connection->readRequestHead();
    if (!head.has_value())
    {
        out.error = head.error().code;
        co_return out;
    }
    auto framing = core::http::requestFraming(head->version, head->fields);
    if (!framing.has_value())
    {
        out.error = framing.error().code;
        co_return out;
    }
    out.head = std::move(*head);
    auto reader = connection->bodyReader(*framing);
    out.body = co_await readAll(reader.get(), maxBytes);
    if (out.body.error.has_value())
        co_return out;
    auto trailers = co_await reader->trailers();
    if (trailers.has_value())
        out.trailers = std::move(*trailers);
    else
        out.error = trailers.error().code;
    co_return out;
}

/// The client end of one exchange: reads a response head to @p method and its body to the end.
struct ReadResponse
{
    std::optional<ResponseHead> head;
    ReadBody body;
    std::optional<Fields> trailers;
    std::optional<HttpErrorCode> error;
};

Task<ReadResponse> receiveOne(Connection* connection, std::string method, std::size_t maxBytes)
{
    auto out = ReadResponse {};
    auto head = co_await connection->readResponseHead();
    if (!head.has_value())
    {
        out.error = head.error().code;
        co_return out;
    }
    auto framing = core::http::responseFraming(method, head->version, head->status, head->fields);
    if (!framing.has_value())
    {
        out.error = framing.error().code;
        co_return out;
    }
    out.head = std::move(*head);
    auto reader = connection->bodyReader(*framing);
    out.body = co_await readAll(reader.get(), maxBytes);
    if (out.body.error.has_value())
        co_return out;
    auto trailers = co_await reader->trailers();
    if (trailers.has_value())
        out.trailers = std::move(*trailers);
    else
        out.error = trailers.error().code;
    co_return out;
}

/// The client end of one upload: the request head, then the body, under one framing.
Task<HttpResult<void>> sendRequest(Connection* connection,
                                   RequestHead head,
                                   IBodySource* body,
                                   Framing framing)
{
    auto wrote = co_await connection->writeRequestHead(std::move(head), framing);
    if (!wrote.has_value())
        co_return wrote;
    co_return co_await connection->writeBody(body, framing);
}

/// The client end of an upload that then closes, so the peer reads exactly what was written.
Task<HttpResult<void>> sendRequestThenClose(Connection* connection, IBodySource* body, Framing framing)
{
    // The head is made before the co_await, not in its full-expression: MSVC at /O2 cannot keep the
    // tail call a resumption needs past a call there (C4737).
    auto head = requestHead("PUT", "/", { { "Host", "h" } });
    auto sending = sendRequest(connection, std::move(head), body, framing);
    auto result = co_await std::move(sending);
    connection->close();
    co_return result;
}

/// Writes a request head and @p body under the body's own framing, and closes the socket.
Task<HttpResult<void>> bodyThenClose(Connection* connection, IBodySource* body)
{
    auto head = requestHead("PUT", "/", { { "Host", "h" } });
    auto const framing = core::http::framingFor(body);
    auto sending = sendRequest(connection, std::move(head), body, framing);
    auto result = co_await std::move(sending);
    connection->close();
    co_return result;
}

/// Writes a request head announcing a length of 3, then a body under a chunked framing.
Task<HttpResult<void>> headThenOtherBody(Connection* connection, IBodySource* body)
{
    auto head = co_await connection->writeRequestHead(
        requestHead("PUT", "/", { { "Host", "h" } }),
        Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} });
    if (!head.has_value())
        co_return head;
    co_return co_await connection->writeBody(
        body, Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} });
}

/// The server end of one reply: the response head, then the body, under one framing.
Task<HttpResult<void>> sendResponse(Connection* connection,
                                    ResponseHead head,
                                    IBodySource* body,
                                    Framing framing)
{
    auto wrote = co_await connection->writeResponseHead(std::move(head), framing);
    if (!wrote.has_value())
        co_return wrote;
    co_return co_await connection->writeBody(body, framing);
}

/// Writes @p head under @p framing and closes the socket, so the peer reads exactly what was written.
Task<HttpResult<void>> requestHeadThenClose(Connection* connection, RequestHead head, Framing framing)
{
    auto wrote = co_await connection->writeRequestHead(std::move(head), framing);
    connection->close();
    co_return wrote;
}

Task<HttpResult<void>> responseHeadThenClose(Connection* connection, ResponseHead head, Framing framing)
{
    auto wrote = co_await connection->writeResponseHead(std::move(head), framing);
    connection->close();
    co_return wrote;
}

/// Records the largest buffer any read was given: the most the connection asks the socket for.
class WindowRecordingSocket final: public SocketDecorator
{
  public:
    explicit WindowRecordingSocket(ISocket& inner, std::size_t* largest) noexcept:
        SocketDecorator { inner }, _largest { largest }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        *_largest = std::max(*_largest, buffer.size());
        return inner().read(buffer);
    }

  private:
    std::size_t* _largest;
};

/// A socket whose every read and write fails with one code, at once.
class FailingSocket final: public SocketDecorator
{
  public:
    FailingSocket(ISocket& inner, NetErrorCode code) noexcept: SocketDecorator { inner }, _code { code } {}

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> /*buffer*/) override
    {
        return core::net::IoAwaitable { std::unexpected(
            NetError { .code = _code, .systemCode = 0, .context = {} }) };
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(
        std::span<std::span<std::byte const> const> /*segments*/,
        std::shared_ptr<void const> /*keepAlive*/) override
    {
        return core::net::IoAwaitable { std::unexpected(
            NetError { .code = _code, .systemCode = 0, .context = {} }) };
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> /*buffer*/) override
    {
        return core::net::IoAwaitable { std::unexpected(
            NetError { .code = _code, .systemCode = 0, .context = {} }) };
    }

  private:
    NetErrorCode _code;
};

/// A body that streams @p chunks in order and does not say how long it is.
class ChunksBodySource final: public IBodySource
{
  public:
    /// @param chunks What @c next hands out, in order.
    /// @param claimedLength What @c knownLength answers, which a lying source makes differ.
    explicit ChunksBodySource(std::vector<std::string> chunks,
                              std::optional<std::uint64_t> claimedLength = {}):
        _chunks { std::move(chunks) }, _claimedLength { claimedLength }
    {
    }

    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override
    {
        return _claimedLength;
    }
    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::None; }

    [[nodiscard]] Task<HttpResult<BodyChunk>> next() override
    {
        auto chunk = BodyChunk {};
        if (_next < _chunks.size())
        {
            auto const& text = _chunks.at(_next++);
            auto const bytes = core::http::test::bytesOf(text);
            chunk.bytes.assign(bytes.begin(), bytes.end());
        }
        chunk.end = _next < _chunks.size() ? BodyEnd::More : BodyEnd::Reached;
        co_return chunk;
    }

    [[nodiscard]] Task<HttpResult<Fields>> trailers() override { co_return Fields {}; }

  private:
    std::vector<std::string> _chunks;
    std::optional<std::uint64_t> _claimedLength;
    std::size_t _next = 0;
};

constexpr auto NoTimeouts = WireTimeouts {};

} // namespace

TEST_CASE("Connection.chunkedBodyWithTrailersStreamsAcross", "[http][connection]")
{
    // Both directions at once: the client end writes a 1000-byte chunked upload with a trailer, the
    // server end reads it 100 bytes at a time through a 64-byte window. The pair holds 16 bytes in
    // flight per direction, so the body crosses in dozens of steps.
    auto wire = Wire {};
    auto largestRead = std::size_t { 0 };
    auto serverSocket = std::make_unique<WindowRecordingSocket>(*wire.pair.server, &largestRead);
    auto serverLimits = HttpLimits {};
    serverLimits.maxReadChunk = 64;
    auto server =
        Connection { wire.loop, std::move(serverSocket), MessageRole::Request, serverLimits, NoTimeouts };
    auto client = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };

    auto const body = patternOf(1000);
    auto source = BufferBodySource { body, fieldsOf({ { "X-Checksum", "abc" } }) };
    REQUIRE(core::http::framingFor(&source).kind == BodyFraming::Chunked);

    auto sent = std::optional<HttpResult<void>> {};
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(sendRequest(&client,
                                     requestHead("POST", "/upload", { { "Host", "example.test" } }),
                                     &source,
                                     core::http::framingFor(&source)),
                         &sent));
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();

    REQUIRE(sent.has_value());
    CHECK(sent->has_value());
    REQUIRE(served.has_value());
    REQUIRE_FALSE(served->error.has_value());
    REQUIRE_FALSE(served->body.error.has_value());
    CHECK(served->head->method.text() == "POST");
    CHECK(served->body.text == body);
    CHECK(std::ranges::all_of(served->body.chunkSizes, [](std::size_t size) { return size <= 64; }));
    CHECK(largestRead <= 64);
    CHECK(largestRead > 0);
    REQUIRE(served->trailers.has_value());
    auto const checksum = served->trailers->get("x-checksum");
    REQUIRE(checksum.size() == 1);
    CHECK(checksum.front() == "abc");
    CHECK_FALSE(server.hasLiveBodyReader());
    CHECK(server.reusable() == Reuse::Yes);
    CHECK(client.reusable() == Reuse::Yes);
}

TEST_CASE("Connection.lengthBodyRoundTripsBothWays", "[http][connection]")
{
    // A request with a Length body one way, a response with one the other, on one connection pair.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    auto client = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };

    auto upload = BufferBodySource { patternOf(300) };
    REQUIRE(core::http::framingFor(&upload).kind == BodyFraming::Length);
    REQUIRE(core::http::framingFor(&upload).length == 300);
    auto sent = std::optional<HttpResult<void>> {};
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(sendRequest(&client,
                                     requestHead("PUT", "/thing", { { "Host", "example.test" } }),
                                     &upload,
                                     core::http::framingFor(&upload)),
                         &sent));
    wire.loop.spawn(into(serveOne(&server, 1000), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(sent.has_value());
    CHECK(sent->has_value());
    REQUIRE(served.has_value());
    REQUIRE_FALSE(served->error.has_value());
    CHECK(served->body.text == patternOf(300));
    REQUIRE(served->trailers.has_value());
    CHECK(served->trailers->entries().empty());

    auto reply = BufferBodySource { "done" };
    auto replied = std::optional<HttpResult<void>> {};
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(
        into(sendResponse(&server, responseHead(200, {}), &reply, core::http::framingFor(&reply)), &replied));
    wire.loop.spawn(into(receiveOne(&client, "PUT", 1000), &received));
    std::ignore = wire.loop.drain();
    REQUIRE(replied.has_value());
    CHECK(replied->has_value());
    REQUIRE(received.has_value());
    REQUIRE_FALSE(received->error.has_value());
    CHECK(received->head->status == 200);
    CHECK(received->body.text == "done");
    CHECK(server.reusable() == Reuse::Yes);
    CHECK(client.reusable() == Reuse::Yes);
}

TEST_CASE("Connection.peerClosingMidBodyIsReportedByRole", "[http][connection]")
{
    // The same truncation is the peer's broken promise in a response and a malformed request in a
    // server's eyes: ResponseIncomplete and ProtocolError. Either way the connection is done.
    struct Row
    {
        std::string_view name;
        MessageRole role;
        std::string_view wire;
        HttpErrorCode code;
    };
    constexpr auto Rows = std::array {
        Row { "response, length",
              MessageRole::Response,
              "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabcd",
              HttpErrorCode::ResponseIncomplete },
        Row { "response, chunked",
              MessageRole::Response,
              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\na\r\nabcd",
              HttpErrorCode::ResponseIncomplete },
        Row { "response, chunked, in the trailers",
              MessageRole::Response,
              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\nX-A: b\r\n",
              HttpErrorCode::ResponseIncomplete },
        Row { "request, length",
              MessageRole::Request,
              "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nabcd",
              HttpErrorCode::ProtocolError },
        Row { "request, chunked",
              MessageRole::Request,
              "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\na\r\nabcd",
              HttpErrorCode::ProtocolError },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto connection =
                Connection { wire.loop, std::move(wire.pair.server), row.role, HttpLimits {}, NoTimeouts };
            wire.loop.spawn(writeAndClose(wire.pair.client.get(), std::string { row.wire }));
            auto bodyError = std::optional<HttpErrorCode> {};
            if (row.role == MessageRole::Response)
            {
                auto received = std::optional<ReadResponse> {};
                wire.loop.spawn(into(receiveOne(&connection, "GET", 1000), &received));
                std::ignore = wire.loop.drain();
                REQUIRE(received.has_value());
                REQUIRE(received->head.has_value());
                REQUIRE(received->body.error.has_value());
                bodyError = received->body.error->code;
            }
            else
            {
                auto served = std::optional<ServedRequest> {};
                wire.loop.spawn(into(serveOne(&connection, 1000), &served));
                std::ignore = wire.loop.drain();
                REQUIRE(served.has_value());
                REQUIRE(served->head.has_value());
                REQUIRE(served->body.error.has_value());
                bodyError = served->body.error->code;
            }
            CHECK(bodyError == row.code);
            CHECK(connection.reusable() == Reuse::No);
        }
    }
}

TEST_CASE("Connection.peerClosingInOrBeforeAHead", "[http][connection]")
{
    // A close before any byte of a head is the connection ending, which a server sees between
    // keep-alive requests and a client sees on a stale pooled connection; a close inside a head is
    // the role's incomplete-message code.
    struct Row
    {
        std::string_view name;
        MessageRole role;
        std::string_view wire;
        HttpErrorCode code;
    };
    constexpr auto Rows = std::array {
        Row { "response, before", MessageRole::Response, "", HttpErrorCode::ConnectionTerminated },
        Row { "request, before", MessageRole::Request, "", HttpErrorCode::ConnectionTerminated },
        Row { "response, inside",
              MessageRole::Response,
              "HTTP/1.1 200 OK\r\nA: b",
              HttpErrorCode::ResponseIncomplete },
        Row { "request, inside", MessageRole::Request, "GET / HTTP/1.1\r\nHo", HttpErrorCode::ProtocolError },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto connection =
                Connection { wire.loop, std::move(wire.pair.server), row.role, HttpLimits {}, NoTimeouts };
            wire.loop.spawn(writeAndClose(wire.pair.client.get(), std::string { row.wire }));
            auto code = std::optional<HttpErrorCode> {};
            if (row.role == MessageRole::Response)
            {
                auto received = std::optional<ReadResponse> {};
                wire.loop.spawn(into(receiveOne(&connection, "GET", 1000), &received));
                std::ignore = wire.loop.drain();
                REQUIRE(received.has_value());
                code = received->error;
            }
            else
            {
                auto served = std::optional<ServedRequest> {};
                wire.loop.spawn(into(serveOne(&connection, 1000), &served));
                std::ignore = wire.loop.drain();
                REQUIRE(served.has_value());
                code = served->error;
            }
            CHECK(code == row.code);
            CHECK(connection.reusable() == Reuse::No);
        }
    }
}

TEST_CASE("Connection.readIsClampedToMaxReadChunk", "[http][connection]")
{
    // A guest may ask for a gigabyte; the host hands out at most maxReadChunk, whatever the framing.
    constexpr auto Rows = std::array<std::pair<std::string_view, std::string_view>, 2> {
        std::pair { "length", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" },
        std::pair { "chunked", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n64\r\n" },
    };
    for (auto const& [name, head]: Rows)
    {
        DYNAMIC_SECTION(name)
        {
            auto wire = Wire {};
            auto limits = HttpLimits {};
            limits.maxReadChunk = 32;
            auto connection = Connection {
                wire.loop, std::move(wire.pair.client), MessageRole::Response, limits, NoTimeouts
            };
            wire.loop.spawn(writeOnly(wire.pair.server.get(), std::string { head } + patternOf(100)));
            auto received = std::optional<ReadResponse> {};
            wire.loop.spawn(into(receiveOne(&connection, "GET", std::size_t { 1 } << 30), &received));
            std::ignore = wire.loop.drain();
            wire.pair.server->close();
            std::ignore = wire.loop.drain();
            REQUIRE(received.has_value());
            REQUIRE(received->head.has_value());
            REQUIRE_FALSE(received->body.chunkSizes.empty());
            CHECK(
                std::ranges::all_of(received->body.chunkSizes, [](std::size_t size) { return size <= 32; }));
            CHECK(received->body.text.starts_with(patternOf(100).substr(0, 32)));
        }
    }
}

TEST_CASE("Connection.aSmallReadTakesOnlyWhatItAsks", "[http][connection]")
{
    // Bytes past a small read stay in the connection's buffer for the next one, in order.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(
        writeOnly(wire.pair.server.get(),
                  "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0a\r\n0123456789\r\n0\r\n\r\n"));
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 3), &received));
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    REQUIRE_FALSE(received->body.error.has_value());
    CHECK(received->body.text == "0123456789");
    CHECK(std::ranges::all_of(received->body.chunkSizes, [](std::size_t size) { return size <= 3; }));
    CHECK(connection.reusable() == Reuse::Yes);
}

TEST_CASE("Connection.aCallerFramingFieldIsRefused", "[http][connection]")
{
    // The connection writes every framing field itself; one the caller brought could contradict it,
    // so it is refused before a byte is written.
    for (auto const name:
         { "Content-Length", "transfer-encoding", "Connection", "Keep-Alive", "TE", "Upgrade" })
    {
        DYNAMIC_SECTION(name)
        {
            SECTION("in a request head")
            {
                auto wire = Wire {};
                auto client = Connection {
                    wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
                };
                auto wrote = std::optional<HttpResult<void>> {};
                auto peer = std::optional<std::string> {};
                wire.loop.spawn(into(
                    requestHeadThenClose(
                        &client, requestHead("GET", "/", { { "Host", "h" }, { name, "5" } }), Framing {}),
                    &wrote));
                wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
                std::ignore = wire.loop.drain();
                REQUIRE(wrote.has_value());
                REQUIRE_FALSE(wrote->has_value());
                CHECK(wrote->error().code == HttpErrorCode::ProtocolError);
                REQUIRE(peer.has_value());
                CHECK(peer->empty());
            }
            SECTION("then a corrected head")
            {
                // A refusal before the first wire byte leaves the connection as it was: the handler
                // that added the field can still send its 500, and the connection stays reusable.
                auto wire = Wire {};
                auto server = Connection {
                    wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
                };
                wire.loop.spawn(writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
                auto served = std::optional<ServedRequest> {};
                wire.loop.spawn(into(serveOne(&server, 100), &served));
                std::ignore = wire.loop.drain();
                REQUIRE(served.has_value());
                REQUIRE(served->head.has_value());

                auto peer = std::optional<std::string> {};
                wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
                auto refused = std::optional<HttpResult<void>> {};
                wire.loop.spawn(into(
                    server.writeResponseHead(responseHead(200, { { name, "5" } }), Framing {}), &refused));
                std::ignore = wire.loop.drain();
                REQUIRE(refused.has_value());
                REQUIRE_FALSE(refused->has_value());
                CHECK(refused->error().code == HttpErrorCode::ProtocolError);
                auto corrected = std::optional<HttpResult<void>> {};
                wire.loop.spawn(
                    into(server.writeResponseHead(responseHead(500, {}), Framing {}), &corrected));
                std::ignore = wire.loop.drain();
                REQUIRE(corrected.has_value());
                CHECK(corrected->has_value());
                CHECK(server.reusable() == Reuse::Yes);
                server.close();
                std::ignore = wire.loop.drain();
                REQUIRE(peer.has_value());
                CHECK(*peer == "HTTP/1.1 500 \r\nContent-Length: 0\r\n\r\n");
            }
            SECTION("in a response head")
            {
                auto wire = Wire {};
                auto server = Connection {
                    wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
                };
                auto wrote = std::optional<HttpResult<void>> {};
                auto peer = std::optional<std::string> {};
                wire.loop.spawn(
                    into(responseHeadThenClose(&server, responseHead(200, { { name, "5" } }), Framing {}),
                         &wrote));
                wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
                std::ignore = wire.loop.drain();
                REQUIRE(wrote.has_value());
                REQUIRE_FALSE(wrote->has_value());
                CHECK(wrote->error().code == HttpErrorCode::ProtocolError);
                REQUIRE(peer.has_value());
                CHECK(peer->empty());
            }
        }
    }
}

TEST_CASE("Connection.headsCarryTheFramingFieldsItWrites", "[http][connection]")
{
    // What each framing puts on the wire, exactly; and the framings a head cannot carry.
    struct Row
    {
        std::string_view name;
        HttpVersion version;
        Framing framing;
        std::optional<std::string_view> wire; ///< Empty when the head is refused.
    };
    auto const rows = std::array {
        Row { "none", HttpVersion::Http11, Framing {}, "GET / HTTP/1.1\r\nHost: h\r\n\r\n" },
        Row { "length",
              HttpVersion::Http11,
              Framing { .kind = BodyFraming::Length, .length = 5, .persistence = {} },
              "GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n" },
        Row { "chunked",
              HttpVersion::Http11,
              Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} },
              "GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n" },
        Row { "close",
              HttpVersion::Http11,
              Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close },
              "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n" },
        Row { "HTTP/1.0 keep-alive",
              HttpVersion::Http10,
              Framing {},
              "GET / HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n" },
        Row { "HTTP/1.0 close",
              HttpVersion::Http10,
              Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close },
              "GET / HTTP/1.0\r\nHost: h\r\n\r\n" },
        Row { "HTTP/1.0 chunked",
              HttpVersion::Http10,
              Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} },
              std::nullopt },
        Row { "until close",
              HttpVersion::Http11,
              Framing { .kind = BodyFraming::UntilClose, .length = 0, .persistence = Persistence::Close },
              std::nullopt },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto client = Connection {
                wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
            };
            auto wrote = std::optional<HttpResult<void>> {};
            auto peer = std::optional<std::string> {};
            wire.loop.spawn(
                into(requestHeadThenClose(
                         &client, requestHead("GET", "/", { { "Host", "h" } }, row.version), row.framing),
                     &wrote));
            wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
            std::ignore = wire.loop.drain();
            REQUIRE(wrote.has_value());
            REQUIRE(peer.has_value());
            if (row.wire.has_value())
            {
                CHECK(wrote->has_value());
                CHECK(*peer == *row.wire);
            }
            else
            {
                REQUIRE_FALSE(wrote->has_value());
                CHECK(wrote->error().code == HttpErrorCode::ProtocolError);
                CHECK(peer->empty());
            }
        }
    }
}

TEST_CASE("Connection.responseHeadsFollowTheirStatus", "[http][connection]")
{
    // A response with no body still says so unless its status says it for it; a status that has no
    // body refuses a body framing.
    struct Row
    {
        std::string_view name;
        std::uint16_t status;
        Framing framing;
        std::optional<std::string_view> wire;
    };
    auto const rows = std::array {
        Row { "200 none", 200, Framing {}, "HTTP/1.1 200 \r\nContent-Length: 0\r\n\r\n" },
        Row { "204 none", 204, Framing {}, "HTTP/1.1 204 \r\n\r\n" },
        Row { "304 none", 304, Framing {}, "HTTP/1.1 304 \r\n\r\n" },
        Row { "101 none", 101, Framing {}, "HTTP/1.1 101 \r\n\r\n" },
        Row { "204 length",
              204,
              Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} },
              std::nullopt },
        Row { "100 chunked",
              100,
              Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} },
              std::nullopt },
        Row { "304 length",
              304,
              Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} },
              "HTTP/1.1 304 \r\nContent-Length: 3\r\n\r\n" },
        Row { "304 chunked",
              304,
              Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} },
              std::nullopt },
        Row { "200 until close",
              200,
              Framing { .kind = BodyFraming::UntilClose, .length = 0, .persistence = Persistence::Close },
              "HTTP/1.1 200 \r\nConnection: close\r\n\r\n" },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto server = Connection {
                wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
            };
            auto wrote = std::optional<HttpResult<void>> {};
            auto peer = std::optional<std::string> {};
            wire.loop.spawn(
                into(responseHeadThenClose(&server, responseHead(row.status, {}), row.framing), &wrote));
            wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
            std::ignore = wire.loop.drain();
            REQUIRE(wrote.has_value());
            REQUIRE(peer.has_value());
            if (row.wire.has_value())
            {
                CHECK(wrote->has_value());
                CHECK(*peer == *row.wire);
            }
            else
            {
                REQUIRE_FALSE(wrote->has_value());
                CHECK(wrote->error().code == HttpErrorCode::ProtocolError);
                CHECK(peer->empty());
            }
        }
    }
}

TEST_CASE("Connection.aBodyThatBreaksItsLengthIsRefused", "[http][connection]")
{
    // A body longer than its Length framing would put bytes on the wire that the peer reads as the
    // next message; shorter, it would leave the peer waiting. Both are the writer's body-size code,
    // and not one byte past the framing reaches the peer.
    auto const head = std::string { "PUT / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n" };
    auto const length5 = Framing { .kind = BodyFraming::Length, .length = 5, .persistence = {} };

    SECTION("a known length of 6 is refused before any body byte")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = BufferBodySource { "abcdef" };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(sendRequestThenClose(&client, &source, length5), &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::RequestBodySize);
        REQUIRE(peer.has_value());
        CHECK(*peer == head);
        CHECK(client.reusable() == Reuse::No);
    }
    SECTION("a streamed body that runs past the length stops at the chunk that would pass it")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = ChunksBodySource { { "abc", "def" } };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(sendRequestThenClose(&client, &source, length5), &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::RequestBodySize);
        REQUIRE(peer.has_value());
        CHECK(*peer == head + "abc");
        CHECK(client.reusable() == Reuse::No);
    }
    SECTION("a streamed body that ends short of the length")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = ChunksBodySource { { "ab", "cd" } };
        auto sent = std::optional<HttpResult<void>> {};
        wire.loop.spawn(into(
            sendRequest(&client, requestHead("PUT", "/", { { "Host", "h" } }), &source, length5), &sent));
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::RequestBodySize);
        CHECK(sent->error().size == std::uint64_t { 4 });
        CHECK(client.reusable() == Reuse::No);
        client.close();
        std::ignore = wire.loop.drain();
    }
    SECTION("a server's reply is held to the response's code")
    {
        auto wire = Wire {};
        auto server = Connection {
            wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
        };
        auto source = BufferBodySource { "abcdef" };
        auto sent = std::optional<HttpResult<void>> {};
        wire.loop.spawn(into(sendResponse(&server, responseHead(200, {}), &source, length5), &sent));
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
        std::ignore = wire.loop.drain();
        server.close();
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::ResponseBodySize);
        REQUIRE(peer.has_value());
        CHECK(*peer == "HTTP/1.1 200 \r\nContent-Length: 5\r\n\r\n");
    }
}

TEST_CASE("Connection.trailersNeedAChunkedFramingAndNoFramingField", "[http][connection]")
{
    // Trailers can only ride a chunked body, and a framing field among them would reframe the
    // message after the fact.
    SECTION("a trailer source under a Length framing")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = BufferBodySource { "abc", fieldsOf({ { "X-A", "b" } }) };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        wire.loop.spawn(
            into(sendRequest(&client,
                             requestHead("PUT", "/", { { "Host", "h" } }),
                             &source,
                             Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} }),
                 &sent));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::ProtocolError);
        CHECK(client.reusable() == Reuse::No);
        client.close();
        std::ignore = wire.loop.drain();
        REQUIRE(peer.has_value());
        CHECK(peer->ends_with("Content-Length: 3\r\n\r\n")); // the head only, no body byte
    }
    SECTION("a framing field among the trailers")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = BufferBodySource { "abc", fieldsOf({ { "Content-Length", "3" } }) };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(bodyThenClose(&client, &source), &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::ProtocolError);
        REQUIRE(peer.has_value());
        CHECK_FALSE(peer->contains("Content-Length"));
    }
}

TEST_CASE("Connection.trailersCountAgainstTheLimits", "[http][connection]")
{
    // The connection's limits reach the chunked decoder: a trailer section past maxFields is the
    // role's trailer-section code, as a head past it is the head's.
    auto wire = Wire {};
    auto limits = HttpLimits {};
    limits.maxFields = 2;
    auto connection =
        Connection { wire.loop, std::move(wire.pair.client), MessageRole::Response, limits, NoTimeouts };
    wire.loop.spawn(writeAndClose(wire.pair.server.get(),
                                  "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                  "1\r\na\r\n0\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n"));
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &received));
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    REQUIRE(received->body.error.has_value());
    CHECK(received->body.error->code == HttpErrorCode::ResponseTrailerSectionSize);
    CHECK(connection.reusable() == Reuse::No);
}

TEST_CASE("Connection.aLengthBodyIsHeldToMaxBodyBytes", "[http][connection]")
{
    // The chunked decoder holds a chunked body to maxBodyBytes; a declared length is held to it here.
    auto wire = Wire {};
    auto limits = HttpLimits {};
    limits.maxBodyBytes = 50;
    auto connection =
        Connection { wire.loop, std::move(wire.pair.server), MessageRole::Request, limits, NoTimeouts };
    wire.loop.spawn(
        writeOnly(wire.pair.client.get(), "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 51\r\n\r\n"));
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&connection, 100), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(served.has_value());
    REQUIRE(served->body.error.has_value());
    CHECK(served->body.error->code == HttpErrorCode::RequestBodySize);
    CHECK(served->body.error->size == std::uint64_t { 51 });
    CHECK(connection.reusable() == Reuse::No);
    wire.pair.client->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aSilentPeerTimesOut", "[http][connection]")
{
    // Each deadline is asserted parked before the clock moves, so a read that never waited passes
    // nothing. The first byte of a response head is the response's own timeout; every other wait is
    // a read timeout.
    struct Row
    {
        std::string_view name;
        MessageRole role;
        WireTimeouts timeouts;
        std::string_view sent; ///< What the peer sends before it falls silent.
        HttpErrorCode code;
    };
    constexpr auto Rows = std::array {
        Row { "request head, first byte",
              MessageRole::Request,
              WireTimeouts { .firstByte = 100ms, .betweenBytes = 0ms },
              "",
              HttpErrorCode::ConnectionReadTimeout },
        Row { "response head, first byte",
              MessageRole::Response,
              WireTimeouts { .firstByte = 100ms, .betweenBytes = 0ms },
              "",
              HttpErrorCode::ResponseTimeout },
        Row { "response head, between bytes",
              MessageRole::Response,
              WireTimeouts { .firstByte = 0ms, .betweenBytes = 100ms },
              "HTTP/1.1 200 OK\r\n",
              HttpErrorCode::ConnectionReadTimeout },
        Row { "response body, between bytes",
              MessageRole::Response,
              WireTimeouts { .firstByte = 0ms, .betweenBytes = 100ms },
              "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nab",
              HttpErrorCode::ConnectionReadTimeout },
        Row { "request body, between bytes",
              MessageRole::Request,
              WireTimeouts { .firstByte = 0ms, .betweenBytes = 100ms },
              "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\nab",
              HttpErrorCode::ConnectionReadTimeout },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto connection =
                Connection { wire.loop, std::move(wire.pair.server), row.role, HttpLimits {}, row.timeouts };
            if (!row.sent.empty())
                wire.loop.spawn(writeOnly(wire.pair.client.get(), std::string { row.sent }));
            auto code = std::optional<HttpErrorCode> {};
            auto received = std::optional<ReadResponse> {};
            auto served = std::optional<ServedRequest> {};
            if (row.role == MessageRole::Response)
                wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &received));
            else
                wire.loop.spawn(into(serveOne(&connection, 100), &served));
            std::ignore = wire.loop.drain();
            REQUIRE_FALSE(received.has_value());
            REQUIRE_FALSE(served.has_value());

            wire.clock.advance(99ms);
            std::ignore = wire.loop.drain();
            REQUIRE_FALSE(received.has_value());
            REQUIRE_FALSE(served.has_value());

            wire.clock.advance(1ms);
            std::ignore = wire.loop.drain();
            if (row.role == MessageRole::Response)
            {
                REQUIRE(received.has_value());
                code = received->body.error.has_value() ? std::optional { received->body.error->code }
                                                        : received->error;
            }
            else
            {
                REQUIRE(served.has_value());
                code = served->body.error.has_value() ? std::optional { served->body.error->code }
                                                      : served->error;
            }
            CHECK(code == row.code);
            CHECK(connection.reusable() == Reuse::No);
        }
    }
}

namespace
{

/// Writes @p prefix, then @p text one byte every @p gap on @p loop's clock, each write awaited.
Task<void> trickleAfter(core::net::EventLoop* loop,
                        ISocket* socket,
                        std::string prefix,
                        std::string text,
                        std::chrono::milliseconds gap)
{
    if (!prefix.empty() && !(co_await socket->write(core::http::test::bytesOf(prefix))).has_value())
        co_return;
    for (auto const octet: text)
    {
        auto const one = std::string(1, octet);
        if (!(co_await socket->write(core::http::test::bytesOf(one))).has_value())
            co_return;
        co_await loop->delay(gap);
    }
}

/// A transport whose reads always beat their cancel: `cancelRead` reaches nothing, as a completion
/// transport's does when its receive has already finished (the data wins).
class UncancellableSocket final: public SocketDecorator
{
  public:
    explicit UncancellableSocket(ISocket& inner) noexcept: SocketDecorator { inner } {}

    void cancelRead() noexcept override {}
};

} // namespace

TEST_CASE("Connection.aHeadOrABodyAsAWholeIsBounded", "[http][connection]")
{
    // Every byte arrives inside betweenBytes; only the whole-message bound can end the trickle. Where
    // the cancel reaches nothing, the bound still holds at the first read after it: one byte late.
    enum class Cancel : std::uint8_t
    {
        Reaches,
        Ignored,
    };
    struct Row
    {
        std::string_view name;
        WireTimeouts timeouts;
        std::string_view prefix;
        Cancel cancel;
        std::chrono::milliseconds answeredAt;
    };
    constexpr auto Head = std::string_view {};
    constexpr auto Body = std::string_view { "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 40\r\n\r\n" };
    constexpr auto Rows = std::array {
        Row { "a head", WireTimeouts { .betweenBytes = 100ms, .head = 300ms }, Head, Cancel::Reaches, 300ms },
        Row { "a head whose read beats its cancel",
              WireTimeouts { .betweenBytes = 100ms, .head = 300ms },
              Head,
              Cancel::Ignored,
              360ms },
        Row { "a body", WireTimeouts { .betweenBytes = 100ms, .body = 300ms }, Body, Cancel::Reaches, 300ms },
        Row { "a body whose read beats its cancel",
              WireTimeouts { .betweenBytes = 100ms, .body = 300ms },
              Body,
              Cancel::Ignored,
              360ms },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto wire = Wire {};
            auto socket =
                row.cancel == Cancel::Ignored
                    ? std::unique_ptr<ISocket> { std::make_unique<UncancellableSocket>(*wire.pair.server) }
                    : std::unique_ptr<ISocket> { std::move(wire.pair.server) };
            auto connection = Connection {
                wire.loop, std::move(socket), MessageRole::Request, HttpLimits {}, row.timeouts
            };
            auto const trickle = row.prefix.empty()
                                     ? std::string { "GET / HTTP/1.1\r\nHost: h\r\nX-Slow: aaaaaaaaaa" }
                                     : std::string(20, 'x');
            wire.loop.spawn(
                trickleAfter(&wire.loop, wire.pair.client.get(), std::string { row.prefix }, trickle, 90ms));
            auto served = std::optional<ServedRequest> {};
            wire.loop.spawn(into(serveOne(&connection, 100), &served));
            std::ignore = wire.loop.drain();
            auto elapsed = 0ms;
            while (!served.has_value() && elapsed < 2000ms)
            {
                wire.clock.advance(10ms);
                elapsed += 10ms;
                std::ignore = wire.loop.drain();
            }
            REQUIRE(served.has_value());
            auto const code =
                served->body.error.has_value() ? std::optional { served->body.error->code } : served->error;
            CHECK(code == HttpErrorCode::ConnectionReadTimeout);
            CHECK(elapsed == row.answeredAt);
        }
    }
}

TEST_CASE("Connection.aZeroTimeoutIsNone", "[http][connection]")
{
    // R31: a zero duration means no deadline, not one that fires at once.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &received));
    std::ignore = wire.loop.drain();
    wire.clock.advance(24h);
    std::ignore = wire.loop.drain();
    REQUIRE_FALSE(received.has_value());

    wire.loop.spawn(writeAndClose(wire.pair.server.get(), "HTTP/1.1 204 No Content\r\n\r\n"));
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    CHECK_FALSE(received->error.has_value());
    REQUIRE(received->head.has_value());
    CHECK(received->head->status == 204);
    CHECK(connection.reusable() == Reuse::Yes); // a bodiless response is a finished one
}

TEST_CASE("Connection.anUploadInProgressCountsTowardTheResponseTimeout", "[http][connection]")
{
    // The response timeout follows wasmtime's first-byte timeout (P13 Task 16 ruling): it runs from
    // the head read's start, and a request body still being written -- here one chunk every three
    // timeouts -- does not hold it.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    auto client = Connection { wire.loop,
                               std::move(wire.pair.client),
                               MessageRole::Response,
                               HttpLimits {},
                               WireTimeouts { .firstByte = 100ms, .betweenBytes = 0ms, .write = 0ms } };
    auto upload = SlowBodySource { wire.loop, { "aaaa", "bbbb", "cccc" }, 300ms };
    auto const framing = core::http::framingFor(&upload);
    auto sent = std::optional<HttpResult<void>> {};
    auto received = std::optional<ReadResponse> {};
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(
        into(sendRequest(&client, requestHead("PUT", "/", { { "Host", "h" } }), &upload, framing), &sent));
    wire.loop.spawn(into(receiveOne(&client, "PUT", 100), &received));
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();
    wire.clock.advance(99ms);
    std::ignore = wire.loop.drain();
    REQUIRE_FALSE(received.has_value());
    wire.clock.advance(1ms);
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    CHECK(received->error == HttpErrorCode::ResponseTimeout);
    REQUIRE_FALSE(sent.has_value()); // the upload was still in progress
    for ([[maybe_unused]] auto const chunk: std::views::iota(0, 3))
    {
        wire.clock.advance(300ms);
        std::ignore = wire.loop.drain();
    }
    REQUIRE(sent.has_value());
    CHECK(sent->has_value());
}

TEST_CASE("Connection.aHeadWrittenAloneLeavesTheResponseTimeoutRunning", "[http][connection]")
{
    // Only a write in progress holds the timeout. A client that writes its head and waits for an
    // interim answer before the body (Expect: 100-continue) is waiting on the server, and is bounded.
    auto wire = Wire {};
    auto client = Connection { wire.loop,
                               std::move(wire.pair.client),
                               MessageRole::Response,
                               HttpLimits {},
                               WireTimeouts { .firstByte = 100ms, .betweenBytes = 0ms, .write = 0ms } };
    auto drained = std::optional<std::string> {};
    auto wrote = std::optional<HttpResult<void>> {};
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &drained));
    wire.loop.spawn(into(
        client.writeRequestHead(requestHead("PUT", "/", { { "Host", "h" }, { "Expect", "100-continue" } }),
                                Framing { .kind = BodyFraming::Length, .length = 4 }),
        &wrote));
    std::ignore = wire.loop.drain();
    REQUIRE(wrote.has_value());
    REQUIRE(wrote->has_value());
    wire.loop.spawn(into(receiveOne(&client, "PUT", 100), &received));
    std::ignore = wire.loop.drain();
    wire.clock.advance(99ms);
    std::ignore = wire.loop.drain();
    REQUIRE_FALSE(received.has_value());
    wire.clock.advance(1ms);
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    CHECK(received->error == HttpErrorCode::ResponseTimeout);
    client.close();
}

TEST_CASE("Connection.socketErrorsMapThroughTheTable", "[http][connection]")
{
    // A transport failure is classified once, by a row per NetErrorCode, and the direction decides
    // what a timeout means.
    struct Row
    {
        NetErrorCode net;
        HttpErrorCode onRead;
        HttpErrorCode onWrite;
    };
    constexpr auto Rows = std::array {
        Row { NetErrorCode::ConnReset,
              HttpErrorCode::ConnectionTerminated,
              HttpErrorCode::ConnectionTerminated },
        Row { NetErrorCode::Timeout,
              HttpErrorCode::ConnectionReadTimeout,
              HttpErrorCode::ConnectionWriteTimeout },
        Row { NetErrorCode::BadHandle,
              HttpErrorCode::ConnectionTerminated,
              HttpErrorCode::ConnectionTerminated },
        Row { NetErrorCode::HostUnreach,
              HttpErrorCode::DestinationUnavailable,
              HttpErrorCode::DestinationUnavailable },
        Row { NetErrorCode::SystemError, HttpErrorCode::InternalError, HttpErrorCode::InternalError },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(core::net::toString(row.net))
        {
            auto wire = Wire {};
            auto connection = Connection { wire.loop,
                                           std::make_unique<FailingSocket>(*wire.pair.client, row.net),
                                           MessageRole::Response,
                                           HttpLimits {},
                                           NoTimeouts };
            auto head = std::optional<HttpResult<ResponseHead>> {};
            auto wrote = std::optional<HttpResult<void>> {};
            wire.loop.spawn(into(connection.readResponseHead(), &head));
            wire.loop.spawn(
                into(connection.writeRequestHead(requestHead("GET", "/", { { "Host", "h" } }), Framing {}),
                     &wrote));
            std::ignore = wire.loop.drain();
            REQUIRE(head.has_value());
            REQUIRE_FALSE(head->has_value());
            CHECK(head->error().code == row.onRead);
            REQUIRE(wrote.has_value());
            REQUIRE_FALSE(wrote->has_value());
            CHECK(wrote->error().code == row.onWrite);
            CHECK(connection.reusable() == Reuse::No);
        }
    }
}

TEST_CASE("Connection.reuseFollowsTheExchange", "[http][connection]")
{
    SECTION("a second response in the same bytes is read after the first")
    {
        // Bytes past a body stay buffered for the next head; while they wait, the connection is not
        // one a pool may hand to a request they do not answer.
        auto wire = Wire {};
        auto connection = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        wire.loop.spawn(writeOnly(wire.pair.server.get(),
                                  "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcHTTP/1.1 201 "
                                  "Created\r\nContent-Length: 2\r\n\r\nde"));
        auto first = std::optional<ReadResponse> {};
        wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &first));
        std::ignore = wire.loop.drain();
        REQUIRE(first.has_value());
        CHECK(first->body.text == "abc");
        CHECK(connection.reusable() == Reuse::No);
        auto second = std::optional<ReadResponse> {};
        wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &second));
        std::ignore = wire.loop.drain();
        REQUIRE(second.has_value());
        REQUIRE(second->head.has_value());
        CHECK(second->head->status == 201);
        CHECK(second->body.text == "de");
        CHECK(connection.reusable() == Reuse::Yes);
        wire.pair.server->close();
        std::ignore = wire.loop.drain();
    }
    SECTION("a body left unread, and the same body read later")
    {
        auto wire = Wire {};
        auto connection = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        wire.loop.spawn(writeOnly(wire.pair.server.get(), "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"));
        auto head = std::optional<HttpResult<ResponseHead>> {};
        wire.loop.spawn(into(connection.readResponseHead(), &head));
        std::ignore = wire.loop.drain();
        REQUIRE(head.has_value());
        REQUIRE(head->has_value());
        CHECK(connection.reusable() == Reuse::No); // a head read, its body not yet

        auto const framing = Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} };
        {
            auto reader = connection.bodyReader(framing);
            CHECK(connection.hasLiveBodyReader());
            auto chunk = std::optional<HttpResult<BodyChunk>> {};
            wire.loop.spawn(into(reader->read(1), &chunk));
            std::ignore = wire.loop.drain();
            REQUIRE(chunk.has_value());
            REQUIRE(chunk->has_value());
            CHECK(core::http::test::textOf((*chunk)->bytes) == "a");
        }
        CHECK_FALSE(connection.hasLiveBodyReader());
        CHECK(connection.reusable() == Reuse::No); // two bytes unread

        auto reader = connection.bodyReader(framing);
        auto rest = std::optional<ReadBody> {};
        wire.loop.spawn(into(readAll(reader.get(), 100), &rest));
        std::ignore = wire.loop.drain();
        REQUIRE(rest.has_value());
        CHECK(rest->text == "bc");
        CHECK(connection.reusable() == Reuse::Yes);
        wire.pair.server->close();
        std::ignore = wire.loop.drain();
    }
    SECTION("persistence close")
    {
        auto wire = Wire {};
        auto connection = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        wire.loop.spawn(writeOnly(wire.pair.server.get(),
                                  "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 1\r\n\r\na"));
        auto received = std::optional<ReadResponse> {};
        wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &received));
        std::ignore = wire.loop.drain();
        REQUIRE(received.has_value());
        CHECK(received->body.text == "a");
        CHECK(connection.reusable() == Reuse::No);
        wire.pair.server->close();
        std::ignore = wire.loop.drain();
    }
    SECTION("until close reads to the peer's close")
    {
        auto wire = Wire {};
        auto connection = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        wire.loop.spawn(writeAndClose(wire.pair.server.get(), "HTTP/1.1 200 OK\r\n\r\n" + patternOf(100)));
        auto received = std::optional<ReadResponse> {};
        wire.loop.spawn(into(receiveOne(&connection, "GET", 10), &received)); // under the pair's 16
        std::ignore = wire.loop.drain();
        REQUIRE(received.has_value());
        REQUIRE_FALSE(received->body.error.has_value());
        CHECK(received->body.text == patternOf(100));
        CHECK(std::ranges::all_of(received->body.chunkSizes, [](std::size_t size) { return size <= 10; }));
        CHECK(connection.reusable() == Reuse::No);
    }
    SECTION("a head written with a body pending")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto wrote = std::optional<HttpResult<void>> {};
        wire.loop.spawn(into(
            client.writeRequestHead(requestHead("PUT", "/", { { "Host", "h" } }),
                                    Framing { .kind = BodyFraming::Length, .length = 2, .persistence = {} }),
            &wrote));
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(wrote.has_value());
        CHECK(wrote->has_value());
        CHECK(client.reusable() == Reuse::No);
        auto source = BufferBodySource { "ab" };
        auto body = std::optional<HttpResult<void>> {};
        wire.loop.spawn(into(client.writeBody(&source, core::http::framingFor(&source)), &body));
        std::ignore = wire.loop.drain();
        REQUIRE(body.has_value());
        CHECK(body->has_value());
        CHECK(client.reusable() == Reuse::Yes);
        client.close();
        std::ignore = wire.loop.drain();
        REQUIRE(peer.has_value());
        CHECK(peer->ends_with("\r\n\r\nab"));
    }
    SECTION("close")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        CHECK(client.reusable() == Reuse::Yes);
        client.close();
        CHECK(client.reusable() == Reuse::No);
        CHECK(client.socket().isClosed());
    }
    SECTION("a socket closed past the connection")
    {
        // A lingering close goes through socket(), which the connection must still notice.
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        client.socket().close();
        CHECK(client.reusable() == Reuse::No);
    }
    SECTION("a head written with persistence close")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto wrote = std::optional<HttpResult<void>> {};
        wire.loop.spawn(
            into(client.writeRequestHead(
                     requestHead("GET", "/", { { "Host", "h" } }),
                     Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close }),
                 &wrote));
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(wrote.has_value());
        CHECK(wrote->has_value());
        CHECK_FALSE(client.socket().isClosed());
        CHECK(client.reusable() == Reuse::No);
        client.close();
        std::ignore = wire.loop.drain();
    }
}

TEST_CASE("Connection.aBodyReaderOutlivesItsConnection", "[http][connection]")
{
    // The reader shares the connection's state, so a pool that hands a body out may drop the
    // connection first.
    auto wire = Wire {};
    auto connection = std::make_unique<Connection>(
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts);
    wire.loop.spawn(
        writeAndClose(wire.pair.server.get(), "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nwxyz"));
    auto head = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection->readResponseHead(), &head));
    std::ignore = wire.loop.drain();
    REQUIRE(head.has_value());
    REQUIRE(head->has_value());
    auto reader =
        connection->bodyReader(Framing { .kind = BodyFraming::Length, .length = 4, .persistence = {} });
    connection.reset();
    auto body = std::optional<ReadBody> {};
    wire.loop.spawn(into(readAll(reader.get(), 100), &body));
    std::ignore = wire.loop.drain();
    REQUIRE(body.has_value());
    CHECK(body->text == "wxyz");
}

TEST_CASE("Connection.framingForAndBufferBodySource", "[http][connection]")
{
    CHECK(core::http::framingFor(nullptr).kind == BodyFraming::None);

    auto plain = BufferBodySource { "hello" };
    CHECK(plain.knownLength() == std::uint64_t { 5 });
    CHECK(plain.trailerIntent() == TrailerIntent::None);
    auto const length = core::http::framingFor(&plain);
    CHECK(length.kind == BodyFraming::Length);
    CHECK(length.length == 5);
    CHECK(length.persistence == Persistence::KeepAlive);

    auto withTrailers = BufferBodySource { "hello", fieldsOf({ { "X-A", "b" } }) };
    CHECK(withTrailers.trailerIntent() == TrailerIntent::Possible);
    CHECK(core::http::framingFor(&withTrailers).kind == BodyFraming::Chunked);

    auto unknown = ChunksBodySource { { "a" } };
    CHECK(core::http::framingFor(&unknown).kind == BodyFraming::Chunked);

    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto chunk = std::optional<HttpResult<BodyChunk>> {};
    loop.spawn(into(plain.next(), &chunk));
    std::ignore = loop.drain();
    REQUIRE(chunk.has_value());
    REQUIRE(chunk->has_value());
    CHECK(core::http::test::textOf((*chunk)->bytes) == "hello");
    CHECK((*chunk)->end == BodyEnd::Reached);
    auto trailers = std::optional<HttpResult<Fields>> {};
    loop.spawn(into(withTrailers.trailers(), &trailers));
    std::ignore = loop.drain();
    REQUIRE(trailers.has_value());
    REQUIRE(trailers->has_value());
    CHECK((*trailers)->get("x-a").size() == 1);
}

TEST_CASE("Connection.aFailedConnectionReadsNoFurtherHead", "[http][connection]")
{
    // After a timeout the peer may still send a head; it is not read, because the connection has
    // already ruled itself out and the bytes it would parse are of unknown framing.
    auto wire = Wire {};
    auto connection = Connection { wire.loop,
                                   std::move(wire.pair.client),
                                   MessageRole::Response,
                                   HttpLimits {},
                                   WireTimeouts { .firstByte = 10ms, .betweenBytes = 0ms } };
    auto first = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection.readResponseHead(), &first));
    std::ignore = wire.loop.drain();
    wire.clock.advance(10ms);
    std::ignore = wire.loop.drain();
    REQUIRE(first.has_value());
    REQUIRE_FALSE(first->has_value());
    CHECK(first->error().code == HttpErrorCode::ResponseTimeout);

    wire.loop.spawn(writeOnly(wire.pair.server.get(), "HTTP/1.1 204 No Content\r\n\r\n"));
    auto second = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection.readResponseHead(), &second));
    std::ignore = wire.loop.drain();
    REQUIRE(second.has_value());
    REQUIRE_FALSE(second->has_value());
    CHECK(second->error().code == HttpErrorCode::ConnectionTerminated);
    wire.pair.server->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aReaderOfAnEarlierBodyReadsNothingOfTheNext", "[http][connection]")
{
    // A pool may re-use a connection while the reader of its last body is still held: that reader
    // answers that its body is over, and never reads the next message's bytes.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(
        writeOnly(wire.pair.server.get(),
                  "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na"
                  "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nb\r\n0\r\nX-T: 2\r\n\r\n"));
    auto first = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &first));
    std::ignore = wire.loop.drain();
    REQUIRE(first.has_value());
    REQUIRE(first->head.has_value());
    CHECK(first->body.text == "a");

    // A reader of the first body, made after it ended, then the second head.
    auto stale =
        connection.bodyReader(Framing { .kind = BodyFraming::Length, .length = 1, .persistence = {} });
    auto head = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection.readResponseHead(), &head));
    std::ignore = wire.loop.drain();
    REQUIRE(head.has_value());
    REQUIRE(head->has_value());
    CHECK_FALSE(connection.hasLiveBodyReader());

    // The next body is adopted first, so only the generation tells the stale reader apart.
    auto current =
        connection.bodyReader(Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} });
    auto staleRead = std::optional<ReadBody> {};
    wire.loop.spawn(into(readAll(stale.get(), 100), &staleRead));
    std::ignore = wire.loop.drain();
    REQUIRE(staleRead.has_value());
    CHECK(staleRead->text.empty());

    auto body = std::optional<ReadBody> {};
    wire.loop.spawn(into(readAll(current.get(), 100), &body));
    std::ignore = wire.loop.drain();
    REQUIRE(body.has_value());
    CHECK(body->text == "b");
    auto staleTrailers = std::optional<HttpResult<Fields>> {};
    wire.loop.spawn(into(stale->trailers(), &staleTrailers));
    auto trailers = std::optional<HttpResult<Fields>> {};
    wire.loop.spawn(into(current->trailers(), &trailers));
    std::ignore = wire.loop.drain();
    REQUIRE(staleTrailers.has_value());
    REQUIRE(staleTrailers->has_value());
    CHECK((*staleTrailers)->entries().empty());
    REQUIRE(trailers.has_value());
    REQUIRE(trailers->has_value());
    CHECK((*trailers)->get("x-t").size() == 1);
    wire.pair.server->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.anInterimResponseHasNoBodyToRead", "[http][connection]")
{
    // A 100 Continue is followed by the final response with no body reader between them.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.server.get(),
                              "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    auto interim = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection.readResponseHead(), &interim));
    std::ignore = wire.loop.drain();
    REQUIRE(interim.has_value());
    REQUIRE(interim->has_value());
    CHECK((*interim)->status == 100);
    auto finalResponse = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &finalResponse));
    std::ignore = wire.loop.drain();
    REQUIRE(finalResponse.has_value());
    REQUIRE(finalResponse->head.has_value());
    CHECK(finalResponse->head->status == 200);
    CHECK(finalResponse->body.text == "ok");
    CHECK(connection.reusable() == Reuse::Yes);
    wire.pair.server->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aFailedBodyRepeatsItsFailure", "[http][connection]")
{
    // Once a body has failed, a later read of it reports the same failure instead of reading on.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(
        writeOnly(wire.pair.server.get(), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"));
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 100), &received));
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    REQUIRE(received->body.error.has_value());
    CHECK(received->body.error->code == HttpErrorCode::ProtocolError);
    auto again =
        connection.bodyReader(Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} });
    auto chunk = std::optional<HttpResult<BodyChunk>> {};
    wire.loop.spawn(into(again->read(100), &chunk));
    std::ignore = wire.loop.drain();
    REQUIRE(chunk.has_value());
    REQUIRE_FALSE(chunk->has_value());
    CHECK(chunk->error().code == HttpErrorCode::ProtocolError);
    wire.pair.server->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aSourceIsHeldToWhatTheHeadAnnounced", "[http][connection]")
{
    SECTION("a source that claims one length and streams another is refused before any body byte")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = ChunksBodySource { { "abc", "de" }, std::uint64_t { 6 } };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(
            sendRequestThenClose(
                &client, &source, Framing { .kind = BodyFraming::Length, .length = 5, .persistence = {} }),
            &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::RequestBodySize);
        CHECK(sent->error().size == std::uint64_t { 6 });
        REQUIRE(peer.has_value());
        CHECK(*peer == "PUT / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n");
    }
    SECTION("a head with no body, and a source with bytes")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto source = ChunksBodySource { { "x" } };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(sendRequestThenClose(&client, &source, Framing {}), &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        REQUIRE_FALSE(sent->has_value());
        CHECK(sent->error().code == HttpErrorCode::RequestBodySize);
        REQUIRE(peer.has_value());
        CHECK(*peer == "PUT / HTTP/1.1\r\nHost: h\r\n\r\n");
    }
    SECTION("no source under a chunked framing still ends the body")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto sent = std::optional<HttpResult<void>> {};
        auto peer = std::optional<std::string> {};
        wire.loop.spawn(into(
            sendRequestThenClose(
                &client, nullptr, Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} }),
            &sent));
        wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        CHECK(sent->has_value());
        REQUIRE(peer.has_value());
        CHECK(peer->ends_with("Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"));
    }
}

TEST_CASE("Connection.aRefusedHeadIsTheParsersRefusal", "[http][connection]")
{
    // The head parser's refusal reaches the caller as it is, and rules the connection out.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\nHost: h\r\n\r\n"));
    auto head = std::optional<HttpResult<RequestHead>> {};
    wire.loop.spawn(into(connection.readRequestHead(), &head));
    std::ignore = wire.loop.drain();
    REQUIRE(head.has_value());
    REQUIRE_FALSE(head->has_value());
    CHECK(head->error().code == HttpErrorCode::ProtocolError);
    CHECK(connection.reusable() == Reuse::No);
    wire.pair.client->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aZeroByteReadReadsNothing", "[http][connection]")
{
    // Over a chunked body, where feeding the decoder nothing would make no progress at all.
    auto wire = Wire {};
    auto connection = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.server.get(),
                              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n"));
    auto head = std::optional<HttpResult<ResponseHead>> {};
    wire.loop.spawn(into(connection.readResponseHead(), &head));
    std::ignore = wire.loop.drain();
    REQUIRE(head.has_value());
    REQUIRE(head->has_value());
    auto reader =
        connection.bodyReader(Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = {} });
    auto empty = std::optional<HttpResult<BodyChunk>> {};
    wire.loop.spawn(into(reader->read(0), &empty));
    std::ignore = wire.loop.drain();
    REQUIRE(empty.has_value());
    REQUIRE(empty->has_value());
    CHECK((*empty)->bytes.empty());
    CHECK((*empty)->end == BodyEnd::More);
    auto rest = std::optional<ReadBody> {};
    wire.loop.spawn(into(readAll(reader.get(), 100), &rest));
    std::ignore = wire.loop.drain();
    REQUIRE(rest.has_value());
    CHECK(rest->text == "ok");
    wire.pair.server->close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aCloseExchangeStillReadsItsAnswer", "[http][connection]")
{
    // Persistence::Close rules out the NEXT message, not the rest of this one.
    SECTION("a client that sent Connection: close reads the response")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto server = Connection {
            wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
        };
        auto const close =
            Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close };
        auto sent = std::optional<HttpResult<void>> {};
        auto served = std::optional<ServedRequest> {};
        wire.loop.spawn(
            into(sendRequest(&client, requestHead("GET", "/", { { "Host", "h" } }), nullptr, close), &sent));
        wire.loop.spawn(into(serveOne(&server, 100), &served));
        std::ignore = wire.loop.drain();
        REQUIRE(sent.has_value());
        CHECK(sent->has_value());
        REQUIRE(served.has_value());
        REQUIRE(served->head.has_value());
        CHECK(server.reusable() == Reuse::No); // it read Connection: close

        auto reply = BufferBodySource { "bye" };
        auto replied = std::optional<HttpResult<void>> {};
        auto received = std::optional<ReadResponse> {};
        auto replyFraming = core::http::framingFor(&reply);
        replyFraming.persistence = Persistence::Close;
        wire.loop.spawn(into(sendResponse(&server, responseHead(200, {}), &reply, replyFraming), &replied));
        wire.loop.spawn(into(receiveOne(&client, "GET", 100), &received));
        std::ignore = wire.loop.drain();
        REQUIRE(replied.has_value());
        CHECK(replied->has_value());
        REQUIRE(received.has_value());
        REQUIRE_FALSE(received->error.has_value());
        CHECK(received->body.text == "bye");
        CHECK(client.reusable() == Reuse::No);
        CHECK(server.reusable() == Reuse::No);

        // The exchange is over on both ends: no further head is read, whatever arrives.
        auto nextResponse = std::optional<HttpResult<ResponseHead>> {};
        auto nextRequest = std::optional<HttpResult<RequestHead>> {};
        wire.loop.spawn(into(client.readResponseHead(), &nextResponse));
        wire.loop.spawn(into(server.readRequestHead(), &nextRequest));
        std::ignore = wire.loop.drain();
        REQUIRE(nextResponse.has_value());
        REQUIRE_FALSE(nextResponse->has_value());
        CHECK(nextResponse->error().code == HttpErrorCode::ConnectionTerminated);
        REQUIRE(nextRequest.has_value());
        REQUIRE_FALSE(nextRequest->has_value());
        CHECK(nextRequest->error().code == HttpErrorCode::ConnectionTerminated);
    }
    SECTION("an HTTP/1.0 exchange, both ends")
    {
        auto wire = Wire {};
        auto client = Connection {
            wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
        };
        auto server = Connection {
            wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
        };
        auto const close =
            Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close };
        auto sent = std::optional<HttpResult<void>> {};
        auto served = std::optional<ServedRequest> {};
        wire.loop.spawn(into(
            sendRequest(
                &client, requestHead("GET", "/", { { "Host", "h" } }, HttpVersion::Http10), nullptr, close),
            &sent));
        wire.loop.spawn(into(serveOne(&server, 100), &served));
        std::ignore = wire.loop.drain();
        REQUIRE(served.has_value());
        REQUIRE(served->head.has_value());
        CHECK(served->head->version == HttpVersion::Http10);

        auto reply = BufferBodySource { "old" };
        auto head = responseHead(200, {});
        head.version = HttpVersion::Http10;
        auto replyFraming = core::http::framingFor(&reply);
        replyFraming.persistence = Persistence::Close;
        auto replied = std::optional<HttpResult<void>> {};
        auto received = std::optional<ReadResponse> {};
        wire.loop.spawn(into(sendResponse(&server, std::move(head), &reply, replyFraming), &replied));
        wire.loop.spawn(into(receiveOne(&client, "GET", 100), &received));
        std::ignore = wire.loop.drain();
        REQUIRE(received.has_value());
        REQUIRE_FALSE(received->error.has_value());
        CHECK(received->body.text == "old");
        CHECK(client.reusable() == Reuse::No);
    }
}

TEST_CASE("Connection.aResponseToHeadAnnouncesItsLengthAndSendsNoBody", "[http][connection]")
{
    // RFC 9110 9.3.2: the response to HEAD carries the fields a GET's would, and no body; the
    // connection stays reusable on both ends and carries the next exchange.
    auto wire = Wire {};
    auto client = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    auto exchange = [&](std::string_view method, std::string_view expectedBody) {
        auto sent = std::optional<HttpResult<void>> {};
        auto served = std::optional<ServedRequest> {};
        wire.loop.spawn(into(
            sendRequest(&client, requestHead(method, "/", { { "Host", "h" } }), nullptr, Framing {}), &sent));
        wire.loop.spawn(into(serveOne(&server, 100), &served));
        std::ignore = wire.loop.drain();
        REQUIRE(served.has_value());
        REQUIRE(served->head.has_value());
        auto reply = BufferBodySource { "hello" };
        auto replied = std::optional<HttpResult<void>> {};
        auto received = std::optional<ReadResponse> {};
        wire.loop.spawn(into(
            sendResponse(&server, responseHead(200, {}), &reply, core::http::framingFor(&reply)), &replied));
        wire.loop.spawn(into(receiveOne(&client, std::string { method }, 100), &received));
        std::ignore = wire.loop.drain();
        REQUIRE(replied.has_value());
        CHECK(replied->has_value());
        REQUIRE(received.has_value());
        REQUIRE_FALSE(received->error.has_value());
        REQUIRE(received->head.has_value());
        auto const length = received->head->fields.get("content-length");
        REQUIRE(length.size() == 1);
        CHECK(length.front() == "5");
        CHECK(received->body.text == expectedBody);
        CHECK(server.reusable() == Reuse::Yes);
        CHECK(client.reusable() == Reuse::Yes);
    };
    exchange("HEAD", "");
    exchange("GET", "hello"); // the HEAD left no stray body bytes behind
}

TEST_CASE("Connection.aHeadResponseWithNoFramingSaysNothing", "[http][connection]")
{
    // A HEAD reply with BodyFraming::None must not claim Content-Length: 0 for the GET it stands for.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(served.has_value());
    REQUIRE(served->head.has_value());
    auto wrote = std::optional<HttpResult<void>> {};
    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(responseHeadThenClose(&server, responseHead(200, {}), Framing {}), &wrote));
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    std::ignore = wire.loop.drain();
    REQUIRE(peer.has_value());
    CHECK(*peer == "HTTP/1.1 200 \r\n\r\n");
}

TEST_CASE("Connection.aBodyFollowsTheFramingItsHeadAnnounced", "[http][connection]")
{
    // writeBody under a framing other than the head's would contradict the wire; refused, with
    // nothing of the body written.
    auto wire = Wire {};
    auto client = Connection {
        wire.loop, std::move(wire.pair.client), MessageRole::Response, HttpLimits {}, NoTimeouts
    };
    auto source = BufferBodySource { "abc" };
    auto body = std::optional<HttpResult<void>> {};
    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.server.get()), &peer));
    wire.loop.spawn(into(headThenOtherBody(&client, &source), &body));
    std::ignore = wire.loop.drain();
    REQUIRE(body.has_value());
    REQUIRE_FALSE(body->has_value());
    CHECK(body->error().code == HttpErrorCode::ProtocolError);
    CHECK(client.reusable() == Reuse::No);
    client.close();
    std::ignore = wire.loop.drain();
    REQUIRE(peer.has_value());
    CHECK(peer->ends_with("Content-Length: 3\r\n\r\n"));
}

TEST_CASE("Connection.anUntilCloseBodyIsHeldToMaxBodyBytes", "[http][connection]")
{
    auto wire = Wire {};
    auto limits = HttpLimits {};
    limits.maxBodyBytes = 50;
    auto connection =
        Connection { wire.loop, std::move(wire.pair.client), MessageRole::Response, limits, NoTimeouts };
    wire.loop.spawn(writeAndClose(wire.pair.server.get(), "HTTP/1.1 200 OK\r\n\r\n" + patternOf(100)));
    auto received = std::optional<ReadResponse> {};
    wire.loop.spawn(into(receiveOne(&connection, "GET", 10), &received));
    std::ignore = wire.loop.drain();
    REQUIRE(received.has_value());
    REQUIRE(received->body.error.has_value());
    CHECK(received->body.error->code == HttpErrorCode::ResponseBodySize);
    CHECK(received->body.text.size() <= 50); // never more than the limit handed out
    CHECK(patternOf(100).starts_with(received->body.text));
}

TEST_CASE("Connection.theRequestRoleIsSerial", "[http][connection]")
{
    // Two pipelined requests: the second head is not read until the first response is written,
    // and then it is, intact.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(),
                              "GET /one HTTP/1.1\r\nHost: h\r\n\r\nGET /two HTTP/1.1\r\nHost: h\r\n\r\n"
                              "GET /three HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto first = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &first));
    std::ignore = wire.loop.drain();
    REQUIRE(first.has_value());
    REQUIRE(first->head.has_value());
    CHECK(first->head->target.text() == "/one");

    auto early = std::optional<HttpResult<RequestHead>> {};
    wire.loop.spawn(into(server.readRequestHead(), &early));
    std::ignore = wire.loop.drain();
    REQUIRE(early.has_value());
    REQUIRE_FALSE(early->has_value());
    CHECK(early->error().code == HttpErrorCode::ProtocolError);

    auto reply = BufferBodySource { "1" };
    auto replied = std::optional<HttpResult<void>> {};
    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    wire.loop.spawn(
        into(sendResponse(&server, responseHead(200, {}), &reply, core::http::framingFor(&reply)), &replied));
    std::ignore = wire.loop.drain();
    REQUIRE(replied.has_value());
    CHECK(replied->has_value());

    auto second = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &second));
    std::ignore = wire.loop.drain();
    REQUIRE(second.has_value());
    REQUIRE(second->head.has_value());
    CHECK(second->head->target.text() == "/two");

    // A bodiless final response answers its request with the head alone.
    auto noContent = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(server.writeResponseHead(responseHead(204, {}), Framing {}), &noContent));
    std::ignore = wire.loop.drain();
    REQUIRE(noContent.has_value());
    CHECK(noContent->has_value());
    auto third = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &third));
    std::ignore = wire.loop.drain();
    REQUIRE(third.has_value());
    REQUIRE(third->head.has_value());
    CHECK(third->head->target.text() == "/three");
    server.close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.anInterimResponseLeavesAHeadReplyBodiless", "[http][connection]")
{
    // 100 Continue answers nothing; the final response to the HEAD is still sent without a body.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(served.has_value());
    REQUIRE(served->head.has_value());

    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    auto interim = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(server.writeResponseHead(responseHead(100, {}), Framing {}), &interim));
    std::ignore = wire.loop.drain();
    REQUIRE(interim.has_value());
    REQUIRE(interim->has_value());
    auto reply = BufferBodySource { "hello" };
    auto replied = std::optional<HttpResult<void>> {};
    wire.loop.spawn(
        into(sendResponse(&server, responseHead(200, {}), &reply, core::http::framingFor(&reply)), &replied));
    std::ignore = wire.loop.drain();
    REQUIRE(replied.has_value());
    CHECK(replied->has_value());
    CHECK(server.reusable() == Reuse::Yes);
    server.close();
    std::ignore = wire.loop.drain();
    REQUIRE(peer.has_value());
    CHECK(*peer == "HTTP/1.1 100 \r\n\r\nHTTP/1.1 200 \r\nContent-Length: 5\r\n\r\n");
}

TEST_CASE("Connection.aRefusedRequestCanStillBeAnswered", "[http][connection]")
{
    // A failure on the read side leaves the write side free: the server answers 400, then closes.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\nHost: h\r\n\r\n"));
    auto head = std::optional<HttpResult<RequestHead>> {};
    wire.loop.spawn(into(server.readRequestHead(), &head));
    std::ignore = wire.loop.drain();
    REQUIRE(head.has_value());
    REQUIRE_FALSE(head->has_value());

    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    auto answered = std::optional<HttpResult<void>> {};
    wire.loop.spawn(
        into(responseHeadThenClose(
                 &server,
                 responseHead(400, {}),
                 Framing { .kind = BodyFraming::None, .length = 0, .persistence = Persistence::Close }),
             &answered));
    std::ignore = wire.loop.drain();
    REQUIRE(answered.has_value());
    CHECK(answered->has_value());
    REQUIRE(peer.has_value());
    CHECK(*peer == "HTTP/1.1 400 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

TEST_CASE("Connection.aRefusedHeadDoesNotParseWhatFollowsIt", "[http][connection]")
{
    // The bytes after a refused head are already buffered, and of unknown framing: never parsed.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(
        writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\nX\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto first = std::optional<HttpResult<RequestHead>> {};
    wire.loop.spawn(into(server.readRequestHead(), &first));
    std::ignore = wire.loop.drain();
    REQUIRE(first.has_value());
    REQUIRE_FALSE(first->has_value());
    auto second = std::optional<HttpResult<RequestHead>> {};
    wire.loop.spawn(into(server.readRequestHead(), &second));
    std::ignore = wire.loop.drain();
    REQUIRE(second.has_value());
    REQUIRE_FALSE(second->has_value());
    CHECK(second->error().code == HttpErrorCode::ConnectionTerminated);
    server.close();
    std::ignore = wire.loop.drain();
}

TEST_CASE("Connection.aHeadRefusedForItsFramingCanBeCorrected", "[http][connection]")
{
    // A framing the status cannot carry is refused before the first wire byte, so a corrected head
    // follows it and the connection stays reusable.
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(served.has_value());
    REQUIRE(served->head.has_value());

    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    auto refused = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(
        server.writeResponseHead(responseHead(204, {}),
                                 Framing { .kind = BodyFraming::Length, .length = 3, .persistence = {} }),
        &refused));
    std::ignore = wire.loop.drain();
    REQUIRE(refused.has_value());
    REQUIRE_FALSE(refused->has_value());
    CHECK(refused->error().code == HttpErrorCode::ProtocolError);
    auto corrected = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(server.writeResponseHead(responseHead(204, {}), Framing {}), &corrected));
    std::ignore = wire.loop.drain();
    REQUIRE(corrected.has_value());
    CHECK(corrected->has_value());
    CHECK(server.reusable() == Reuse::Yes);
    server.close();
    std::ignore = wire.loop.drain();
    REQUIRE(peer.has_value());
    CHECK(*peer == "HTTP/1.1 204 \r\n\r\n");
}

TEST_CASE("Connection.aStatusOutside100To999IsRefusedBeforeTheWire", "[http][connection]")
{
    // A status line carries three digits: 99 and 1000 would put a malformed line on the wire. Refused
    // before the first byte, so a corrected head still follows.
    auto const status =
        GENERATE(std::uint16_t { 0 }, std::uint16_t { 99 }, std::uint16_t { 1000 }, std::uint16_t { 65535 });
    CAPTURE(status);
    auto wire = Wire {};
    auto server = Connection {
        wire.loop, std::move(wire.pair.server), MessageRole::Request, HttpLimits {}, NoTimeouts
    };
    wire.loop.spawn(writeOnly(wire.pair.client.get(), "GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    auto served = std::optional<ServedRequest> {};
    wire.loop.spawn(into(serveOne(&server, 100), &served));
    std::ignore = wire.loop.drain();
    REQUIRE(served.has_value());

    auto peer = std::optional<std::string> {};
    wire.loop.spawn(into(readUntilClosed(wire.pair.client.get()), &peer));
    auto refused = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(server.writeResponseHead(responseHead(status, {}), Framing {}), &refused));
    std::ignore = wire.loop.drain();
    REQUIRE(refused.has_value());
    REQUIRE_FALSE(refused->has_value());
    CHECK(refused->error().code == HttpErrorCode::ProtocolError);
    auto corrected = std::optional<HttpResult<void>> {};
    wire.loop.spawn(into(server.writeResponseHead(responseHead(204, {}), Framing {}), &corrected));
    std::ignore = wire.loop.drain();
    REQUIRE(corrected.has_value());
    CHECK(corrected->has_value());
    server.close();
    std::ignore = wire.loop.drain();
    REQUIRE(peer.has_value());
    CHECK(*peer == "HTTP/1.1 204 \r\n\r\n");
}
