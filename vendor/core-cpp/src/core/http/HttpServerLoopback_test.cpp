// SPDX-License-Identifier: Apache-2.0
//
// The server over real sockets: one request and response between `serveHttp` and `HttpClient` on
// the default backend, and the deadlines that bound a slow client on every backend this platform
// builds -- one that never reads (the write deadline), and ones that send slowly (the head, idle,
// between-bytes and whole-body deadlines). Every case bounds its flow with a watchdog, so a deadline
// that never fires fails the case in seconds instead of hanging it.
#include <core/async/AsTask.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/http/Body.hpp>
#include <core/http/HttpClient.hpp>
#include <core/http/HttpServer.hpp>
#include <core/http/HttpTestSupport.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using core::async::Task;
using core::http::BufferBodySource;
using core::http::HttpResult;
using core::http::HttpServerOptions;
using core::http::IHttpHandler;
using core::http::IncomingRequest;
using core::http::OutgoingResponse;
using core::http::ResponseHead;
using core::http::test::EchoHandler;
using core::http::test::parseResponses;
using core::http::test::textOf;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;

using namespace std::chrono_literals;

namespace
{

/// The watchdog over each case's flow: two orders of magnitude over the deadlines below.
constexpr auto Watchdog = 10s;

/// Long enough that a loaded runner does not deliver what the case never sent first.
constexpr auto Deadline = 50ms;

/// The kernel buffers each end of a pair is asked for, so a little data fills them.
constexpr auto SmallBuffer = std::uint64_t { 4096 };

/// Many times what two `SmallBuffer`-sized kernel buffers hold, so a client that does not read stalls
/// the server's write.
constexpr auto Flood = std::size_t { 4 } * 1024 * 1024;

/// Every deadline off, so a case turns on only the one it is about.
HttpServerOptions quietOptions()
{
    auto options = HttpServerOptions {};
    options.headTimeout = 0ms;
    options.idleTimeout = 0ms;
    options.bodyBetweenBytes = 0ms;
    options.bodyTotal = 0ms;
    options.writeTimeout = 0ms;
    return options;
}

/// Answers every request with a body of `Flood` bytes.
class FloodHandler final: public IHttpHandler
{
  public:
    [[nodiscard]] Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        static_cast<void>(request);
        return answer();
    }

  private:
    static Task<HttpResult<OutgoingResponse>> answer()
    {
        co_return OutgoingResponse { .head = ResponseHead {},
                                     .body = std::make_unique<BufferBodySource>(std::string(Flood, 'f')) };
    }
};

/// The two ends of one case: a loop on a backend, and a real connected pair.
struct Ends
{
    std::unique_ptr<core::net::IoBackend> backend;
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<ISocket> server;
    std::unique_ptr<ISocket> client;
};

/// @return The ends, or nothing where @p kind does not build here.
std::optional<Ends> endsOn(core::net::BackendKind kind)
{
    auto backend = core::net::makeBackend(kind);
    if (!backend)
        return std::nullopt;
    auto loop = std::make_unique<EventLoop>(*backend);
    auto pair = core::net::testing::makeSocketPair(*loop);
    REQUIRE(pair.has_value());
    // Best effort: a transport without the options keeps its defaults, and the flood still exceeds
    // them on every native kernel.
    std::ignore = pair->first->setOption(core::net::SocketOption::SendBufferSize, SmallBuffer);
    std::ignore = pair->second->setOption(core::net::SocketOption::ReceiveBufferSize, SmallBuffer);
    return Ends { .backend = std::move(backend),
                  .loop = std::move(loop),
                  .server = std::move(pair->first),
                  .client = std::move(pair->second) };
}

/// Writes @p text to @p socket in full.
Task<void> send(ISocket* socket, std::string text)
{
    if (!text.empty())
        static_cast<void>(co_await socket->write(core::http::test::bytesOf(text)));
}

/// Writes @p text one byte every @p gap, stopping at the first failed write.
Task<void> trickle(EventLoop* loop, ISocket* socket, std::string text, std::chrono::milliseconds gap)
{
    for (auto const octet: text)
    {
        co_await loop->delay(gap);
        auto const one = std::string(1, octet);
        if (!(co_await socket->write(core::http::test::bytesOf(one))).has_value())
            co_return;
    }
}

/// Reads @p socket until EOF or an error, within @p limit.
/// @return What the server wrote before it closed, or nothing if it had not closed by @p limit.
Task<std::optional<std::string>> readUntilClosedWithin(EventLoop* loop,
                                                       ISocket* socket,
                                                       std::chrono::milliseconds limit)
{
    auto text = std::string {};
    auto collect = [](ISocket* from, std::string* out) -> Task<void> {
        auto buffer = std::vector<std::byte>(4096);
        while (true)
        {
            auto const got = co_await from->read(std::span<std::byte> { buffer });
            if (!got.has_value() || *got == 0)
                co_return;
            *out += textOf(std::span<std::byte const> { buffer }.first(*got));
        }
    };
    if (!co_await core::net::withTimeout(loop, collect(socket, &text), limit))
        co_return std::nullopt;
    co_return text;
}

/// Serves @p ends' server end with @p handler while the client sends @p prefix, then @p rest
/// trickled a byte every @p gap; the client's answer lands in @p out.
Task<void> serveSlowSender(Ends* ends,
                           IHttpHandler* handler,
                           HttpServerOptions options,
                           std::string prefix,
                           std::string rest,
                           std::chrono::milliseconds gap,
                           std::optional<std::optional<std::string>>* out)
{
    auto served =
        core::http::serveHttpConnection(ends->loop.get(), std::move(ends->server), handler, options);
    auto client = [](Ends* both,
                     std::string first,
                     std::string slow,
                     std::chrono::milliseconds pause,
                     std::optional<std::optional<std::string>>* answer) -> Task<void> {
        co_await send(both->client.get(), std::move(first));
        co_await core::async::whenAll(
            trickle(both->loop.get(), both->client.get(), std::move(slow), pause),
            core::http::test::into(readUntilClosedWithin(both->loop.get(), both->client.get(), Watchdog / 2),
                                   answer));
    };
    co_await core::async::whenAll(std::move(served),
                                  client(ends, std::move(prefix), std::move(rest), gap, out));
}

/// Whether @p kind builds on this platform; a case skips the backends that do not.
bool buildsHere(core::net::BackendKind kind)
{
    return core::net::makeBackend(kind) != nullptr;
}

/// Runs @p task under the watchdog and REQUIREs it finished.
void runBounded(EventLoop& loop, Task<void> task)
{
    auto const finished = core::net::testing::blockOnWithin(loop, std::move(task), Watchdog);
    REQUIRE(finished);
}

/// The status of the first response in @p wire, or zero for none.
std::uint16_t firstStatus(std::string_view wire)
{
    auto const responses = parseResponses(wire);
    return responses.empty() ? std::uint16_t { 0 } : responses.front().head.status;
}

/// Sends @p request with @p client and reads the body to its end.
Task<HttpResult<std::string>> fetch(core::http::HttpClient* client, core::http::OutgoingRequest request)
{
    auto response = co_await client->send(std::move(request));
    if (!response.has_value())
        co_return std::unexpected(std::move(response.error()));
    auto body = co_await core::http::test::readAll(response->body.get(), 4096);
    if (body.error.has_value())
        co_return std::unexpected(std::move(*body.error));
    co_return std::move(body.text);
}

} // namespace

TEST_CASE("HttpServerLoopback.aClientRoundTripOnTheDefaultBackend", "[http][server][loopback]")
{
    auto backend = core::net::makeDefaultBackend();
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    if (!listener.has_value())
        SKIP("no loopback listener here: " << listener.error().toString());
    auto connector = core::net::makeConnector(loop, core::net::defaultAsyncResolver());
    auto handler = EchoHandler {};
    auto const port = (*listener)->boundPort();
    auto first = std::optional<HttpResult<std::string>> {};
    auto second = std::optional<HttpResult<std::string>> {};
    {
        auto client = core::http::HttpClient { loop, *connector, nullptr };
        auto const url = "http://127.0.0.1:" + std::to_string(port);
        auto exchangeTwice = [](core::http::HttpClient* http,
                                std::string base,
                                core::net::IListener* accepting,
                                std::optional<HttpResult<std::string>>* one,
                                std::optional<HttpResult<std::string>>* two) -> Task<void> {
            *one = co_await fetch(http, core::http::test::get(base + "/one"));
            *two = co_await fetch(http, core::http::test::get(base + "/two")); // over the pooled connection
            accepting->close();
        };
        loop.spawn(core::http::serveHttp(&loop, listener->get(), &handler));
        runBounded(loop, exchangeTwice(&client, url, listener->get(), &first, &second));
    }
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK(handler.calls() == 2);
}

TEST_CASE("HttpServerLoopback.aClientThatNeverReadsMeetsTheWriteDeadline", "[http][server][loopback]")
{
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ends = endsOn(backend.kind);
            REQUIRE(ends.has_value());
            auto options = quietOptions();
            options.writeTimeout = Deadline;
            auto handler = FloodHandler {};
            auto request = std::string { "GET / HTTP/1.1\r\nHost: a\r\n\r\n" };
            auto served = [](Ends* both,
                             IHttpHandler* serving,
                             HttpServerOptions with,
                             std::string wire) -> Task<void> {
                co_await send(both->client.get(), std::move(wire)); // and then never read
                co_await core::http::serveHttpConnection(
                    both->loop.get(), std::move(both->server), serving, with);
            };
            auto const finished = core::net::testing::blockOnWithin(
                *ends->loop, served(&*ends, &handler, options, std::move(request)), Watchdog);
            if (!finished && core::http::test::loopbackFlood() == core::http::test::LoopbackFlood::Absorbed)
                SKIP("Windows' loopback, natively and under wine, takes the whole flood however small "
                     "SO_SNDBUF/SO_RCVBUF are, so no write stalls there, and the connection waits for its "
                     "next request");
            CHECK(finished);
        }
    }
}

TEST_CASE("HttpServerLoopback.aClientThatSendsSlowlyMeetsTheReadDeadlines", "[http][server][loopback]")
{
    struct Row
    {
        std::string_view name;
        std::string_view prefix; ///< Sent at once.
        std::string_view rest;   ///< Trickled.
        std::chrono::milliseconds gap;
        void (*configure)(HttpServerOptions&);
        std::uint16_t status; ///< What the client is answered; zero for a close without an answer.
    };
    auto const rows = std::array {
        Row { "a head that never finishes, by headTimeout",
              "GET / HTTP/1.1\r\nHo",
              "",
              0ms,
              [](HttpServerOptions& options) { options.headTimeout = Deadline; },
              408 },
        Row { "a head that trickles within bodyBetweenBytes, by headTimeout",
              "GET / HTTP/1.1\r\nHost: a\r\nX-Slow: ",
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              20ms,
              [](HttpServerOptions& options) {
                  options.bodyBetweenBytes = Deadline * 4;
                  options.headTimeout = Deadline * 2;
              },
              408 },
        Row { "no byte at all, closed silently by idleTimeout",
              "",
              "",
              0ms,
              [](HttpServerOptions& options) { options.idleTimeout = Deadline; },
              0 },
        Row { "a body that stops, by bodyBetweenBytes",
              "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\na",
              "",
              0ms,
              [](HttpServerOptions& options) { options.bodyBetweenBytes = Deadline; },
              408 },
        Row { "a body that trickles within bodyBetweenBytes, by bodyTotal",
              "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 50\r\n\r\n",
              "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
              20ms,
              [](HttpServerOptions& options) {
                  options.bodyBetweenBytes = Deadline * 4;
                  options.bodyTotal = Deadline * 2;
              },
              408 },
    };
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        for (auto const& row: rows)
        {
            DYNAMIC_SECTION("backend=" << backend.name << " " << row.name)
            {
                auto ends = endsOn(backend.kind);
                REQUIRE(ends.has_value());
                auto options = quietOptions();
                row.configure(options);
                auto handler = EchoHandler {};
                auto answer = std::optional<std::optional<std::string>> {};
                runBounded(*ends->loop,
                           serveSlowSender(&*ends,
                                           &handler,
                                           options,
                                           std::string { row.prefix },
                                           std::string { row.rest },
                                           row.gap,
                                           &answer));
                REQUIRE(answer.has_value());
                REQUIRE(answer->has_value()); // the server closed
                CHECK(firstStatus(**answer) == row.status);
                if (row.status == 0)
                    CHECK((*answer)->empty());
            }
        }
    }
}

TEST_CASE("HttpServerLoopback.aBodyReadLeftRunningNeverMeetsTheClose", "[http][server][loopback]")
{
    // The handler answers once its own read has parked on a body the client never finishes; the
    // server closes with that read retired first, on every backend, and the close's reads meet none.
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto left = core::http::test::LeftRunning {};
            auto* loop = static_cast<EventLoop*>(nullptr);
            auto handler = core::http::test::SpawningReader { &loop, &left, Deadline };
            auto ends = endsOn(backend.kind);
            REQUIRE(ends.has_value());
            loop = ends->loop.get();
            auto answer = std::optional<std::optional<std::string>> {};
            runBounded(*ends->loop,
                       serveSlowSender(&*ends,
                                       &handler,
                                       quietOptions(),
                                       "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc",
                                       "",
                                       0ms,
                                       &answer));
            REQUIRE(answer.has_value());
            REQUIRE(answer->has_value());
            CHECK(firstStatus(**answer) == 200);
            CHECK(left.text == "abc");
            CHECK(left.failure == core::http::HttpErrorCode::ConnectionTerminated);
            CHECK(left.end == core::http::test::FlowEnd::Finished);
        }
    }
}
