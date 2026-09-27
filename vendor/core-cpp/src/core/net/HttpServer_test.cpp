// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/HttpServer.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::HttpLimits;
using core::net::HttpRequest;
using core::net::HttpResponse;
using core::net::NetErrorCode;

namespace
{

/// Writes @p text to @p socket, then shuts the write side by closing, so the
/// server observes EOF after the request.
Task<void> sendText(core::net::ISocket* socket, std::string const* text)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(text->data()), text->size() };
    static_cast<void>(co_await socket->write(bytes));
}

/// Drains @p socket to EOF into @p out.
Task<void> drain(core::net::ISocket* socket, std::string* out)
{
    auto chunk = std::array<std::byte, 4096> {};
    while (true)
    {
        auto const got = co_await socket->read(chunk);
        if (!got.has_value() || *got == 0)
            co_return;
        out->append(reinterpret_cast<char const*>(chunk.data()), *got);
    }
}

/// Feeds @p wire to a server endpoint, runs one request/response exchange through
/// `readRequest` + the handler + `writeResponse`, and returns the raw reply.
Task<void> exchange(core::net::ISocket* client,
                    core::net::ISocket* server,
                    std::string const* wire,
                    std::string* reply,
                    HttpLimits const* limits,
                    std::optional<HttpRequest>* seen,
                    std::optional<core::net::NetError>* error)
{
    co_await sendText(client, wire);

    auto request = co_await core::net::readRequest(server, *limits);
    if (request.has_value())
    {
        *seen = *request;
        auto response = HttpResponse::ok("pong");
        static_cast<void>(co_await core::net::writeResponse(server, std::move(response)));
    }
    else
        *error = request.error();

    server->close();
    co_await drain(client, reply);
}

/// A connection whose transport handshake fails, and which counts what is asked of it afterwards.
class HandshakeRefusingSocket final: public core::net::ISocket
{
  public:
    /// @param reads Incremented on every read; must outlive this socket.
    /// @param writes Incremented on every write; must outlive this socket.
    HandshakeRefusingSocket(int* reads, int* writes) noexcept: _reads { reads }, _writes { writes } {}

    /// @return A handshake failure, with no wait.
    [[nodiscard]] core::net::ResultAwaitable<void> handshakeIfNeeded() override
    {
        return core::net::ResultAwaitable<void> { std::unexpected(
            core::net::makeNetError(NetErrorCode::SystemError, 0, "handshake refused")) };
    }

    /// @return A clean EOF, counted.
    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> /*buffer*/) override
    {
        ++*_reads;
        return core::net::IoAwaitable { core::net::IoResult { std::size_t { 0 } } };
    }

    /// @param buffer The source.
    /// @return The byte count, counted.
    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        ++*_writes;
        return core::net::IoAwaitable { core::net::IoResult { buffer.size() } };
    }

    void close() noexcept override { _closed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

  private:
    int* _reads;
    int* _writes;
    bool _closed = false;
};

/// Hands out one prepared connection, then reports itself closed.
class OneShotListener final: public core::net::IListener
{
  public:
    /// @param connection The connection the first accept returns.
    explicit OneShotListener(std::unique_ptr<core::net::ISocket> connection) noexcept:
        _connection { std::move(connection) }
    {
    }

    [[nodiscard]] Task<core::net::AcceptResult> accept() override
    {
        if (_connection)
            co_return std::move(_connection);
        co_return std::unexpected(core::net::makeNetError(NetErrorCode::Cancelled, 0, "listener closed"));
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }
    void close() noexcept override { _connection.reset(); }

  private:
    std::unique_ptr<core::net::ISocket> _connection;
};

} // namespace

TEST_CASE("reasonPhrase names known codes and never returns empty", "[net][http]")
{
    REQUIRE(core::net::reasonPhrase(200) == "OK");
    REQUIRE(core::net::reasonPhrase(404) == "Not Found");
    REQUIRE(core::net::reasonPhrase(500) == "Internal Server Error");
    // The regression this port fixes: a non-200 status used to serialize with an
    // EMPTY reason phrase ("HTTP/1.1 404 \r\n"), which is a malformed status line.
    REQUIRE_FALSE(core::net::reasonPhrase(404).empty());
    REQUIRE_FALSE(core::net::reasonPhrase(599).empty());
    REQUIRE(core::net::reasonPhrase(599) == "Unknown");
}

TEST_CASE("withStatus carries a non-empty reason phrase for non-200 codes", "[net][http]")
{
    auto const notFound = HttpResponse::withStatus(404, "nope");
    REQUIRE(notFound.status == 404);
    REQUIRE(notFound.reason == "Not Found");
    REQUIRE(notFound.body == "nope");

    auto const ok = HttpResponse::ok("fine");
    REQUIRE(ok.status == 200);
    REQUIRE(ok.reason == "OK");
}

TEST_CASE("HttpRequest::header matches case-insensitively", "[net][http]")
{
    auto request = HttpRequest {};
    request.headers.emplace_back("Content-Type", "text/plain");
    request.headers.emplace_back("X-Trace", "abc");

    REQUIRE(request.header("content-type") == "text/plain");
    REQUIRE(request.header("CONTENT-TYPE") == "text/plain");
    REQUIRE(request.header("X-Trace") == "abc");
    REQUIRE(request.header("absent").empty());
}

TEST_CASE("readRequest parses a request line and headers", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET /index HTTP/1.1\r\nHost: example\r\nX-A: 1\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->method == "GET");
    REQUIRE(seen->path == "/index");
    REQUIRE(seen->version == "HTTP/1.1");
    REQUIRE(seen->header("Host") == "example");
    REQUIRE(seen->body.empty());
}

TEST_CASE("readRequest reads a Content-Length body", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST /submit HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->method == "POST");
    REQUIRE(seen->body == "hello world");
}

TEST_CASE("readRequest reads a body split across the header boundary", "[net][http]")
{
    // The body's first bytes ride in the same segment as the header delimiter —
    // the case a naive "read headers, then read body" loop gets wrong.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen->body == "abcde");
}

TEST_CASE("readRequest rejects a head exceeding the bound", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // A header block that never terminates, larger than the (tiny) bound.
    auto wire = std::string { "GET / HTTP/1.1\r\nX-Pad: " };
    wire += std::string(512, 'p');
    auto const limits = HttpLimits { .maxHeadBytes = 128, .maxBodyBytes = 1024 };
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readRequest rejects a body exceeding the bound", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // Content-Length advertises more than the limit allows: refused before the
    // body is read, so a hostile length cannot be used to make us buffer it.
    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 9999\r\n\r\n" };
    auto const limits = HttpLimits { .maxHeadBytes = 4096, .maxBodyBytes = 16 };
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readRequest rejects a malformed request line", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "NOTAREQUESTLINE\r\nHost: x\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("readRequest rejects conflicting duplicate Content-Length headers", "[net][http]")
{
    // RFC 9112 6.3: two disagreeing lengths are a request-smuggling vector, since a
    // proxy and an origin may pick different ones and disagree about where this
    // request ends and the next begins.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 100\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("readRequest accepts repeated but identical Content-Length headers", "[net][http]")
{
    // Repetition alone is not the hazard; disagreement is.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->body == "hello");
}

TEST_CASE("readRequest refuses a chunked request rather than mis-framing it", "[net][http]")
{
    // Chunked bodies are out of scope. Parsing one as a zero-length body would leave
    // the chunk data buffered as though it were the start of another request.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("readRequest rejects an obs-fold continuation line", "[net][http]")
{
    // A folded header has no colon. Dropping it silently would lose the folded
    // Content-Length and leave the body unread on a connection we think we parsed.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\n\t0\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("readRequest parses a bare-LF request head", "[net][http]")
{
    // Hand-written clients and scripts routinely send LF-only endings. Splitting
    // only on CRLF would make the whole head one "request line" whose interior
    // spaces populate method/path/version with garbage.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET /plain HTTP/1.1\nHost: example\n\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->method == "GET");
    REQUIRE(seen->path == "/plain");
    REQUIRE(seen->header("Host") == "example");
}

TEST_CASE("readRequest refuses a head whose blank line is a bare LF", "[net][http]")
{
    // Request smuggling. A front-end that honours a bare LF as a line terminator — which
    // RFC 9112 §2.2 permits, and which this parser itself does for every other line — reads
    // the blank line as the end of the head and "Host: evil …" as a SECOND request. Folding
    // those headers into the first instead made the two ends disagree about where the next
    // request begins, which is the desync RFC 9112 §11.2 is about.
    //
    // Refused rather than parsed: the bytes behind that blank line were already consumed as
    // part of the head block, so a Content-Length read before it would index into the wrong
    // place. Same posture as Transfer-Encoding and a conflicting Content-Length.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\n\nHost: evil\r\nContent-Length: 0\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(seen.has_value()); // NOT one request carrying the smuggled headers
    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("readRequest rejects whitespace between a field name and its colon", "[net][http]")
{
    // RFC 9112 §5.1 makes this a MUST reject, for the reason finding 1 above is about: a front-end
    // that trims "Host :" back to "Host" and a server that rejects it (or the reverse) do not agree
    // on what the message says. Trimming was the lenient half of that disagreement.
    constexpr auto Spellings = std::array {
        std::string_view { "Host : example" },    // SP before the colon
        std::string_view { "Host\t: example" },   // HTAB before it
        std::string_view { "Host \t : example" }, // both
        std::string_view { ": headerless" },      // an empty field name is not a name
    };

    for (auto const spelling: Spellings)
    {
        DYNAMIC_SECTION("field line=" << spelling)
        {
            auto const source = core::net::makeDefaultBackend();
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto const wire = std::string { "GET / HTTP/1.1\r\n" } + std::string { spelling } + "\r\n\r\n";
            auto const limits = HttpLimits {};
            auto reply = std::string {};
            auto seen = std::optional<HttpRequest> {};
            auto error = std::optional<core::net::NetError> {};
            loop.blockOn(
                exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

            REQUIRE_FALSE(seen.has_value());
            REQUIRE(error.has_value());
            REQUIRE(error->code == NetErrorCode::SystemError);
        }
    }
}

TEST_CASE("readRequest keeps accepting whitespace AFTER the colon", "[net][http]")
{
    // The valid half of the rule the case above enforces: RFC 9112 §5 pads a field-value with
    // optional whitespace on both sides, which a recipient removes. Rejecting the name's
    // whitespace must not cost the ordinary "Host: example" spelling, nor an aligned one.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\r\nHost:  \texample \t\r\nX-Empty:\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    CHECK(seen->header("Host") == "example"); // padding on both sides removed
    CHECK(seen->header("X-Empty").empty());   // an empty value is still a value
}

TEST_CASE("readRequest rejects an unparsable Content-Length", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::SystemError);
}

TEST_CASE("writeResponse never emits duplicate framing headers", "[net][http]")
{
    // A handler that sets its own Content-Length or Connection is doing something
    // natural; emitting both its value and ours would be malformed.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto response = HttpResponse::ok("hello");
    response.headers.emplace_back("Content-Length", "999");
    response.headers.emplace_back("Connection", "keep-alive");

    auto reply = std::string {};
    auto run = [](core::net::ISocket* client,
                  core::net::ISocket* server,
                  HttpResponse* resp,
                  std::string* out) -> Task<void> {
        static_cast<void>(co_await core::net::writeResponse(server, std::move(*resp)));
        server->close();
        co_await drain(client, out);
    };
    loop.blockOn(run(pair->first.get(), pair->second.get(), &response, &reply));

    // Exactly one of each, and the length must describe the body we actually wrote.
    auto const countOf = [&reply](std::string_view needle) {
        auto count = std::size_t { 0 };
        auto pos = reply.find(needle);
        while (pos != std::string::npos)
        {
            ++count;
            pos = reply.find(needle, pos + needle.size());
        }
        return count;
    };
    CHECK(countOf("Content-Length:") == 1);
    CHECK(countOf("Connection:") == 1);
    CHECK(reply.contains("Content-Length: 5\r\n"));
    CHECK_FALSE(reply.contains("999"));
}

TEST_CASE("readRequest reports EOF when the peer closes before a request", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\r\n" }; // no terminator, then close
    auto const limits = HttpLimits {};
    auto error = std::optional<core::net::NetError> {};

    auto run = [](core::net::ISocket* client,
                  core::net::ISocket* server,
                  std::string const* text,
                  HttpLimits const* lim,
                  std::optional<core::net::NetError>* err) -> Task<void> {
        co_await sendText(client, text);
        client->close(); // half-close so the server sees EOF
        auto request = co_await core::net::readRequest(server, *lim);
        if (!request.has_value())
            *err = request.error();
    };
    loop.blockOn(run(pair->first.get(), pair->second.get(), &wire, &limits, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Eof);
}

TEST_CASE("writeResponse serializes status, framing headers and body", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\r\nHost: x\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<core::net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(reply.contains("Content-Length: 4\r\n"));
    REQUIRE(reply.contains("Content-Type: text/plain; charset=utf-8\r\n"));
    REQUIRE(reply.contains("Connection: close\r\n"));
    REQUIRE(reply.ends_with("\r\n\r\npong"));
}

TEST_CASE("writeResponse keeps a handler-supplied Content-Type", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto response = HttpResponse::ok("{}");
    response.headers.emplace_back("Content-Type", "application/json");

    auto reply = std::string {};
    auto run = [](core::net::ISocket* server,
                  core::net::ISocket* client,
                  HttpResponse resp,
                  std::string* out) -> Task<void> {
        static_cast<void>(co_await core::net::writeResponse(server, std::move(resp)));
        server->close();
        co_await drain(client, out);
    };
    loop.blockOn(run(pair->second.get(), pair->first.get(), std::move(response), &reply));

    REQUIRE(reply.contains("Content-Type: application/json\r\n"));
    // The default must not also be emitted.
    REQUIRE_FALSE(reply.contains("text/plain"));
}

TEST_CASE("serve dispatches a request through a handler and closes", "[net][http]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    REQUIRE(port != 0);

    auto seenPath = std::string {};
    auto handler = core::net::HttpHandler { [&seenPath](HttpRequest const& request) {
        seenPath = request.path;
        return HttpResponse::ok("served:" + request.path);
    } };

    // Race the server against one client; the client's completion cancels serve().
    auto reply = std::string {};
    auto client = [](EventLoop* l, std::uint16_t p, std::string* out) -> Task<void> {
        auto connected = co_await core::net::connect(l, "127.0.0.1", p);
        if (!connected.has_value())
            co_return;
        auto socket = std::move(*connected);
        auto const request = std::string { "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" };
        co_await sendText(socket.get(), &request);
        co_await drain(socket.get(), out);
    };

    auto run = [](EventLoop* l,
                  core::net::IListener* lis,
                  core::net::HttpHandler h,
                  std::uint16_t p,
                  std::string* out,
                  auto clientFn) -> Task<void> {
        static_cast<void>(
            co_await core::async::whenAny(core::net::serve(lis, std::move(h)), clientFn(l, p, out)));
    };
    loop.blockOn(run(&loop, listener->get(), std::move(handler), port, &reply, client));

    REQUIRE(seenPath == "/hello");
    REQUIRE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(reply.ends_with("served:/hello"));
}

TEST_CASE("serve answers 500 when a handler throws rather than dying", "[net][http]")
{
    // The handler is caller-supplied code. An exception escaping it would unwind
    // through the accept loop and end every future connection, so serve() must
    // contain it and still answer this request.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    REQUIRE(port != 0);

    auto handler = core::net::HttpHandler { [](HttpRequest const&) -> HttpResponse {
        throw std::runtime_error { "handler blew up" };
    } };

    auto reply = std::string {};
    auto client = [](EventLoop* l, std::uint16_t p, std::string* out) -> Task<void> {
        auto connected = co_await core::net::connect(l, "127.0.0.1", p);
        if (!connected.has_value())
            co_return;
        auto socket = std::move(*connected);
        auto const request = std::string { "GET /boom HTTP/1.1\r\nHost: x\r\n\r\n" };
        co_await sendText(socket.get(), &request);
        co_await drain(socket.get(), out);
    };

    auto run = [](EventLoop* l,
                  core::net::IListener* lis,
                  core::net::HttpHandler h,
                  std::uint16_t p,
                  std::string* out,
                  auto clientFn) -> Task<void> {
        static_cast<void>(
            co_await core::async::whenAny(core::net::serve(lis, std::move(h)), clientFn(l, p, out)));
    };
    loop.blockOn(run(&loop, listener->get(), std::move(handler), port, &reply, client));

    REQUIRE(reply.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
}

TEST_CASE("serve completes the transport handshake before it reads a request", "[net][http]")
{
    // `handshakeIfNeeded` is the accept loop's to await, once, before anything reads the stream:
    // a transport that negotiates (TLS) must have finished before the server starts framing
    // bytes. A connection whose handshake fails has no request to read and no channel to answer
    // on -- so it is dropped with nothing read and nothing written, rather than answered with a
    // 400 that would go out over a transport that just refused to come up.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto reads = 0;
    auto writes = 0;
    auto listener = OneShotListener { std::make_unique<HandshakeRefusingSocket>(&reads, &writes) };
    auto handled = false;
    auto handler = core::net::HttpHandler { [&handled](HttpRequest const&) {
        handled = true;
        return HttpResponse::ok("unreachable");
    } };

    // Bounded: a serve() that read the socket after all would not return, and the wait says so
    // rather than leaving it to ctest's TIMEOUT.
    auto const served = loop.blockOn(core::net::withTimeout(
        &loop, core::net::serve(&listener, std::move(handler)), std::chrono::seconds { 10 }));
    REQUIRE(served);

    CHECK(reads == 0);
    CHECK(writes == 0);
    CHECK_FALSE(handled);
}

TEST_CASE("serve's refusal of a request whose body it did not read reaches the client, then EOF",
          "[net][http][linger]")
{
    // core-cpp#35. A 413 is written over a body the server never read, so a bare close finds
    // bytes still in its receive buffer and sends a reset rather than a FIN -- and the reset
    // destroys the refusal on Windows, and follows it as an error everywhere else. The body is
    // far past one read chunk of the server's reader, so most of it is unread at the close.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    REQUIRE(port != 0);

    auto handled = false;
    auto handler = core::net::HttpHandler { [&handled](HttpRequest const&) {
        handled = true;
        return HttpResponse::ok("unreachable");
    } };

    struct Received
    {
        std::string text;                       ///< Every byte read, in order.
        std::optional<core::net::IoResult> end; ///< EOF as 0, or the error that ended the reads.
    };
    auto client = [](EventLoop* l, std::uint16_t p, Received* out) -> Task<void> {
        auto connected = co_await core::net::connect(l, "127.0.0.1", p);
        if (!connected.has_value())
            co_return;
        auto socket = std::move(*connected);
        auto const request =
            std::string { "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 32768\r\n\r\n" }
            + std::string(32768, 'b');
        co_await sendText(socket.get(), &request);
        auto chunk = std::array<std::byte, 4096> {};
        while (true)
        {
            auto const got = co_await socket->read(chunk);
            if (!got.has_value() || *got == 0)
            {
                out->end = got;
                co_return;
            }
            out->text.append(reinterpret_cast<char const*>(chunk.data()), *got);
        }
    };
    auto run = [](EventLoop* l,
                  core::net::IListener* lis,
                  core::net::HttpHandler h,
                  std::uint16_t p,
                  Received* out,
                  auto clientFn) -> Task<void> {
        static_cast<void>(co_await core::async::whenAny(
            core::net::serve(lis, std::move(h), HttpLimits { .maxBodyBytes = 1024 }), clientFn(l, p, out)));
    };

    auto received = Received {};
    auto const ran = loop.blockOn(
        core::net::withTimeout(&loop,
                               run(&loop, listener->get(), std::move(handler), port, &received, client),
                               std::chrono::seconds { 10 }));
    REQUIRE(ran);

    CHECK_FALSE(handled);
    CHECK(received.text.starts_with("HTTP/1.1 413 "));
    REQUIRE(received.end.has_value());
    INFO("the client's reads ended in "
         << (received.end->has_value() ? "EOF" : received.end->error().context));
    CHECK(received.end->has_value());
}
