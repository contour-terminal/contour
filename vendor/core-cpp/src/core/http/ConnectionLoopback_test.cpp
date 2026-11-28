// SPDX-License-Identifier: Apache-2.0
//
// `Connection` over a real socket pair on every backend this platform builds: the deadlines, which
// the in-memory cases can only drive through the double's own `cancelRead`, and cancellation, which
// that double does not model at all; and `HttpClient`'s response timeout bounding a peer that never
// reads, on real timers. Every case bounds its whole flow with a watchdog, so a deadline that never fires
// fails the case in seconds instead of hanging it.
#include <core/async/AsTask.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/http/Body.hpp>
#include <core/http/Connection.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpClient.hpp>
#include <core/http/HttpTestSupport.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

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
using core::http::BodyFraming;
using core::http::BufferBodySource;
using core::http::Connection;
using core::http::Framing;
using core::http::HttpErrorCode;
using core::http::HttpLimits;
using core::http::HttpResult;
using core::http::MessageRole;
using core::http::Method;
using core::http::RequestHead;
using core::http::RequestTarget;
using core::http::ResponseHead;
using core::http::Reuse;
using core::http::WireTimeouts;
using core::http::test::into;
using core::http::test::readAll;
using core::http::test::ReadBody;
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

/// Many times what two `SmallBuffer`-sized kernel buffers hold (the kernel may round them up), so a
/// peer that does not read stalls a writer.
constexpr auto Flood = std::size_t { 4 } * 1024 * 1024;

/// Asserts that a write against a peer that never reads stalled. Windows' loopback, natively and
/// under wine, takes the whole flood whatever the buffer sizes, so there the case could not run and
/// says so; on a kernel whose loopback stalls a write that finished is the defect this case exists
/// to find.
/// @param finished Whether the write ran to its end.
void expectStalled(bool finished)
{
    if (finished && core::http::test::loopbackFlood() == core::http::test::LoopbackFlood::Absorbed)
        SKIP("Windows' loopback, natively and under wine, takes the whole flood however small "
             "SO_SNDBUF/SO_RCVBUF are, so no write stalls there");
    REQUIRE_FALSE(finished);
}

/// Writes @p text to @p socket in full.
Task<void> send(ISocket* socket, std::string text)
{
    static_cast<void>(co_await socket->write(core::http::test::bytesOf(text)));
}

/// Waits @p delay on @p loop, then writes @p text to @p socket.
Task<void> sendAfter(EventLoop* loop, ISocket* socket, std::chrono::milliseconds delay, std::string text)
{
    co_await loop->delay(delay);
    static_cast<void>(co_await socket->write(core::http::test::bytesOf(text)));
}

/// Reads once from @p socket, bounded by @p limit: what a connection that gave up left there.
Task<std::string> readOnceWithin(EventLoop* loop, ISocket* socket, std::chrono::milliseconds limit)
{
    auto buffer = std::vector<std::byte>(4096);
    auto got = co_await core::net::withTimeout(
        loop, core::async::asTask(socket->read(std::span<std::byte> { buffer })), limit);
    if (!got.has_value() || !got->has_value())
        co_return std::string {};
    co_return textOf(std::span<std::byte const> { buffer }.first(**got));
}

RequestHead getHead(std::string extraValue = {})
{
    auto head = RequestHead {};
    auto target = RequestTarget::parse("/");
    REQUIRE(target.has_value());
    head.target = std::move(*target);
    auto method = Method::parse(extraValue.empty() ? "GET" : "PUT");
    REQUIRE(method.has_value());
    head.method = std::move(*method);
    REQUIRE(head.fields.append("Host", "h").has_value());
    if (!extraValue.empty())
    {
        auto const appended = head.fields.append("X-Flood", std::move(extraValue));
        REQUIRE(appended.has_value());
    }
    return head;
}

/// Reads a response head and its body to the end; the first failure is the answer.
Task<HttpResult<std::string>> receive(Connection* connection, std::size_t maxBytes)
{
    auto head = co_await connection->readResponseHead();
    if (!head.has_value())
        co_return std::unexpected(std::move(head.error()));
    auto framing = core::http::responseFraming("GET", head->version, head->status, head->fields);
    if (!framing.has_value())
        co_return std::unexpected(std::move(framing.error()));
    auto reader = connection->bodyReader(*framing);
    auto body = co_await readAll(reader.get(), maxBytes);
    if (body.error.has_value())
        co_return std::unexpected(std::move(*body.error));
    co_return std::move(body.text);
}

/// Writes a request head, then @p body under @p framing.
Task<HttpResult<void>> upload(Connection* connection,
                              RequestHead head,
                              BufferBodySource* body,
                              Framing framing)
{
    auto wrote = co_await connection->writeRequestHead(std::move(head), framing);
    if (!wrote.has_value())
        co_return wrote;
    co_return co_await connection->writeBody(body, framing);
}

/// The two ends of one case: a loop on a backend, a connection over one end of a real pair.
struct Ends
{
    std::unique_ptr<core::net::IoBackend> backend;
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<ISocket> peer;
    std::unique_ptr<Connection> connection;
};

/// @return The ends, or nothing where @p kind does not build here.
std::optional<Ends> endsOn(core::net::BackendKind kind, MessageRole role, WireTimeouts timeouts)
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
    auto connection =
        std::make_unique<Connection>(*loop, std::move(pair->first), role, HttpLimits {}, timeouts);
    return Ends { .backend = std::move(backend),
                  .loop = std::move(loop),
                  .peer = std::move(pair->second),
                  .connection = std::move(connection) };
}

/// Runs @p work against a withTimeout of `Deadline` and records whether the deadline won.
Task<void> race(EventLoop* loop, Task<void> work, std::optional<bool>* timedOut)
{
    *timedOut = !co_await core::net::withTimeout(loop, std::move(work), Deadline);
}

/// @p task with its answer dropped, for a race that only asks whether it finished.
template <typename T>
Task<void> discard(Task<T> task)
{
    static_cast<void>(co_await std::move(task));
}

/// Whether @p kind builds on this platform; a case skips the backends that do not.
bool buildsHere(core::net::BackendKind kind)
{
    return core::net::makeBackend(kind) != nullptr;
}

/// What every cancellation case asserts once its race is over: the deadline won, and the
/// connection is not offered for reuse. Called inside each SECTION, because code after a set of
/// sections also runs in Catch2's discovery pass that enters none of them.
void expectCutOff(std::optional<bool> const& timedOut, Connection const& connection)
{
    REQUIRE(timedOut.has_value());
    CHECK(*timedOut);
    CHECK(connection.reusable() == Reuse::No);
}

/// Runs @p task under the watchdog and REQUIREs it finished.
void runBounded(EventLoop& loop, Task<void> task)
{
    auto const finished = core::net::testing::blockOnWithin(loop, std::move(task), Watchdog);
    REQUIRE(finished);
}

/// A connector that hands out the sockets it was given, one per dial, in order.
class QueuedSocketConnector final: public core::net::IConnector
{
  public:
    explicit QueuedSocketConnector(std::vector<std::unique_ptr<ISocket>> sockets):
        _sockets { std::move(sockets) }
    {
    }

    [[nodiscard]] Task<core::net::SocketResult> connect(std::string /*host*/,
                                                        std::uint16_t /*port*/,
                                                        core::net::DialOptions /*options*/) override
    {
        return handOut(&_sockets, _dials++);
    }

    /// @return How many dials were asked for.
    [[nodiscard]] std::size_t connects() const noexcept { return _dials; }

  private:
    static Task<core::net::SocketResult> handOut(std::vector<std::unique_ptr<ISocket>>* sockets,
                                                 std::size_t index)
    {
        if (index >= sockets->size())
            co_return std::unexpected(
                core::net::makeNetError(core::net::NetErrorCode::BadHandle, 0, "dialled once too often"));
        co_return std::move(sockets->at(index));
    }

    std::vector<std::unique_ptr<ISocket>> _sockets;
    std::size_t _dials = 0;
};

/// Runs @p server and @p client together, to both ends.
Task<void> both(Task<void> server, Task<void> client)
{
    co_await core::async::whenAll(std::move(server), std::move(client));
}

} // namespace

TEST_CASE("ConnectionLoopback.aSilentPeerTimesOutOnEveryBackend", "[http][connection][loopback]")
{
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            SECTION("the first byte of a response head")
            {
                auto ends = endsOn(backend.kind,
                                   MessageRole::Response,
                                   WireTimeouts { .firstByte = Deadline, .betweenBytes = 0ms, .write = 0ms });
                REQUIRE(ends.has_value());
                auto answer = std::optional<HttpResult<std::string>> {};
                runBounded(*ends->loop, into(receive(ends->connection.get(), 100), &answer));
                REQUIRE(answer.has_value());
                REQUIRE_FALSE(answer->has_value());
                CHECK(answer->error().code == HttpErrorCode::ResponseTimeout);
                CHECK_FALSE(ends->connection->socket().isClosed()); // still open: a server can answer 408
                CHECK(ends->connection->reusable() == Reuse::No);
            }
            SECTION("between the bytes of a head")
            {
                auto ends = endsOn(backend.kind,
                                   MessageRole::Response,
                                   WireTimeouts { .firstByte = 0ms, .betweenBytes = Deadline, .write = 0ms });
                REQUIRE(ends.has_value());
                runBounded(*ends->loop, send(ends->peer.get(), "HTTP/1.1 200 OK\r\n"));
                auto answer = std::optional<HttpResult<std::string>> {};
                runBounded(*ends->loop, into(receive(ends->connection.get(), 100), &answer));
                REQUIRE(answer.has_value());
                REQUIRE_FALSE(answer->has_value());
                CHECK(answer->error().code == HttpErrorCode::ConnectionReadTimeout);
            }
            SECTION("between the bytes of a body")
            {
                auto ends = endsOn(backend.kind,
                                   MessageRole::Response,
                                   WireTimeouts { .firstByte = 0ms, .betweenBytes = Deadline, .write = 0ms });
                REQUIRE(ends.has_value());
                runBounded(*ends->loop,
                           send(ends->peer.get(), "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nab"));
                auto answer = std::optional<HttpResult<std::string>> {};
                runBounded(*ends->loop, into(receive(ends->connection.get(), 100), &answer));
                REQUIRE(answer.has_value());
                REQUIRE_FALSE(answer->has_value());
                CHECK(answer->error().code == HttpErrorCode::ConnectionReadTimeout);
            }
        }
    }
}

TEST_CASE("ConnectionLoopback.bytesThatMeetTheDeadlineAreNeverLost", "[http][connection][loopback]")
{
    // The peer's bytes and the read deadline land together. Whichever wins -- the read (a readiness
    // backend that saw them first, or a completion backend whose receive had already finished when
    // the deadline retired it: the data wins), or the deadline -- no byte disappears: the connection
    // either read the head, or the bytes are still on the socket for the next reader.
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ends = endsOn(backend.kind,
                               MessageRole::Response,
                               WireTimeouts { .firstByte = Deadline, .betweenBytes = 0ms, .write = 0ms });
            REQUIRE(ends.has_value());
            constexpr auto Wire = std::string_view { "HTTP/1.1 204 No Content\r\n\r\n" };
            // Spawned first, so its delay is armed before the connection's deadline: due together.
            ends->loop->spawn(sendAfter(ends->loop.get(), ends->peer.get(), Deadline, std::string { Wire }));
            auto answer = std::optional<HttpResult<std::string>> {};
            runBounded(*ends->loop, into(receive(ends->connection.get(), 100), &answer));
            REQUIRE(answer.has_value());
            if (answer->has_value())
            {
                CHECK(ends->connection->reusable() == Reuse::Yes);
            }
            else
            {
                CHECK(answer->error().code == HttpErrorCode::ResponseTimeout);
                auto left = std::optional<std::string> {};
                runBounded(*ends->loop,
                           into(readOnceWithin(ends->loop.get(), &ends->connection->socket(), 5s), &left));
                REQUIRE(left.has_value());
                CHECK(*left == Wire);
            }
        }
    }
}

TEST_CASE("ConnectionLoopback.aPeerThatStopsReadingTimesTheWriteOut", "[http][connection][loopback]")
{
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ends = endsOn(backend.kind,
                               MessageRole::Response,
                               WireTimeouts { .firstByte = 0ms, .betweenBytes = 0ms, .write = Deadline });
            REQUIRE(ends.has_value());
            auto body = BufferBodySource { std::string(Flood, 'x') };
            auto sent = std::optional<HttpResult<void>> {};
            runBounded(
                *ends->loop,
                into(upload(ends->connection.get(), getHead("v"), &body, core::http::framingFor(&body)),
                     &sent));
            REQUIRE(sent.has_value());
            expectStalled(sent->has_value());
            CHECK(sent->error().code == HttpErrorCode::ConnectionWriteTimeout);
            CHECK(ends->connection->socket().isClosed());
            CHECK(ends->connection->reusable() == Reuse::No);
        }
    }
}

TEST_CASE("ConnectionLoopback.aFlowCancelledOnTheWireLeavesTheConnectionUnreusable",
          "[http][connection][loopback]")
{
    // A stop from the caller's own token (here a withTimeout race) unwinds the connection's flow in
    // the middle of a message; whatever side it was on is cut off at an unknown byte.
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ends = endsOn(backend.kind, MessageRole::Response, WireTimeouts {});
            REQUIRE(ends.has_value());
            auto* const loop = ends->loop.get();
            auto* const connection = ends->connection.get();
            auto timedOut = std::optional<bool> {};
            SECTION("a head read")
            {
                runBounded(*loop, race(loop, discard(connection->readResponseHead()), &timedOut));
                CHECK(connection->reusable() == Reuse::No); // before anything else could say so
                // A head that arrives now is not parsed from a stream cut off at an unknown byte.
                runBounded(*loop, send(ends->peer.get(), "HTTP/1.1 204 No Content\r\n\r\n"));
                auto next = std::optional<HttpResult<ResponseHead>> {};
                runBounded(*loop, into(connection->readResponseHead(), &next));
                REQUIRE(next.has_value());
                REQUIRE_FALSE(next->has_value());
                CHECK(next->error().code == HttpErrorCode::ConnectionTerminated);
                expectCutOff(timedOut, *connection);
            }
            SECTION("a body read")
            {
                runBounded(*loop, send(ends->peer.get(), "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nab"));
                auto head = std::optional<HttpResult<ResponseHead>> {};
                runBounded(*loop, into(connection->readResponseHead(), &head));
                REQUIRE(head.has_value());
                REQUIRE(head->has_value());
                auto reader = connection->bodyReader(
                    Framing { .kind = BodyFraming::Length, .length = 9, .persistence = {} });
                runBounded(*loop, race(loop, discard(readAll(reader.get(), 100)), &timedOut));
                expectCutOff(timedOut, *connection);
                // The rest arrives and the read is retried: still refused, for the cut is where nobody
                // knows.
                runBounded(*loop, send(ends->peer.get(), "cdefghi"));
                auto retried = std::optional<ReadBody> {};
                runBounded(*loop, into(readAll(reader.get(), 100), &retried));
                REQUIRE(retried.has_value());
                REQUIRE(retried->error.has_value());
                CHECK(retried->error->code == HttpErrorCode::ConnectionTerminated);
                CHECK(connection->reusable() == Reuse::No);
            }
            SECTION("a head write")
            {
                runBounded(
                    *loop,
                    race(loop,
                         discard(connection->writeRequestHead(getHead(std::string(Flood, 'x')), Framing {})),
                         &timedOut));
                REQUIRE(timedOut.has_value());
                expectStalled(!*timedOut);
                expectCutOff(timedOut, *connection);
                // A retried head would follow the bytes the cancelled one left half-sent: refused.
                auto retried = std::optional<HttpResult<void>> {};
                runBounded(*loop, into(connection->writeRequestHead(getHead(), Framing {}), &retried));
                REQUIRE(retried.has_value());
                REQUIRE_FALSE(retried->has_value());
                CHECK(retried->error().code == HttpErrorCode::ConnectionTerminated);
                CHECK(connection->reusable() == Reuse::No);
            }
            SECTION("a body write")
            {
                auto body = BufferBodySource { std::string(Flood, 'x') };
                runBounded(
                    *loop,
                    race(loop,
                         discard(upload(connection, getHead("v"), &body, core::http::framingFor(&body))),
                         &timedOut));
                REQUIRE(timedOut.has_value());
                expectStalled(!*timedOut);
                expectCutOff(timedOut, *connection);
            }
        }
    }
}

TEST_CASE("ConnectionLoopback.aServerThatNeverReadsIsBoundedByTheResponseTimeout", "[http][client][loopback]")
{
    // HttpClient reads the response head while it uploads a flood the peer never reads, so the write
    // stalls in the kernel. The response timeout runs through the upload (P13 Task 16 ruling, as
    // wasmtime's first-byte timeout does), ends the request, and its close ends the stalled write.
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ioBackend = core::net::makeBackend(backend.kind);
            REQUIRE(ioBackend != nullptr);
            auto loop = EventLoop { *ioBackend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());
            std::ignore = pair->first->setOption(core::net::SocketOption::SendBufferSize, SmallBuffer);
            std::ignore = pair->second->setOption(core::net::SocketOption::ReceiveBufferSize, SmallBuffer);
            auto sockets = std::vector<std::unique_ptr<ISocket>> {};
            sockets.push_back(std::move(pair->first));
            auto connector = QueuedSocketConnector { std::move(sockets) };
            auto options = core::http::HttpClientOptions {};
            options.timeouts =
                core::http::HttpTimeouts { .connect = 0ms, .firstByte = Deadline, .betweenBytes = 0ms };
            auto client = core::http::HttpClient { loop, connector, nullptr, options };
            auto request = core::http::test::get("http://example.test/upload", "PUT");
            request.body = std::make_unique<BufferBodySource>(std::string(Flood, 'x'));
            auto answer = std::optional<HttpResult<core::http::IncomingResponse>> {};
            runBounded(loop, into(client.send(std::move(request)), &answer));
            REQUIRE(answer.has_value());
            REQUIRE_FALSE(answer->has_value());
            CHECK(answer->error().code == HttpErrorCode::ResponseTimeout);
        }
    }
}

TEST_CASE("ConnectionLoopback.aKeptAliveConnectionTheServerClosedIsRetried", "[http][client][pool][loopback]")
{
    // A server closes a kept-alive connection while it sits in the client's pool, as servers do at
    // their own idle timeout. On a real socket the reuse sees the close as an end of stream or a
    // reset, depending on the kernel and the timing; either is a stale connection, and the bodiless
    // GET is sent again on a fresh one.
    for (auto const& backend: BackendMatrix)
    {
        if (!buildsHere(backend.kind))
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto ioBackend = core::net::makeBackend(backend.kind);
            REQUIRE(ioBackend != nullptr);
            auto loop = EventLoop { *ioBackend };
            auto pooled = core::net::testing::makeSocketPair(loop);
            REQUIRE(pooled.has_value());
            auto fresh = core::net::testing::makeSocketPair(loop);
            REQUIRE(fresh.has_value());
            auto sockets = std::vector<std::unique_ptr<ISocket>> {};
            sockets.push_back(std::move(pooled->first));
            sockets.push_back(std::move(fresh->first));
            auto connector = QueuedSocketConnector { std::move(sockets) };
            auto client = core::http::HttpClient { loop, connector, nullptr };
            constexpr auto Reply = std::string_view { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" };

            auto firstLine = std::optional<std::string> {};
            auto first = std::optional<HttpResult<core::http::IncomingResponse>> {};
            runBounded(
                loop,
                both(into(core::http::test::answerHeadOnly(pooled->second.get(), std::string { Reply }),
                          &firstLine),
                     into(client.send(core::http::test::get("http://example.test/one")), &first)));
            REQUIRE(firstLine == "GET /one HTTP/1.1");
            REQUIRE(first.has_value());
            REQUIRE(first->has_value());
            auto firstBody = std::optional<ReadBody> {};
            runBounded(loop, into(readAll((*first)->body.get(), 100), &firstBody));
            REQUIRE(firstBody.has_value());
            CHECK(firstBody->text == "ok");
            REQUIRE(client.idleConnections() == 1);

            pooled->second->close();
            auto secondLine = std::optional<std::string> {};
            auto second = std::optional<HttpResult<core::http::IncomingResponse>> {};
            runBounded(loop,
                       both(into(core::http::test::answerHeadOnly(fresh->second.get(), std::string { Reply }),
                                 &secondLine),
                            into(client.send(core::http::test::get("http://example.test/two")), &second)));
            CHECK(connector.connects() == 2);
            CHECK(secondLine == "GET /two HTTP/1.1");
            REQUIRE(second.has_value());
            REQUIRE(second->has_value());
            CHECK((*second)->head.status == 200);
        }
    }
}
