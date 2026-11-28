// SPDX-License-Identifier: Apache-2.0
//
// `serveHttp` and `serveHttpConnection` over in-memory pairs on a hand-driven loop: keep-alive with
// chunked bodies and trailers, the smuggling corpus answered with each row's exact status, the
// statuses of the refusal table, the drain of an unread body and when it closes instead, the head,
// idle and body deadlines, `Expect: 100-continue`, a handler's failures, and the accept loop's
// connection cap and accept policy. Every flow runs on a `TestLoop` over a `ManualClock`, so a deadline fires
// only when a case moves the clock, and a flow that never finishes leaves its answer empty instead of
// hanging.
#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpServer.hpp>
#include <core/http/HttpTestSupport.hpp>
#include <core/http/Message.hpp>
#include <core/http/SmugglingCorpus.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/Diagnostics.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/FailingListener.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using core::async::Task;
using core::http::BodyChunk;
using core::http::BufferBodySource;
using core::http::errorStatusFor;
using core::http::errorStatusTable;
using core::http::Fields;
using core::http::HeadParser;
using core::http::HttpError;
using core::http::HttpErrorCode;
using core::http::HttpResult;
using core::http::HttpServerOptions;
using core::http::IBodySource;
using core::http::IHttpHandler;
using core::http::IncomingRequest;
using core::http::MessageRole;
using core::http::OutgoingResponse;
using core::http::Progress;
using core::http::RefusalReason;
using core::http::ResponseHead;
using core::http::TrailerIntent;
using core::http::test::EchoHandler;
using core::http::test::EchoMode;
using core::http::test::exchange;
using core::http::test::fieldOf;
using core::http::test::FlowEnd;
using core::http::test::IgnoringHandler;
using core::http::test::KeepsReader;
using core::http::test::parseResponses;
using core::http::test::ServedConnection;
using core::http::test::smugglingCorpus;
using core::http::test::smugglingLimits;
using core::http::test::SmugglingVerdict;

using namespace std::chrono_literals;

namespace
{

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

/// @return An error of @p code and @p reason, its payload empty.
HttpError errorOf(HttpErrorCode code, RefusalReason reason = RefusalReason::Unclassified)
{
    auto error = HttpError {};
    error.code = code;
    error.reason = reason;
    return error;
}

/// Whether the pure layer refuses @p wire at its head or its framing, before any body byte.
bool refusedBeforeTheBody(std::string_view wire)
{
    auto parser = HeadParser { MessageRole::Request, smugglingLimits() };
    auto fed = parser.feed(wire);
    if (!fed.has_value())
        return true;
    if (fed->progress != Progress::Complete)
        return false;
    auto const head = parser.takeRequest();
    return !core::http::requestFraming(head.version, head.fields).has_value();
}

/// Answers each request after @p delay on the loop's clock, recording the order it was called in.
class SlowHandler final: public IHttpHandler
{
  public:
    SlowHandler(core::net::EventLoop** loop, std::chrono::milliseconds delay):
        _loop { loop }, _delay { delay }
    {
    }

    [[nodiscard]] Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        _targets.emplace_back(request.head.target.text());
        return answer(*_loop, _delay, std::string { request.head.target.text() });
    }

    [[nodiscard]] std::vector<std::string> const& targets() const noexcept { return _targets; }

  private:
    static Task<HttpResult<OutgoingResponse>> answer(core::net::EventLoop* loop,
                                                     std::chrono::milliseconds delay,
                                                     std::string target)
    {
        co_await loop->delay(delay);
        co_return OutgoingResponse { .head = ResponseHead {},
                                     .body = std::make_unique<BufferBodySource>(target) };
    }

    core::net::EventLoop** _loop;
    std::chrono::milliseconds _delay;
    std::vector<std::string> _targets;
};

/// Reads the request body only after @p delay, then answers it as an echo.
class LateReader final: public IHttpHandler
{
  public:
    LateReader(core::net::EventLoop** loop, std::chrono::milliseconds delay): _loop { loop }, _delay { delay }
    {
    }

    [[nodiscard]] Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        return answer(*_loop, _delay, std::move(request));
    }

  private:
    static Task<HttpResult<OutgoingResponse>> answer(core::net::EventLoop* loop,
                                                     std::chrono::milliseconds delay,
                                                     IncomingRequest request)
    {
        co_await loop->delay(delay);
        auto body = co_await core::http::test::readAll(request.body.get(), 1024);
        if (body.error.has_value())
            co_return std::unexpected(std::move(*body.error));
        co_return OutgoingResponse { .head = ResponseHead {},
                                     .body = std::make_unique<BufferBodySource>(std::move(body.text)) };
    }

    core::net::EventLoop** _loop;
    std::chrono::milliseconds _delay;
};

/// How a @c FailingHandler fails.
enum class Failure : std::uint8_t
{
    Error,        ///< It returns an error.
    Exception,    ///< It throws.
    FramingField, ///< It answers with a framing field, which the connection refuses before the wire.
    Interim,      ///< It answers with a 1xx, which is no final response.
    WideStatus,   ///< It answers with a status of four digits, which no status line can carry.
    BodySource,   ///< Its body fails after the head is written.
    BodyThrows,   ///< Its body throws after the head is written.
};

/// A body whose first chunk fails.
class FailingSource final: public IBodySource
{
  public:
    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override { return std::nullopt; }
    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::None; }
    [[nodiscard]] Task<HttpResult<BodyChunk>> next() override { return fail(); }
    [[nodiscard]] Task<HttpResult<Fields>> trailers() override { return core::http::test::noTrailers(); }

  private:
    static Task<HttpResult<BodyChunk>> fail()
    {
        co_return std::unexpected(errorOf(HttpErrorCode::InternalError));
    }
};

/// A body whose first chunk throws.
class ThrowingSource final: public IBodySource
{
  public:
    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override { return std::nullopt; }
    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::None; }
    [[nodiscard]] Task<HttpResult<BodyChunk>> next() override { return fail(); }
    [[nodiscard]] Task<HttpResult<Fields>> trailers() override { return core::http::test::noTrailers(); }

  private:
    static Task<HttpResult<BodyChunk>> fail()
    {
        throw std::runtime_error { "the body threw" };
        co_return BodyChunk {}; // what makes this a coroutine, so the throw is the task's
    }
};

class FailingHandler final: public IHttpHandler
{
  public:
    explicit FailingHandler(Failure failure): _failure { failure } {}

    [[nodiscard]] Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        static_cast<void>(request);
        return answer(_failure);
    }

  private:
    static Task<HttpResult<OutgoingResponse>> answer(Failure failure)
    {
        auto head = ResponseHead {};
        switch (failure)
        {
            case Failure::Error: co_return std::unexpected(errorOf(HttpErrorCode::InternalError));
            case Failure::Exception: throw std::runtime_error { "the handler threw" };
            case Failure::FramingField: REQUIRE(head.fields.append("Content-Length", "2").has_value()); break;
            case Failure::Interim:
                // Bodiless, which the connection would write: only the server can tell it is no answer.
                head.status = 103;
                co_return OutgoingResponse { .head = std::move(head), .body = nullptr };
            case Failure::WideStatus:
                head.status = 1000;
                co_return OutgoingResponse { .head = std::move(head), .body = nullptr };
            case Failure::BodySource:
                co_return OutgoingResponse { .head = std::move(head),
                                             .body = std::make_unique<FailingSource>() };
            case Failure::BodyThrows:
                co_return OutgoingResponse { .head = std::move(head),
                                             .body = std::make_unique<ThrowingSource>() };
        }
        co_return OutgoingResponse { .head = std::move(head),
                                     .body = std::make_unique<BufferBodySource>("ok") };
    }

    Failure _failure;
};

constexpr auto ChunkedUpload = std::string_view { "POST /up HTTP/1.1\r\n"
                                                  "Host: a\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "\r\n"
                                                  "3\r\nabc\r\n"
                                                  "2\r\nde\r\n"
                                                  "0\r\n"
                                                  "X-Sum: 5\r\n"
                                                  "\r\n" };

constexpr auto LengthUpload = std::string_view { "POST /up HTTP/1.1\r\n"
                                                 "Host: a\r\n"
                                                 "Content-Length: 10\r\n"
                                                 "\r\n"
                                                 "0123456789" };

constexpr auto ClosingGet = std::string_view { "GET /last HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n" };

/// @return `GET <target>` with a Host.
std::string get(std::string_view target)
{
    return "GET " + std::string { target } + " HTTP/1.1\r\nHost: a\r\n\r\n";
}

} // namespace

TEST_CASE("HttpServer.errorStatusTable", "[http][server]")
{
    // An independent statement of every row, so a row changed, removed or added fails here.
    struct Expected
    {
        HttpErrorCode code = HttpErrorCode::InternalError;
        RefusalReason reason = RefusalReason::Unclassified;
        std::optional<std::uint16_t> status;
    };
    constexpr auto Rows = std::array {
        Expected { HttpErrorCode::ProtocolError, RefusalReason::Unclassified, std::uint16_t { 400 } },
        Expected { HttpErrorCode::ProtocolError, RefusalReason::UnsupportedVersion, std::uint16_t { 505 } },
        Expected { HttpErrorCode::ProtocolError, RefusalReason::UnimplementedCoding, std::uint16_t { 501 } },
        Expected { HttpErrorCode::RequestMethodInvalid, RefusalReason::Unclassified, std::uint16_t { 400 } },
        Expected { HttpErrorCode::RequestUriInvalid, RefusalReason::Unclassified, std::uint16_t { 400 } },
        Expected { HttpErrorCode::RequestUriTooLong, RefusalReason::Unclassified, std::uint16_t { 414 } },
        Expected {
            HttpErrorCode::RequestHeaderSectionSize, RefusalReason::Unclassified, std::uint16_t { 431 } },
        Expected { HttpErrorCode::RequestHeaderSize, RefusalReason::Unclassified, std::uint16_t { 431 } },
        Expected {
            HttpErrorCode::RequestTrailerSectionSize, RefusalReason::Unclassified, std::uint16_t { 431 } },
        Expected { HttpErrorCode::RequestTrailerSize, RefusalReason::Unclassified, std::uint16_t { 431 } },
        Expected { HttpErrorCode::RequestBodySize, RefusalReason::Unclassified, std::uint16_t { 413 } },
        Expected { HttpErrorCode::RequestLengthRequired, RefusalReason::Unclassified, std::uint16_t { 411 } },
        Expected { HttpErrorCode::ConnectionReadTimeout, RefusalReason::Unclassified, std::uint16_t { 408 } },
        Expected { HttpErrorCode::InternalError, RefusalReason::Unclassified, std::uint16_t { 500 } },
        // A reason without a row of its own falls back to its code's.
        Expected {
            HttpErrorCode::RequestUriTooLong, RefusalReason::UnsupportedVersion, std::uint16_t { 414 } },
        // A code without a row is not answered.
        Expected { HttpErrorCode::ConnectionTerminated, RefusalReason::Unclassified, std::nullopt },
        Expected { HttpErrorCode::ConnectionWriteTimeout, RefusalReason::Unclassified, std::nullopt },
        Expected { HttpErrorCode::ResponseIncomplete, RefusalReason::Unclassified, std::nullopt },
        // An idle connection is closed, not answered: no byte of a request arrived.
        Expected { HttpErrorCode::ConnectionReadTimeout, RefusalReason::NothingArrived, std::nullopt },
    };
    for (auto const& row: Rows)
    {
        INFO(core::http::toString(row.code) << " reason " << static_cast<int>(row.reason));
        auto const found = errorStatusFor(errorOf(row.code, row.reason));
        REQUIRE(found.has_value() == row.status.has_value());
        if (found.has_value())
            CHECK(found->status == *row.status);
    }
    CHECK(errorStatusTable().size() == 14);
}

TEST_CASE("HttpServer.statusPhrase", "[http][server]")
{
    struct Expected
    {
        std::uint16_t status = 0;
        std::string_view phrase;
    };
    constexpr auto Rows = std::array {
        Expected { 100, "Continue" },
        Expected { 400, "Bad Request" },
        Expected { 408, "Request Timeout" },
        Expected { 411, "Length Required" },
        Expected { 413, "Content Too Large" },
        Expected { 414, "URI Too Long" },
        Expected { 431, "Request Header Fields Too Large" },
        Expected { 500, "Internal Server Error" },
        Expected { 501, "Not Implemented" },
        Expected { 505, "HTTP Version Not Supported" },
        Expected { 200, "" }, // never written by the server itself
        Expected { 404, "" },
    };
    for (auto const& row: Rows)
    {
        INFO(row.status);
        CHECK(core::http::statusPhrase(row.status) == row.phrase);
    }
    for (auto const& row: errorStatusTable())
        CHECK_FALSE(core::http::statusPhrase(row.status).empty());
}

TEST_CASE("HttpServer.keepAliveCarriesAChunkedUploadThenAClose", "[http][server]")
{
    auto handler = EchoHandler {};
    auto const answer =
        exchange(handler, quietOptions(), std::string { ChunkedUpload } + std::string { ClosingGet });

    auto const responses = parseResponses(answer.wire);
    REQUIRE(responses.size() == 2);
    CHECK(responses[0].head.status == 200);
    CHECK(fieldOf(responses[0].head.fields, "X-Echo") == "POST /up");
    CHECK(fieldOf(responses[0].head.fields, "Transfer-Encoding") == "chunked"); // trailers need it
    CHECK(responses[0].body == "abcde");
    CHECK(fieldOf(responses[0].trailers, "X-Sum") == "5");
    CHECK(fieldOf(responses[0].head.fields, "Connection").empty());
    CHECK(fieldOf(responses[1].head.fields, "X-Echo") == "GET /last");
    CHECK(fieldOf(responses[1].head.fields, "Connection") == "close");
    CHECK(handler.calls() == 2);
    CHECK(answer.server == FlowEnd::Finished);
    CHECK(answer.closed == FlowEnd::Finished);
}

TEST_CASE("HttpServer.theWireEndsAfterAConnectionClose", "[http][server]")
{
    // The client never half-closes: only the server's close can end the wire.
    auto handler = EchoHandler {};
    auto served = ServedConnection { handler, quietOptions() };
    served.send(std::string { ClosingGet } + get("/never"));
    served.advance(HttpServerOptions {}.linger.total);
    CHECK(parseResponses(served.received()).size() == 1);
    CHECK(handler.calls() == 1);
    CHECK(served.wire() == FlowEnd::Finished);
    CHECK(served.server() == FlowEnd::Finished);
}

TEST_CASE("HttpServer.pipelinedRequestsAreServedInOrderOneAtATime", "[http][server]")
{
    auto* loop = static_cast<core::net::EventLoop*>(nullptr);
    auto handler = SlowHandler { &loop, 100ms };
    auto served = ServedConnection { handler, quietOptions() };
    loop = &served.loop();

    served.send(get("/1") + get("/2") + get("/3"));
    CHECK(handler.targets() == std::vector<std::string> { "/1" }); // /2 waits for /1's answer
    CHECK(served.received().empty());

    served.advance(100ms);
    CHECK(handler.targets() == std::vector<std::string> { "/1", "/2" });
    CHECK(parseResponses(served.received()).size() == 1);

    served.advance(100ms);
    served.advance(100ms);
    auto const responses = parseResponses(served.received());
    REQUIRE(responses.size() == 3);
    CHECK(responses[0].body == "/1");
    CHECK(responses[1].body == "/2");
    CHECK(responses[2].body == "/3");
}

TEST_CASE("HttpServer.everyCorpusRequestRowIsAnsweredWithItsStatus", "[http][server][smuggling]")
{
    auto rows = std::size_t { 0 };
    for (auto const& row: smugglingCorpus())
    {
        if (row.role != MessageRole::Request)
            continue;
        ++rows;
        DYNAMIC_SECTION(row.name)
        {
            auto options = quietOptions();
            options.limits = smugglingLimits();
            auto handler = EchoHandler {};
            auto const answer = exchange(handler, options, std::string { row.wire });
            auto const responses = parseResponses(answer.wire);
            REQUIRE_FALSE(responses.empty());
            CHECK(responses.front().head.status == row.status);
            CHECK(std::ranges::find(handler.targets(), "/smuggled") == handler.targets().end());
            if (row.verdict == SmugglingVerdict::Refused)
            {
                CHECK(responses.size() == 1);
                CHECK(fieldOf(responses.front().head.fields, "Connection") == "close");
                CHECK(answer.closed == FlowEnd::Finished);
                CHECK(answer.server == FlowEnd::Finished);
                // A request smuggled in behind the refused one never reaches a handler.
                auto smuggler = EchoHandler {};
                auto const smuggled =
                    exchange(smuggler,
                             options,
                             std::string { row.wire } + "GET /smuggled HTTP/1.1\r\nHost: a\r\n\r\n");
                CHECK(parseResponses(smuggled.wire).size() == 1);
                CHECK(std::ranges::find(smuggler.targets(), "/smuggled") == smuggler.targets().end());
                if (refusedBeforeTheBody(row.wire))
                    CHECK(handler.calls() == 0);
                else
                {
                    // A refused body reaches the handler only as its failed read.
                    CHECK(handler.calls() == 1);
                    CHECK(handler.readFailure() == row.code);
                }
            }
            else
            {
                CHECK(handler.calls() == 1);
                CHECK(responses.front().body == row.body);
            }
        }
    }
    CHECK(rows >= 50);
}

TEST_CASE("HttpServer.anUnreadBodyIsDrainedWithinItsBound", "[http][server]")
{
    SECTION("within maxDrainBytes the connection carries the next request")
    {
        auto handler = IgnoringHandler {};
        auto const answer = exchange(handler, quietOptions(), std::string { LengthUpload } + get("/next"));
        auto const responses = parseResponses(answer.wire);
        REQUIRE(responses.size() == 2);
        CHECK(fieldOf(responses[0].head.fields, "Connection").empty());
        CHECK(responses[1].body == "ok");
        CHECK(handler.calls() == 2);
    }
    SECTION("a chunked one too")
    {
        auto handler = IgnoringHandler {};
        auto const answer = exchange(handler, quietOptions(), std::string { ChunkedUpload } + get("/next"));
        CHECK(parseResponses(answer.wire).size() == 2);
        CHECK(handler.calls() == 2);
    }
    SECTION("exactly maxDrainBytes is within it")
    {
        auto options = quietOptions();
        options.maxDrainBytes = 10;
        auto handler = IgnoringHandler {};
        auto const answer = exchange(handler, options, std::string { LengthUpload } + get("/next"));
        CHECK(parseResponses(answer.wire).size() == 2);
    }
    SECTION("a known remainder past maxDrainBytes is not read at all")
    {
        // The body never comes: a drain that waited for it would answer nothing.
        auto options = quietOptions();
        options.maxDrainBytes = 9;
        auto handler = IgnoringHandler {};
        auto served = ServedConnection { handler, options };
        served.send("POST /up HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n");
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
    }
    SECTION("past maxDrainBytes the response says close, and the wire ends")
    {
        auto options = quietOptions();
        options.maxDrainBytes = 9;
        auto handler = IgnoringHandler {};
        auto served = ServedConnection { handler, options };
        served.send(std::string { LengthUpload } + get("/next"));
        served.advance(options.linger.total);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
        CHECK(handler.calls() == 1);
        CHECK(served.wire() == FlowEnd::Finished);
    }
}

TEST_CASE("HttpServer.theDrainWaitsForTheHandlersReader", "[http][server]")
{
    SECTION("a handler streaming the body into its response is never raced by the drain")
    {
        auto handler = EchoHandler { EchoMode::Streamed };
        auto options = quietOptions();
        options.maxDrainBytes = 0; // any drain would close; none may happen
        auto const answer = exchange(handler, options, std::string { LengthUpload } + get("/next"));
        auto const responses = parseResponses(answer.wire);
        REQUIRE(responses.size() == 2);
        CHECK(responses[0].body == "0123456789");
        CHECK(fieldOf(responses[0].head.fields, "Connection").empty());
        CHECK(handler.calls() == 2);
    }
    SECTION("a reader still alive once the response is written closes the connection")
    {
        // The handler outlives the connection and its loop, and destroys the reader it kept after both:
        // the reader must reach nothing of the request it belonged to.
        auto handler = IgnoringHandler { KeepsReader::Yes };
        auto options = quietOptions();
        options.bodyTotal = 1000ms;
        auto served = ServedConnection { handler, options };
        served.send(std::string { LengthUpload } + get("/next"));
        served.advance(HttpServerOptions {}.linger.total);
        CHECK(parseResponses(served.received()).size() == 1);
        CHECK(handler.calls() == 1);
        CHECK(served.wire() == FlowEnd::Finished);
    }
}

TEST_CASE("HttpServer.aStalledClientMeetsItsDeadline", "[http][server]")
{
    auto handler = EchoHandler {};
    SECTION("a head started and not finished is 408 after headTimeout")
    {
        auto options = quietOptions();
        options.headTimeout = 100ms;
        auto served = ServedConnection { handler, options };
        served.send("GET / HTTP/1.1\r\nHo");
        served.advance(99ms);
        CHECK(served.received().empty());
        served.advance(1ms);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 408);
        CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
        served.advance(options.linger.total);
        CHECK(served.wire() == FlowEnd::Finished);
        CHECK(handler.calls() == 0);
    }
    SECTION("no byte at all is closed silently after idleTimeout")
    {
        auto options = quietOptions();
        options.idleTimeout = 100ms;
        auto served = ServedConnection { handler, options };
        served.advance(99ms);
        CHECK(served.server() == FlowEnd::Running);
        served.advance(1ms);
        served.advance(options.linger.total);
        CHECK(served.received().empty());
        CHECK(served.wire() == FlowEnd::Finished);
        CHECK(served.server() == FlowEnd::Finished);
    }
    SECTION("the wait for a first byte is idleTimeout's alone, not headTimeout's")
    {
        auto options = quietOptions();
        options.idleTimeout = 1000ms;
        options.headTimeout = 100ms;
        auto served = ServedConnection { handler, options };
        served.advance(999ms);
        served.send(get("/")); // well past headTimeout, inside idleTimeout: still served
        auto const answered = parseResponses(served.received());
        REQUIRE(answered.size() == 1);
        CHECK(answered[0].head.status == 200);
        served.advance(1000ms);
        served.advance(options.linger.total);
        CHECK(parseResponses(served.received()).size() == 1); // the idle one is closed unanswered
        CHECK(served.server() == FlowEnd::Finished);
    }
    SECTION("the head bound starts at the request's first byte, after an idle wait")
    {
        // Idle and head are separate budgets: a keep-alive client whose next request starts just
        // inside idleTimeout still has the whole headTimeout for that head.
        auto options = quietOptions();
        options.idleTimeout = 100ms;
        options.headTimeout = 100ms;
        auto served = ServedConnection { handler, options };
        served.send(get("/1"));
        served.advance(90ms); // idle, inside idleTimeout
        served.send("GET /2 HTTP/1.1\r\nHo");
        served.advance(90ms); // 180 ms after the idle read started, 90 ms into the head
        CHECK(parseResponses(served.received()).size() == 1);
        served.send("st: a\r\n\r\n");
        auto const answered = parseResponses(served.received());
        REQUIRE(answered.size() == 2);
        CHECK(answered[1].head.status == 200);
        served.advance(90ms);
        served.send("GET /3 HTTP/1.1\r\nHo");
        served.advance(99ms);
        CHECK(parseResponses(served.received()).size() == 2);
        served.advance(1ms); // headTimeout after /3's first byte
        auto const timedOut = parseResponses(served.received());
        REQUIRE(timedOut.size() == 3);
        CHECK(timedOut[2].head.status == 408);
        CHECK(handler.calls() == 2);
    }
    SECTION("the idle wait applies between requests too")
    {
        auto options = quietOptions();
        options.idleTimeout = 100ms;
        auto served = ServedConnection { handler, options };
        served.send(get("/1"));
        served.advance(99ms);
        served.send(get("/2")); // the idle clock starts again after /1's answer
        served.advance(99ms);
        CHECK(parseResponses(served.received()).size() == 2);
        served.advance(1ms);
        served.advance(options.linger.total);
        CHECK(parseResponses(served.received()).size() == 2); // the idle one is closed unanswered
        CHECK(served.wire() == FlowEnd::Finished);
    }
    SECTION("a head that trickles within bodyBetweenBytes is 408 at headTimeout")
    {
        auto options = quietOptions();
        options.headTimeout = 300ms;
        options.bodyBetweenBytes = 100ms;
        auto served = ServedConnection { handler, options };
        for (auto const octet: std::string_view { "GET" }) // at 0, 90 and 180 ms
        {
            served.send(std::string(1, octet));
            served.advance(90ms);
        }
        served.send(" "); // at 270 ms, each byte inside bodyBetweenBytes
        served.advance(29ms);
        CHECK(served.received().empty());
        served.advance(1ms);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 408);
        CHECK(handler.calls() == 0);
    }
    SECTION("a body that trickles a byte per bodyBetweenBytes + 1 is closed")
    {
        auto options = quietOptions();
        options.bodyBetweenBytes = 100ms;
        auto served = ServedConnection { handler, options };
        served.send("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\na");
        served.advance(101ms);
        served.send("b");
        served.advance(options.linger.total);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 408);
        CHECK(handler.readFailure() == HttpErrorCode::ConnectionReadTimeout);
        CHECK(served.wire() == FlowEnd::Finished);
    }
    SECTION("a body that trickles within bodyBetweenBytes but past bodyTotal is closed")
    {
        auto options = quietOptions();
        options.bodyBetweenBytes = 100ms;
        options.bodyTotal = 300ms;
        auto served = ServedConnection { handler, options };
        served.send("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n");
        for ([[maybe_unused]] auto const step: std::array { 1, 2, 3 })
        {
            served.advance(90ms);
            served.send("x");
        }
        CHECK(served.received().empty()); // 270 ms in
        served.advance(30ms);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 408);
        CHECK(handler.readFailure() == HttpErrorCode::ConnectionReadTimeout);
    }
}

TEST_CASE("HttpServer.expectContinueIsAnsweredWhenTheHandlerFirstReads", "[http][server]")
{
    constexpr auto Head = std::string_view { "POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                                             "Content-Length: 5\r\n\r\n" };
    SECTION("not before the read, then the interim response, then the final one")
    {
        auto* loop = static_cast<core::net::EventLoop*>(nullptr);
        auto handler = LateReader { &loop, 50ms };
        auto served = ServedConnection { handler, quietOptions() };
        loop = &served.loop();
        served.send(std::string { Head });
        served.advance(49ms);
        CHECK(served.received().empty());
        served.advance(1ms);
        CHECK(served.received() == "HTTP/1.1 100 Continue\r\n\r\n");
        served.send("hello");
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 2);
        CHECK(responses[0].head.status == 100);
        CHECK(responses[1].head.status == 200);
        CHECK(responses[1].body == "hello");
        CHECK(fieldOf(responses[1].head.fields, "Connection").empty());
    }
    SECTION("never, when the handler answers without reading; the connection then closes")
    {
        auto handler = IgnoringHandler {};
        auto served = ServedConnection { handler, quietOptions() };
        served.send(std::string { Head });
        served.advance(HttpServerOptions {}.linger.total);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 200);
        CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
        CHECK(served.wire() == FlowEnd::Finished);
    }
    SECTION("never for HTTP/1.0, which has no interim responses")
    {
        auto handler = EchoHandler {};
        auto const answer =
            exchange(handler,
                     quietOptions(),
                     "POST / HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\nhello");
        auto const responses = parseResponses(answer.wire);
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].head.status == 200);
        CHECK(responses[0].body == "hello");
    }
}

TEST_CASE("HttpServer.aHandlersFailureIs500AndAClose", "[http][server]")
{
    struct Row
    {
        std::string_view name;
        Failure failure;
    };
    constexpr auto Rows = std::array {
        Row { "an error", Failure::Error },
        Row { "an exception", Failure::Exception },
        Row { "a framing field in its head", Failure::FramingField },
        Row { "a 1xx as its final response", Failure::Interim },
        Row { "a status of four digits", Failure::WideStatus },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto handler = FailingHandler { row.failure };
            auto const answer = exchange(handler, quietOptions(), get("/") + get("/again"));
            auto const responses = parseResponses(answer.wire);
            REQUIRE(responses.size() == 1);
            CHECK(responses[0].head.status == 500);
            CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
            CHECK(answer.closed == FlowEnd::Finished);
        }
    }
    SECTION("a body that throws after its head closes the connection, lingering")
    {
        auto handler = FailingHandler { Failure::BodyThrows };
        auto const answer = exchange(handler, quietOptions(), get("/") + get("/again"));
        CHECK(answer.wire.starts_with("HTTP/1.1 200 \r\n"));
        CHECK_FALSE(answer.wire.contains("500"));
        CHECK(answer.closed == FlowEnd::Finished);
        CHECK(answer.server == FlowEnd::Finished); // the throw ended no flow: it was the body's failure
    }
    SECTION("a body that fails after its head closes the connection unanswered")
    {
        auto handler = FailingHandler { Failure::BodySource };
        auto const answer = exchange(handler, quietOptions(), get("/") + get("/again"));
        CHECK(answer.wire.starts_with("HTTP/1.1 200 \r\n")); // the handler left the reason empty
        CHECK_FALSE(answer.wire.contains("500"));
        CHECK(answer.closed == FlowEnd::Finished);
        CHECK(answer.server == FlowEnd::Finished);
    }
}

TEST_CASE("HttpServer.theLastRequestOfAConnectionSaysClose", "[http][server]")
{
    auto options = quietOptions();
    options.maxRequestsPerConnection = 2;
    auto handler = EchoHandler {};
    auto const answer = exchange(handler, options, get("/1") + get("/2") + get("/3"));
    auto const responses = parseResponses(answer.wire);
    REQUIRE(responses.size() == 2);
    CHECK(fieldOf(responses[0].head.fields, "Connection").empty());
    CHECK(fieldOf(responses[1].head.fields, "Connection") == "close");
    CHECK(handler.calls() == 2);
}

TEST_CASE("HttpServer.anHttp10RequestIsAnsweredInItsTerms", "[http][server]")
{
    SECTION("without keep-alive, the response closes")
    {
        auto handler = EchoHandler {};
        auto const answer = exchange(handler, quietOptions(), "GET / HTTP/1.0\r\n\r\n" + get("/never"));
        auto const responses = parseResponses(answer.wire);
        REQUIRE(responses.size() == 1);
        CHECK(fieldOf(responses[0].head.fields, "Connection") == "close");
        CHECK(handler.calls() == 1);
    }
    SECTION("a streamed body is sent until close, since HTTP/1.0 has no chunked coding")
    {
        auto handler = EchoHandler { EchoMode::Streamed };
        auto const answer =
            exchange(handler, quietOptions(), "POST / HTTP/1.0\r\nContent-Length: 3\r\n\r\nabc");
        CHECK_FALSE(answer.wire.contains("Transfer-Encoding"));
        CHECK(answer.wire.ends_with("\r\n\r\nabc"));
        CHECK(answer.closed == FlowEnd::Finished);
    }
}

TEST_CASE("HttpServer.aHeadResponseKeepsTheConnection", "[http][server]")
{
    auto handler = IgnoringHandler {};
    auto const answer = exchange(handler, quietOptions(), "HEAD / HTTP/1.1\r\nHost: a\r\n\r\n" + get("/2"));
    CHECK(answer.wire.starts_with("HTTP/1.1 200 \r\nContent-Length: 2\r\n\r\nHTTP/1.1 200 \r\n"));
    CHECK(answer.wire.ends_with("\r\n\r\nok"));
    CHECK(handler.calls() == 2);
}

TEST_CASE("HttpServer.aBodyPastMaxBodyBytesIs413WithoutTheHandler", "[http][server]")
{
    auto options = quietOptions();
    options.limits.maxBodyBytes = 9;
    auto handler = EchoHandler {};
    auto const answer = exchange(handler, options, std::string { LengthUpload });
    auto const responses = parseResponses(answer.wire);
    REQUIRE(responses.size() == 1);
    CHECK(responses[0].head.status == 413);
    CHECK(handler.calls() == 0);
    CHECK(HttpServerOptions {}.limits.maxBodyBytes.has_value()); // bounded by default
}

TEST_CASE("HttpServer.theHandlerSeesThePeer", "[http][server]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto peer = core::net::ResolvedEndpoint {};
    peer.length = 16;
    peer.family = 2;
    auto pair = core::net::testing::InMemorySocketPair::create(
        0, {}, core::net::testing::InMemoryPairEndpoints { .client = peer, .server = std::nullopt });
    auto handler = EchoHandler {};
    loop.spawn(core::http::serveHttpConnection(&loop, std::move(pair.server), &handler, quietOptions()));
    loop.spawn(core::http::test::writeThenFinish(pair.client.get(), get("/")));
    std::ignore = loop.drain();
    REQUIRE(handler.calls() == 1);
    REQUIRE(handler.lastPeer().has_value());
    CHECK(handler.lastPeer()->family == 2);
    CHECK(handler.lastPeer()->length == 16);
}

namespace
{

/// Connects a client to @p listener, sends @p wire and half-closes, and collects the answer.
struct Client
{
    std::unique_ptr<core::net::testing::InMemorySocket> socket;
    std::string received;
    FlowEnd closed = FlowEnd::Running;
};

void connect(core::net::testing::TestLoop& loop,
             core::net::testing::InMemoryListener& listener,
             Client& client,
             std::string wire)
{
    client.socket = listener.connectClient();
    loop.spawn(core::http::test::collect(client.socket.get(), &client.received, &client.closed));
    loop.spawn(core::http::test::writeOnly(client.socket.get(), std::move(wire)));
}

} // namespace

TEST_CASE("HttpServer.serveHttpServesConnectionsAtOnceUpToItsCap", "[http][server]")
{
    auto clock = core::platform::ManualClock {};
    auto listener = core::net::testing::InMemoryListener {};
    auto first = Client {};
    auto second = Client {};
    auto third = Client {};
    auto loop = core::net::testing::TestLoop { clock };
    auto options = quietOptions();
    auto const policy = std::make_shared<core::net::CountingAdmissionControl>(1);
    options.admission = policy;
    auto events = std::vector<core::net::AcceptLoopEvent> {};
    options.reporting.onEvent = [&events](core::net::AcceptLoopEvent const& event) {
        events.push_back(event);
    };
    auto handler = EchoHandler {};
    auto accepting = FlowEnd::Running;
    loop.spawn(
        core::http::test::marking(core::http::serveHttp(&loop, &listener, &handler, options), &accepting));

    connect(loop, listener, first, get("/first")); // answered, then held open: it is idle, not done
    std::ignore = loop.drain();
    CHECK(parseResponses(first.received).size() == 1);

    connect(loop, listener, second, get("/second")); // past the cap: closed unanswered
    std::ignore = loop.drain();
    CHECK(second.received.empty());
    CHECK(second.closed == FlowEnd::Finished);
    REQUIRE(events.size() == 1); // the refusal is reported, through the injected policy's cap
    CHECK(events.front().kind == core::net::AcceptLoopEventKind::Warning);
    CHECK(events.front().line.contains("admission cap"));
    CHECK(policy->inFlight() == 1);

    first.socket->close(); // gives the slot back
    std::ignore = loop.drain();
    connect(loop, listener, third, get("/third"));
    std::ignore = loop.drain();
    CHECK(parseResponses(third.received).size() == 1);
    CHECK(handler.calls() == 2);

    listener.close();
    std::ignore = loop.drain();
    CHECK(accepting == FlowEnd::Finished);
}

TEST_CASE("HttpServer.slowConnectionsDoNotHoldOneAnother", "[http][server]")
{
    auto clock = core::platform::ManualClock {};
    auto listener = core::net::testing::InMemoryListener {};
    auto slow = Client {};
    auto fast = Client {};
    auto loop = core::net::testing::TestLoop { clock };
    auto handler = EchoHandler {};
    loop.spawn(core::http::serveHttp(&loop, &listener, &handler, quietOptions()));
    connect(loop, listener, slow, "GET / HTTP/1.1\r\nHo"); // a head that never finishes
    connect(loop, listener, fast, get("/fast"));
    std::ignore = loop.drain();
    CHECK(slow.received.empty());
    CHECK(parseResponses(fast.received).size() == 1);
    listener.close();
    std::ignore = loop.drain();
}

namespace
{

/// The kinds of @p events, in order, each followed by "; ".
std::string kindsOf(std::vector<core::net::AcceptLoopEvent> const& events)
{
    auto text = std::string {};
    for (auto const& event: events)
    {
        switch (event.kind)
        {
            case core::net::AcceptLoopEventKind::Warning: text += "warning; "; break;
            case core::net::AcceptLoopEventKind::Degraded: text += "degraded; "; break;
            case core::net::AcceptLoopEventKind::Recovered: text += "recovered; "; break;
            case core::net::AcceptLoopEventKind::GaveUp: text += "gave up; "; break;
            case core::net::AcceptLoopEventKind::Stopped: text += "stopped; "; break;
        }
    }
    return text;
}

/// Options whose accept loop reports into @p events.
HttpServerOptions reportingInto(std::vector<core::net::AcceptLoopEvent>* events)
{
    auto options = quietOptions();
    options.reporting.onEvent = [events](core::net::AcceptLoopEvent const& event) {
        events->push_back(event);
    };
    return options;
}

/// The events of @p events but the rate-limited warnings.
std::vector<core::net::AcceptLoopEvent> withoutWarnings(std::vector<core::net::AcceptLoopEvent> events)
{
    std::erase_if(events, [](core::net::AcceptLoopEvent const& event) {
        return event.kind == core::net::AcceptLoopEventKind::Warning;
    });
    return events;
}

/// Restores the discarding diagnostic sink, whatever a case installed.
struct DiagnosticSinkGuard
{
    DiagnosticSinkGuard() = default;
    DiagnosticSinkGuard(DiagnosticSinkGuard const&) = delete;
    DiagnosticSinkGuard& operator=(DiagnosticSinkGuard const&) = delete;
    DiagnosticSinkGuard(DiagnosticSinkGuard&&) = delete;
    DiagnosticSinkGuard& operator=(DiagnosticSinkGuard&&) = delete;
    ~DiagnosticSinkGuard() { core::net::setDiagnosticSink({}); }
};

} // namespace

TEST_CASE("HttpServer.theAcceptLoopFollowsTheAcceptPolicy", "[http][server][accept-policy]")
{
    // The accept loop is `net::serve`'s; these are that loop's cases, over serveHttp.
    auto clock = core::platform::ManualClock {};
    auto inner = std::make_unique<core::net::testing::InMemoryListener>();
    auto* innerListener = inner.get();
    auto client = Client {};
    auto events = std::vector<core::net::AcceptLoopEvent> {};
    auto accepting = FlowEnd::Running;
    auto handler = EchoHandler {};
    SECTION("a failed accept is backed off on and reported, and the connection behind it served")
    {
        auto listener = core::net::testing::FailingListener {
            std::move(inner),
            { core::net::makeNetError(core::net::NetErrorCode::ResourceExhausted, 0, "accept"),
              core::net::makeNetError(core::net::NetErrorCode::ConnReset, 0, "AcceptEx"),
              core::net::makeNetError(core::net::NetErrorCode::ResourceExhausted, 0, "accept") }
        };
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(core::http::test::marking(
            core::http::serveHttp(&loop, &listener, &handler, reportingInto(&events)), &accepting));
        connect(loop, *innerListener, client, get("/behind"));
        for ([[maybe_unused]] auto const pass: std::array { 1, 2, 3, 4 })
        {
            std::ignore = loop.drain();
            clock.advance(core::net::AcceptErrorPolicy::MaxBackoff);
        }
        std::ignore = loop.drain();
        CHECK(listener.failuresAnswered() == 3);
        CHECK(parseResponses(client.received).size() == 1);
        CHECK(kindsOf(events) == "warning; ");
        CHECK(accepting == FlowEnd::Running);
        listener.close();
        std::ignore = loop.drain();
        CHECK(accepting == FlowEnd::Finished);
    }
    SECTION("a long run of unclassified failures is reported degraded once, then recovered")
    {
        constexpr auto Failures = core::net::AcceptErrorPolicy::UnclassifiedBeforeDegraded + 8;
        auto listener = core::net::testing::FailingListener {
            std::move(inner),
            core::net::testing::repeatedFailures(core::net::NetErrorCode::SystemError, Failures)
        };
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(core::http::test::marking(
            core::http::serveHttp(&loop, &listener, &handler, reportingInto(&events)), &accepting));
        connect(loop, *innerListener, client, get("/after"));
        for ([[maybe_unused]] auto const pass: std::views::iota(0U, Failures + 4))
        {
            std::ignore = loop.drain();
            clock.advance(core::net::AcceptErrorPolicy::MaxBackoff);
        }
        std::ignore = loop.drain();
        CHECK(kindsOf(withoutWarnings(events)) == "degraded; recovered; ");
        CHECK(parseResponses(client.received).size() == 1);
        listener.close();
        std::ignore = loop.drain();
        CHECK(accepting == FlowEnd::Finished);
    }
    SECTION("a degraded loop that is closed says it stopped")
    {
        constexpr auto Failures = core::net::AcceptErrorPolicy::UnclassifiedBeforeDegraded + 8;
        auto listener = core::net::testing::FailingListener {
            std::move(inner),
            core::net::testing::repeatedFailures(core::net::NetErrorCode::SystemError, Failures)
        };
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(core::http::test::marking(
            core::http::serveHttp(&loop, &listener, &handler, reportingInto(&events)), &accepting));
        for ([[maybe_unused]] auto const pass:
             std::views::iota(0U, core::net::AcceptErrorPolicy::UnclassifiedBeforeDegraded + 2))
        {
            std::ignore = loop.drain();
            clock.advance(core::net::AcceptErrorPolicy::MaxBackoff);
        }
        listener.close(); // during a backoff: the loop returns at once
        std::ignore = loop.drain();
        CHECK(accepting == FlowEnd::Finished);
        CHECK(kindsOf(withoutWarnings(events)) == "degraded; stopped; ");
    }
    SECTION("a dead listener is given up on, closed, and reported")
    {
        auto listener = core::net::testing::FailingListener {
            std::move(inner), core::net::testing::repeatedFailures(core::net::NetErrorCode::BadHandle, 1)
        };
        auto const closed = listener.closeToken();
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(core::http::test::marking(
            core::http::serveHttp(&loop, &listener, &handler, reportingInto(&events)), &accepting));
        std::ignore = loop.drain();
        CHECK(accepting == FlowEnd::Finished);
        CHECK(closed.stop_requested());
        REQUIRE(kindsOf(events) == "gave up; ");
        CHECK(events.front().error.code == core::net::NetErrorCode::BadHandle);
    }
    SECTION("with no one to tell, the report goes to the diagnostic sink")
    {
        auto const guard = DiagnosticSinkGuard {};
        auto lines = std::vector<std::string> {};
        core::net::setDiagnosticSink([&lines](std::string_view line) { lines.emplace_back(line); });
        auto listener = core::net::testing::FailingListener {
            std::move(inner), core::net::testing::repeatedFailures(core::net::NetErrorCode::BadHandle, 1)
        };
        auto loop = core::net::testing::TestLoop { clock };
        loop.spawn(core::http::test::marking(
            core::http::serveHttp(&loop, &listener, &handler, quietOptions()), &accepting));
        std::ignore = loop.drain();
        CHECK(accepting == FlowEnd::Finished);
        REQUIRE(lines.size() == 1);
        CHECK(lines.front().starts_with("http"));
    }
}

TEST_CASE("HttpServer.aBodyReadLeftRunningNeverMeetsTheClose", "[http][server]")
{
    auto left = core::http::test::LeftRunning {};
    auto* loop = static_cast<core::net::EventLoop*>(nullptr);
    SECTION("a read still parked is retired, and has returned, before the close lingers")
    {
        // The handler answers once its read has parked on the 7 bytes the client never sends.
        auto handler = core::http::test::SpawningReader { &loop, &left, 1ms };
        auto served = ServedConnection { handler, quietOptions() };
        loop = &served.loop();
        served.send("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc");
        served.advance(1ms);
        served.advance(HttpServerOptions {}.linger.total);
        auto const responses = parseResponses(served.received());
        REQUIRE(responses.size() == 1);
        CHECK(responses[0].body == "ok");
        CHECK(left.text == "abc");
        CHECK(left.failure == HttpErrorCode::ConnectionTerminated);
        CHECK(left.retry == HttpErrorCode::ConnectionTerminated); // the request is over
        CHECK(left.end == FlowEnd::Finished);
        CHECK(served.server() == FlowEnd::Finished);
        CHECK(served.wire() == FlowEnd::Finished);
    }
    SECTION("a kept reader that reads during the close is refused")
    {
        auto handler = IgnoringHandler { KeepsReader::Yes };
        auto served = ServedConnection { handler, quietOptions() };
        served.send("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc");
        REQUIRE(handler.kept().size() == 1);
        CHECK(served.server() == FlowEnd::Running); // lingering: the client has not closed
        served.loop().spawn(core::http::test::readThenRetry(std::move(handler.kept().front()), &left));
        served.drain();
        CHECK(left.failure == HttpErrorCode::ConnectionTerminated);
        CHECK(left.text.empty());
        served.advance(HttpServerOptions {}.linger.total);
        CHECK(served.server() == FlowEnd::Finished);
    }
    SECTION("a read writing the interim response has the connection closed under it")
    {
        // A client that does not read, over a pair that holds 8 bytes: the `100 Continue` stalls.
        auto handler = core::http::test::SpawningReader { &loop, &left, 1ms };
        auto served = ServedConnection { handler, quietOptions(), 8, core::http::test::ClientReads::No };
        loop = &served.loop();
        served.send("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 10\r\n\r\n");
        served.advance(1ms);
        CHECK(served.server() == FlowEnd::Finished);
        CHECK(left.failure.has_value());
    }
}

namespace
{

/// A socket whose `cancelRead` takes effect a turn later, as a completion transport's does: the
/// parked read holds the socket's read slot until then.
class DeferredCancelSocket final: public core::net::testing::SocketDecorator
{
  public:
    DeferredCancelSocket(core::net::ISocket& inner, core::net::EventLoop& loop) noexcept:
        SocketDecorator { inner }, _loop { &loop }
    {
    }

    void cancelRead() noexcept override
    {
        _loop->post([target = &inner()] { target->cancelRead(); });
    }

  private:
    core::net::EventLoop* _loop;
};

} // namespace

TEST_CASE("HttpServer.theCloseWaitsForARetiredReadToReturn", "[http][server]")
{
    // The retired read returns a turn after its cancel: a close that read before then would arm a
    // second read over it, which every socket refuses.
    auto clock = core::platform::ManualClock {};
    auto pair = core::net::testing::InMemorySocketPair::create();
    auto left = core::http::test::LeftRunning {};
    auto received = std::string {};
    auto wire = FlowEnd::Running;
    auto server = FlowEnd::Running;
    auto loop = core::net::testing::TestLoop { clock };
    auto* spawnOn = static_cast<core::net::EventLoop*>(&loop);
    auto handler = core::http::test::SpawningReader { &spawnOn, &left, 1ms };
    loop.spawn(core::http::test::marking(
        core::http::serveHttpConnection(
            &loop, std::make_unique<DeferredCancelSocket>(*pair.server, loop), &handler, quietOptions()),
        &server));
    loop.spawn(core::http::test::collect(pair.client.get(), &received, &wire));
    loop.spawn(core::http::test::writeOnly(pair.client.get(),
                                           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc"));
    std::ignore = loop.drain();
    clock.advance(1ms);
    std::ignore = loop.drain();
    clock.advance(HttpServerOptions {}.linger.total);
    std::ignore = loop.drain();
    CHECK(parseResponses(received).size() == 1);
    CHECK(left.failure == HttpErrorCode::ConnectionTerminated);
    CHECK(server == FlowEnd::Finished);
}
