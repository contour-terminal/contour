// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <coro/Task.hpp>
#include <coro/WhenAny.hpp>
#include <net/HttpServer.hpp>
#include <net/ISocket.hpp>
#include <net/PollEventSource.hpp>
#include <net/Sockets.hpp>
#include <net/testing/InMemoryTransport.hpp>

using coro::Task;
using net::EventLoop;
using net::HttpLimits;
using net::HttpRequest;
using net::HttpResponse;
using net::NetErrorCode;

namespace
{

/// Writes @p text to @p socket, then shuts the write side by closing, so the
/// server observes EOF after the request.
Task<void> sendText(net::ISocket* socket, std::string const* text)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(text->data()), text->size() };
    static_cast<void>(co_await socket->write(bytes));
}

/// Drains @p socket to EOF into @p out.
Task<void> drain(net::ISocket* socket, std::string* out)
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
Task<void> exchange(net::ISocket* client,
                    net::ISocket* server,
                    std::string const* wire,
                    std::string* reply,
                    HttpLimits const* limits,
                    std::optional<HttpRequest>* seen,
                    std::optional<net::NetError>* error)
{
    co_await sendText(client, wire);

    auto request = co_await net::readRequest(server, *limits);
    if (request.has_value())
    {
        *seen = *request;
        auto response = HttpResponse::ok("pong");
        static_cast<void>(co_await net::writeResponse(server, std::move(response)));
    }
    else
        *error = request.error();

    server->close();
    co_await drain(client, reply);
}

} // namespace

TEST_CASE("reasonPhrase names known codes and never returns empty", "[net][http]")
{
    REQUIRE(net::reasonPhrase(200) == "OK");
    REQUIRE(net::reasonPhrase(404) == "Not Found");
    REQUIRE(net::reasonPhrase(500) == "Internal Server Error");
    // The regression this port fixes: a non-200 status used to serialize with an
    // EMPTY reason phrase ("HTTP/1.1 404 \r\n"), which is a malformed status line.
    REQUIRE_FALSE(net::reasonPhrase(404).empty());
    REQUIRE_FALSE(net::reasonPhrase(599).empty());
    REQUIRE(net::reasonPhrase(599) == "Unknown");
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
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET /index HTTP/1.1\r\nHost: example\r\nX-A: 1\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
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
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST /submit HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
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
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen->body == "abcde");
}

TEST_CASE("readRequest rejects a head exceeding the bound", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // A header block that never terminates, larger than the (tiny) bound.
    auto wire = std::string { "GET / HTTP/1.1\r\nX-Pad: " };
    wire += std::string(512, 'p');
    auto const limits = HttpLimits { .maxHeadBytes = 128, .maxBodyBytes = 1024 };
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readRequest rejects a body exceeding the bound", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // Content-Length advertises more than the limit allows: refused before the
    // body is read, so a hostile length cannot be used to make us buffer it.
    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 9999\r\n\r\n" };
    auto const limits = HttpLimits { .maxHeadBytes = 4096, .maxBodyBytes = 16 };
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readRequest rejects a malformed request line", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "NOTAREQUESTLINE\r\nHost: x\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("readRequest rejects conflicting duplicate Content-Length headers", "[net][http]")
{
    // RFC 9112 6.3: two disagreeing lengths are a request-smuggling vector, since a
    // proxy and an origin may pick different ones and disagree about where this
    // request ends and the next begins.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 100\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("readRequest accepts repeated but identical Content-Length headers", "[net][http]")
{
    // Repetition alone is not the hazard; disagreement is.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->body == "hello");
}

TEST_CASE("readRequest refuses a chunked request rather than mis-framing it", "[net][http]")
{
    // Chunked bodies are out of scope. Parsing one as a zero-length body would leave
    // the chunk data buffered as though it were the start of another request.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire =
        std::string { "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("readRequest rejects an obs-fold continuation line", "[net][http]")
{
    // A folded header has no colon. Dropping it silently would lose the folded
    // Content-Length and leave the body unread on a connection we think we parsed.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 5\r\n\t0\r\n\r\nhello" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("readRequest parses a bare-LF request head", "[net][http]")
{
    // Hand-written clients and scripts routinely send LF-only endings. Splitting
    // only on CRLF would make the whole head one "request line" whose interior
    // spaces populate method/path/version with garbage.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET /plain HTTP/1.1\nHost: example\n\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(seen.has_value());
    REQUIRE(seen->method == "GET");
    REQUIRE(seen->path == "/plain");
    REQUIRE(seen->header("Host") == "example");
}

TEST_CASE("readRequest rejects an unparsable Content-Length", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "POST / HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("writeResponse never emits duplicate framing headers", "[net][http]")
{
    // A handler that sets its own Content-Length or Connection is doing something
    // natural; emitting both its value and ours would be malformed.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto response = HttpResponse::ok("hello");
    response.headers.emplace_back("Content-Length", "999");
    response.headers.emplace_back("Connection", "keep-alive");

    auto reply = std::string {};
    auto run =
        [](net::ISocket* client, net::ISocket* server, HttpResponse* resp, std::string* out) -> Task<void> {
        static_cast<void>(co_await net::writeResponse(server, std::move(*resp)));
        server->close();
        co_await drain(client, out);
    };
    loop.blockOn(run(pair->first.get(), pair->second.get(), &response, &reply));

    // Exactly one of each, and the length must describe the body we actually wrote.
    auto const countOf = [&reply](std::string_view needle) {
        auto count = std::size_t { 0 };
        for (auto pos = reply.find(needle); pos != std::string::npos;
             pos = reply.find(needle, pos + needle.size()))
            ++count;
        return count;
    };
    CHECK(countOf("Content-Length:") == 1);
    CHECK(countOf("Connection:") == 1);
    CHECK(reply.contains("Content-Length: 5\r\n"));
    CHECK_FALSE(reply.contains("999"));
}

TEST_CASE("readRequest reports EOF when the peer closes before a request", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\r\n" }; // no terminator, then close
    auto const limits = HttpLimits {};
    auto error = std::optional<net::NetError> {};

    auto run = [](net::ISocket* client,
                  net::ISocket* server,
                  std::string const* text,
                  HttpLimits const* lim,
                  std::optional<net::NetError>* err) -> Task<void> {
        co_await sendText(client, text);
        client->close(); // half-close so the server sees EOF
        auto request = co_await net::readRequest(server, *lim);
        if (!request.has_value())
            *err = request.error();
    };
    loop.blockOn(run(pair->first.get(), pair->second.get(), &wire, &limits, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Eof);
}

TEST_CASE("writeResponse serializes status, framing headers and body", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto const wire = std::string { "GET / HTTP/1.1\r\nHost: x\r\n\r\n" };
    auto const limits = HttpLimits {};
    auto reply = std::string {};
    auto seen = std::optional<HttpRequest> {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(exchange(pair->first.get(), pair->second.get(), &wire, &reply, &limits, &seen, &error));

    REQUIRE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(reply.contains("Content-Length: 4\r\n"));
    REQUIRE(reply.contains("Content-Type: text/plain; charset=utf-8\r\n"));
    REQUIRE(reply.contains("Connection: close\r\n"));
    REQUIRE(reply.ends_with("\r\n\r\npong"));
}

TEST_CASE("writeResponse keeps a handler-supplied Content-Type", "[net][http]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto response = HttpResponse::ok("{}");
    response.headers.emplace_back("Content-Type", "application/json");

    auto reply = std::string {};
    auto run =
        [](net::ISocket* server, net::ISocket* client, HttpResponse resp, std::string* out) -> Task<void> {
        static_cast<void>(co_await net::writeResponse(server, std::move(resp)));
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
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->localPort();
    REQUIRE(port != 0);

    auto seenPath = std::string {};
    auto handler = net::HttpHandler { [&seenPath](HttpRequest const& request) {
        seenPath = request.path;
        return HttpResponse::ok("served:" + request.path);
    } };

    // Race the server against one client; the client's completion cancels serve().
    auto reply = std::string {};
    auto client = [](EventLoop* l, std::uint16_t p, std::string* out) -> Task<void> {
        auto connected = co_await net::connect(l, "127.0.0.1", p);
        if (!connected.has_value())
            co_return;
        auto socket = std::move(*connected);
        auto const request = std::string { "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" };
        co_await sendText(socket.get(), &request);
        co_await drain(socket.get(), out);
    };

    auto run = [](EventLoop* l,
                  net::IListener* lis,
                  net::HttpHandler h,
                  std::uint16_t p,
                  std::string* out,
                  auto clientFn) -> Task<void> {
        static_cast<void>(co_await coro::whenAny(net::serve(lis, std::move(h)), clientFn(l, p, out)));
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
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->localPort();
    REQUIRE(port != 0);

    auto handler = net::HttpHandler { [](HttpRequest const&) -> HttpResponse {
        throw std::runtime_error { "handler blew up" };
    } };

    auto reply = std::string {};
    auto client = [](EventLoop* l, std::uint16_t p, std::string* out) -> Task<void> {
        auto connected = co_await net::connect(l, "127.0.0.1", p);
        if (!connected.has_value())
            co_return;
        auto socket = std::move(*connected);
        auto const request = std::string { "GET /boom HTTP/1.1\r\nHost: x\r\n\r\n" };
        co_await sendText(socket.get(), &request);
        co_await drain(socket.get(), out);
    };

    auto run = [](EventLoop* l,
                  net::IListener* lis,
                  net::HttpHandler h,
                  std::uint16_t p,
                  std::string* out,
                  auto clientFn) -> Task<void> {
        static_cast<void>(co_await coro::whenAny(net::serve(lis, std::move(h)), clientFn(l, p, out)));
    };
    loop.blockOn(run(&loop, listener->get(), std::move(handler), port, &reply, client));

    REQUIRE(reply.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
}
