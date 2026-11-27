// SPDX-License-Identifier: Apache-2.0
//
// `HttpClient` over `testing::ScriptedConnector`'s in-memory pairs on a hand-driven loop: what is
// refused before the dial, how a dial, a TLS wrap and a handshake fail, the request as written, the
// response as read, and how each half of the exchange ends the other. Every flow runs on a `TestLoop`
// over a `ManualClock`, so nothing here waits on anything but the loop the case drives, and a flow
// that never finishes leaves its answer empty instead of hanging the case.
#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/HttpClient.hpp>
#include <core/http/HttpTestSupport.hpp>
#include <core/http/Message.hpp>
#include <core/http/testing/ScriptedConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using core::http::BufferBodySource;
using core::http::HttpClient;
using core::http::HttpClientOptions;
using core::http::HttpErrorCode;
using core::http::HttpResult;
using core::http::HttpTimeouts;
using core::http::IncomingResponse;
using core::http::OutgoingRequest;
using core::http::test::AfterReply;
using core::http::test::answerHeadOnly;
using core::http::test::get;
using core::http::test::HandshakeOutcome;
using core::http::test::into;
using core::http::test::LegacyTlsContext;
using core::http::test::readAll;
using core::http::test::ReadBody;
using core::http::test::ReadOutcome;
using core::http::test::readUntilClosed;
using core::http::test::ScriptedServer;
using core::http::test::ScriptedTlsContext;
using core::http::test::scriptOf;
using core::http::test::ServedRequest;
using core::http::test::SlowBodySource;
using core::http::test::startSend;
using core::http::test::TlsScript;
using core::http::test::writeOnly;
using core::http::testing::ScriptedConnector;
using core::net::makeNetError;
using core::net::NetErrorCode;
using core::net::TlsFailure;
using core::net::TlsFailureKind;
using core::net::testing::TestLoop;
using core::platform::ManualClock;

using namespace std::chrono_literals;

namespace
{

using Answer = std::optional<HttpResult<IncomingResponse>>;

/// A loop driven by hand over a clock moved by hand, and a connector whose pairs hold at most
/// @c window bytes in each direction (zero: no bound).
struct Rig
{
    explicit Rig(std::size_t window = 0): connector { window } {}

    ManualClock clock {};
    TestLoop loop { clock };
    ScriptedConnector connector;
};

/// @return The server end of the oldest dial, as a scripted server; a missing dial fails the case.
std::unique_ptr<ScriptedServer> acceptServer(Rig& rig)
{
    auto socket = rig.connector.acceptNext();
    REQUIRE(socket != nullptr);
    return std::make_unique<ScriptedServer>(rig.loop, std::move(socket));
}

/// @return @p request with @p fields appended; a field the factories refuse fails the case.
OutgoingRequest withFields(OutgoingRequest request,
                           std::vector<std::pair<std::string, std::string>> const& fields)
{
    for (auto const& [name, value]: fields)
        REQUIRE(request.head.fields.append(name, value).has_value());
    return request;
}

/// @return The error @p answer failed with; an answer that is missing or succeeded fails the case.
core::http::HttpError const& failureOf(Answer const& answer)
{
    REQUIRE(answer.has_value());
    REQUIRE_FALSE(answer->has_value());
    return answer->error();
}

/// Reads @p answer's response body to its end, on @p rig's loop.
ReadBody bodyOf(Rig& rig, Answer& answer)
{
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    auto body = std::optional<ReadBody> {};
    rig.loop.spawn(into(readAll((*answer)->body.get(), 1000), &body));
    std::ignore = rig.loop.drain();
    REQUIRE(body.has_value());
    return std::move(*body);
}

/// The values of the field @p name in what the server read.
std::vector<std::string> fieldValues(ServedRequest const& served, std::string_view name)
{
    REQUIRE(served.head.has_value());
    auto values = std::vector<std::string> {};
    for (auto const value: served.head->fields.get(name))
        values.emplace_back(value);
    return values;
}

/// @return The client options with @p timeouts, the rest as the defaults say.
HttpClientOptions optionsWith(HttpTimeouts timeouts)
{
    auto options = HttpClientOptions {};
    options.timeouts = timeouts;
    return options;
}

constexpr auto OkReply = std::string_view { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" };

} // namespace

TEST_CASE("HttpClient.aGetRoundTrips", "[http][client]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test:8080/where?q=1"), &answer);
    std::ignore = rig.loop.drain();
    REQUIRE(rig.connector.connects() == 1);
    CHECK(rig.connector.dials()[0].host == "example.test");
    CHECK(rig.connector.dials()[0].port == 8080);

    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    REQUIRE(served.has_value());
    REQUIRE(served->head.has_value());
    CHECK(served->head->method.text() == "GET");
    CHECK(served->head->target.text() == "/where?q=1");
    REQUIRE_FALSE(served->head->fields.entries().empty());
    CHECK(served->head->fields.entries().front().name == "Host"); // first, as RFC 9112 section 3.2 asks
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK((*answer)->head.status == 200);
    CHECK(bodyOf(rig, answer).text == "ok");
    CHECK(client.idleConnections() == 1); // read to its end, so pooled
}

TEST_CASE("HttpClient.dialFailuresMapThroughTheTable", "[http][client]")
{
    struct Row
    {
        NetErrorCode net;
        HttpErrorCode http;
    };
    constexpr auto Rows = std::array {
        Row { NetErrorCode::NameNotFound, HttpErrorCode::DnsError },
        Row { NetErrorCode::ResolverTemporaryFailure, HttpErrorCode::DnsError },
        Row { NetErrorCode::AddressError, HttpErrorCode::DnsError },
        Row { NetErrorCode::ConnRefused, HttpErrorCode::ConnectionRefused },
        Row { NetErrorCode::HostUnreach, HttpErrorCode::DestinationUnavailable },
        Row { NetErrorCode::Timeout, HttpErrorCode::ConnectionTimeout },
        Row { NetErrorCode::ConnReset, HttpErrorCode::ConnectionTerminated },
        Row { NetErrorCode::Eof, HttpErrorCode::ConnectionTerminated },
        Row { NetErrorCode::PermissionDenied, HttpErrorCode::InternalError }, // no row: the default
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(core::net::toString(row.net))
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            rig.connector.refuseNext(makeNetError(row.net, 0, "scripted"));
            auto answer = Answer {};
            startSend(rig.loop, client, get("http://example.test/"), &answer);
            std::ignore = rig.loop.drain();
            auto const& error = failureOf(answer);
            CHECK(error.code == row.http);
            CHECK(error.detail.contains("scripted"));
            CHECK(rig.connector.acceptNext() == nullptr);
        }
    }
}

TEST_CASE("HttpClient.aSilentServerIsAResponseTimeout", "[http][client]")
{
    auto rig = Rig {};
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     nullptr,
                     optionsWith(HttpTimeouts { .connect = 0ms, .firstByte = 100ms, .betweenBytes = 0ms }) };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    REQUIRE(served.has_value());
    rig.clock.advance(99ms);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.clock.advance(1ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ResponseTimeout);
}

TEST_CASE("HttpClient.aRequestsOwnTimeoutsReplaceTheClients", "[http][client]")
{
    auto rig = Rig {};
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     nullptr,
                     optionsWith(HttpTimeouts { .connect = 5s, .firstByte = 0ms, .betweenBytes = 0ms }) };
    SECTION("the client's")
    {
        auto answer = Answer {};
        startSend(rig.loop, client, get("http://example.test/"), &answer);
        std::ignore = rig.loop.drain();
        REQUIRE(rig.connector.connects() == 1);
        CHECK(rig.connector.dials()[0].options.connectTimeout == 5s);
        // End the exchange here, while its answer is in scope: the dropped server end ends it.
        rig.connector.acceptNext().reset();
        std::ignore = rig.loop.drain();
        CHECK(failureOf(answer).code == HttpErrorCode::ConnectionTerminated);
    }
    SECTION("the request's")
    {
        auto request = get("http://example.test/");
        request.timeouts = HttpTimeouts { .connect = 7s, .firstByte = 100ms, .betweenBytes = 0ms };
        auto answer = Answer {};
        startSend(rig.loop, client, std::move(request), &answer);
        std::ignore = rig.loop.drain();
        REQUIRE(rig.connector.connects() == 1);
        CHECK(rig.connector.dials()[0].options.connectTimeout == 7s);
        auto server = acceptServer(rig);
        auto served = std::optional<ServedRequest> {};
        rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
        rig.clock.advance(100ms);
        std::ignore = rig.loop.drain();
        CHECK(failureOf(answer).code == HttpErrorCode::ResponseTimeout);
    }
}

TEST_CASE("HttpClient.zeroTimeoutsAreNone", "[http][client]")
{
    // R31: zero bounds nothing. The dial is left to the platform; a silent server is waited for.
    auto rig = Rig {};
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     nullptr,
                     optionsWith(HttpTimeouts { .connect = 0ms, .firstByte = 0ms, .betweenBytes = 0ms }) };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    REQUIRE(rig.connector.connects() == 1);
    CHECK(rig.connector.dials()[0].options.connectTimeout == 0ms);
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    rig.clock.advance(24h);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.loop.spawn(writeOnly(&server->connection().socket(), std::string { OkReply }));
    std::ignore = rig.loop.drain();
    REQUIRE(answer.has_value());
    CHECK(answer->has_value());
}

TEST_CASE("HttpClient.theDefaultTimeoutsAre600Seconds", "[http][client]")
{
    // wasmtime's wasi:http defaults: a client built with no options still bounds every wait.
    auto const defaults = HttpTimeouts {};
    CHECK(defaults.connect == 600s);
    CHECK(defaults.firstByte == 600s);
    CHECK(defaults.betweenBytes == 600s);

    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    REQUIRE(rig.connector.connects() == 1);
    CHECK(rig.connector.dials()[0].options.connectTimeout == 600s);
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    rig.clock.advance(599s);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.clock.advance(1s);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ResponseTimeout);
}

TEST_CASE("HttpClient.httpsWithoutATlsContextIsAConfigurationError", "[http][client]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ConfigurationError);
    CHECK(rig.connector.connects() == 0);
}

TEST_CASE("HttpClient.httpsOverALegacyContextIsAConfigurationError", "[http][client]")
{
    // C3/R23: a context that does not override wrapClient cannot bind the name, so the default
    // refuses it, and the client never falls back to a connection unverified for the host.
    auto rig = Rig {};
    auto tls = LegacyTlsContext {};
    auto client = HttpClient { rig.loop, rig.connector, &tls };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ConfigurationError);
    CHECK(rig.connector.connects() == 1);
    CHECK(tls.wraps() == 0);
    auto server = rig.connector.acceptNext();
    REQUIRE(server != nullptr);
    auto received = std::optional<std::string> {};
    rig.loop.spawn(into(readUntilClosed(server.get()), &received));
    std::ignore = rig.loop.drain();
    REQUIRE(received.has_value()); // the dialled socket was dropped, unwritten
    CHECK(received->empty());
}

TEST_CASE("HttpClient.wrapRefusalsMapThroughTheTable", "[http][client]")
{
    struct Row
    {
        NetErrorCode net;
        HttpErrorCode http;
    };
    constexpr auto Rows = std::array {
        Row { NetErrorCode::Unsupported, HttpErrorCode::ConfigurationError },
        Row { NetErrorCode::AddressError, HttpErrorCode::RequestUriInvalid },
        Row { NetErrorCode::ResourceExhausted, HttpErrorCode::InternalError }, // no row: the default
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(core::net::toString(row.net))
        {
            auto rig = Rig {};
            auto script = TlsScript {};
            script.wrapError = makeNetError(row.net);
            auto tls = ScriptedTlsContext { rig.loop, script };
            auto client = HttpClient { rig.loop, rig.connector, &tls };
            auto answer = Answer {};
            startSend(rig.loop, client, get("https://example.test/"), &answer);
            std::ignore = rig.loop.drain();
            CHECK(failureOf(answer).code == row.http);
            CHECK(tls.record().wraps == 0);
        }
    }
}

TEST_CASE("HttpClient.aRefusedHandshakeIsItsTlsFailure", "[http][client]")
{
    // Reported from handshakeIfNeeded right after wrapClient, with the failure attached, and before
    // any request byte: never as a write error.
    struct Row
    {
        TlsFailureKind kind;
        HttpErrorCode http;
    };
    constexpr auto Rows = std::array {
        Row { TlsFailureKind::Certificate, HttpErrorCode::TlsCertificateError },
        Row { TlsFailureKind::Protocol, HttpErrorCode::TlsProtocolError },
        Row { TlsFailureKind::AlertReceived, HttpErrorCode::TlsAlertReceived },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION("kind " << static_cast<int>(row.kind))
        {
            auto rig = Rig {};
            auto const failure =
                TlsFailure { .kind = row.kind, .alert = std::uint8_t { 42 }, .message = "scripted refusal" };
            auto script = scriptOf(HandshakeOutcome::RefusedByTls);
            script.failure = failure;
            auto tls = ScriptedTlsContext { rig.loop, script };
            auto client = HttpClient { rig.loop, rig.connector, &tls };
            auto answer = Answer {};
            startSend(rig.loop, client, get("https://example.test:8443/"), &answer);
            std::ignore = rig.loop.drain();
            auto const& error = failureOf(answer);
            CHECK(error.code == row.http);
            REQUIRE(error.tls.has_value());
            CHECK(error.tls->kind == row.kind);
            CHECK(error.tls->alert == std::optional { std::uint8_t { 42 } });
            CHECK(error.tls->message == "scripted refusal");
            CHECK(tls.record().handshakes == 1);
            CHECK(tls.record().writes == 0);
            REQUIRE(tls.record().serverNames.size() == 1);
            CHECK(tls.record().serverNames[0] == "example.test"); // the host, never host:port
        }
    }
}

TEST_CASE("HttpClient.tlsIsBoundToTheBareHost", "[http][client]")
{
    // The port is split off for the name TLS checks, and an IPv6 literal goes without its brackets.
    auto rig = Rig {};
    auto tls = ScriptedTlsContext { rig.loop, scriptOf(HandshakeOutcome::RefusedByTls) };
    auto client = HttpClient { rig.loop, rig.connector, &tls };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://[::1]:8443/"), &answer);
    std::ignore = rig.loop.drain();
    REQUIRE(answer.has_value());
    REQUIRE(tls.record().serverNames.size() == 1);
    CHECK(tls.record().serverNames[0] == "::1");
    REQUIRE(rig.connector.connects() == 1);
    CHECK(rig.connector.dials()[0].host == "::1");
    CHECK(rig.connector.dials()[0].port == 8443);
}

TEST_CASE("HttpClient.aHandshakeTheTransportCutMapsThroughTheTable", "[http][client]")
{
    // No TLS failure behind it: the socket's error says what happened.
    auto rig = Rig {};
    auto script = scriptOf(HandshakeOutcome::CutByTransport);
    script.transportError = makeNetError(NetErrorCode::ConnReset);
    auto tls = ScriptedTlsContext { rig.loop, script };
    auto client = HttpClient { rig.loop, rig.connector, &tls };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto const& error = failureOf(answer);
    CHECK(error.code == HttpErrorCode::ConnectionTerminated);
    CHECK_FALSE(error.tls.has_value());
}

TEST_CASE("HttpClient.aStalledHandshakeIsAConnectionTimeout", "[http][client]")
{
    auto rig = Rig {};
    auto script = scriptOf(HandshakeOutcome::ParksThenSucceeds);
    script.park = 1h;
    auto tls = ScriptedTlsContext { rig.loop, script };
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     &tls,
                     optionsWith(HttpTimeouts { .connect = 100ms, .firstByte = 0ms, .betweenBytes = 0ms }) };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    rig.clock.advance(99ms);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.clock.advance(1ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ConnectionTimeout);
    CHECK(tls.record().writes == 0);
}

TEST_CASE("HttpClient.aZeroConnectTimeoutLeavesTheHandshakeUnbounded", "[http][client]")
{
    // R31, for the handshake; and the exchange over TLS once it is settled.
    auto rig = Rig {};
    auto script = scriptOf(HandshakeOutcome::ParksThenSucceeds);
    script.park = 24h;
    auto tls = ScriptedTlsContext { rig.loop, script };
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     &tls,
                     optionsWith(HttpTimeouts { .connect = 0ms, .firstByte = 0ms, .betweenBytes = 0ms }) };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    rig.clock.advance(23h);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    CHECK(tls.record().writes == 0);
    rig.clock.advance(1h);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    REQUIRE(served.has_value());
    CHECK(fieldValues(*served, "Host")
          == std::vector<std::string> { "example.test" }); // 443 is https's default
    CHECK(tls.record().writesBeforeHandshake == 0);
    CHECK(bodyOf(rig, answer).text == "ok");
}

TEST_CASE("HttpClient.aTlsFailureMidExchangeIsItsTlsFailure", "[http][client]")
{
    // The record layer refuses after the handshake: the socket's error is a SystemError, but the
    // session says why, and that is the answer.
    auto rig = Rig {};
    auto const failure = TlsFailure { .kind = TlsFailureKind::AlertReceived,
                                      .alert = std::uint8_t { 20 },
                                      .message = "bad_record_mac" };
    auto script = scriptOf(HandshakeOutcome::Succeeds);
    script.reads = ReadOutcome::RefusedByTls;
    script.failure = failure;
    auto tls = ScriptedTlsContext { rig.loop, script };
    auto client = HttpClient { rig.loop, rig.connector, &tls };
    auto answer = Answer {};
    startSend(rig.loop, client, get("https://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto const& error = failureOf(answer);
    CHECK(error.code == HttpErrorCode::TlsAlertReceived);
    REQUIRE(error.tls.has_value());
    CHECK(error.tls->alert == std::optional { std::uint8_t { 20 } });
}

TEST_CASE("HttpClient.interimResponsesAreSkipped", "[http][client]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer("HTTP/1.1 100 Continue\r\n\r\n"
                                       "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"
                                       "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\ndone",
                                       AfterReply::StaysOpen),
                        &served));
    std::ignore = rig.loop.drain();
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK((*answer)->head.status == 201);
    CHECK(bodyOf(rig, answer).text == "done");
}

TEST_CASE("HttpClient.moreThan32InterimResponsesAreAProtocolError", "[http][client]")
{
    // Each interim head costs a read and nothing else: without a cap, a server could keep a request
    // reading 1xx heads for as long as it liked, past every byte bound.
    auto const interim = GENERATE(32, 33);
    CAPTURE(interim);
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto reply = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, interim))
        reply += "HTTP/1.1 103 Early Hints\r\n\r\n";
    reply += "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer(std::move(reply), AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    if (interim == 32)
    {
        REQUIRE(answer.has_value());
        REQUIRE(answer->has_value());
        CHECK((*answer)->head.status == 200);
    }
    else
        CHECK(failureOf(answer).code == HttpErrorCode::ProtocolError);
}

TEST_CASE("HttpClient.a101IsAProtocolError", "[http][client]")
{
    // No request asks to upgrade (Upgrade is a framing field the client refuses), so a 101 is not an
    // interim answer to skip: the stream after it is another protocol's.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(
        server->answer("HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n" + std::string { OkReply },
                       AfterReply::StaysOpen),
        &served));
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ProtocolError);
}

TEST_CASE("HttpClient.hostIsWrittenFromTheOrigin", "[http][client]")
{
    struct Row
    {
        std::string_view url;
        std::string_view host;     ///< The Host field written.
        std::string_view dialHost; ///< What the connector was asked for.
    };
    constexpr auto Rows = std::array {
        Row { "http://example.test/", "example.test", "example.test" },
        Row { "http://example.test:80/", "example.test", "example.test" },
        Row { "http://example.test:8080/", "example.test:8080", "example.test" },
        Row { "http://192.0.2.1:81/", "192.0.2.1:81", "192.0.2.1" },
        Row { "http://[2001:db8::1]/", "[2001:db8::1]", "2001:db8::1" },
        Row { "http://[2001:db8::1]:8080/", "[2001:db8::1]:8080", "2001:db8::1" },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.url)
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto answer = Answer {};
            startSend(rig.loop, client, get(row.url), &answer);
            std::ignore = rig.loop.drain();
            REQUIRE(rig.connector.connects() == 1);
            CHECK(rig.connector.dials()[0].host == row.dialHost);
            auto server = acceptServer(rig);
            auto served = std::optional<ServedRequest> {};
            rig.loop.spawn(into(server->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
            std::ignore = rig.loop.drain();
            REQUIRE(served.has_value());
            CHECK(fieldValues(*served, "Host") == std::vector<std::string> { std::string { row.host } });
            REQUIRE(answer.has_value());
            CHECK(answer->has_value());
        }
    }
    SECTION("an unbracketed IPv6 host is bracketed")
    {
        auto rig = Rig {};
        auto client = HttpClient { rig.loop, rig.connector, nullptr };
        auto request = get("http://example.test/");
        request.origin.host = "2001:db8::1";
        auto answer = Answer {};
        startSend(rig.loop, client, std::move(request), &answer);
        std::ignore = rig.loop.drain();
        auto server = acceptServer(rig);
        auto served = std::optional<ServedRequest> {};
        rig.loop.spawn(into(server->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
        std::ignore = rig.loop.drain();
        REQUIRE(served.has_value());
        CHECK(fieldValues(*served, "Host") == std::vector<std::string> { "[2001:db8::1]" });
    }
}

TEST_CASE("HttpClient.aCallersHostOrFramingFieldIsRefusedBeforeTheDial", "[http][client]")
{
    constexpr auto Names = std::array<std::string_view, 7> {
        "Transfer-Encoding", "Content-Length", "Connection", "Upgrade", "TE", "Host", "host",
    };
    for (auto const name: Names)
    {
        DYNAMIC_SECTION(name)
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto answer = Answer {};
            startSend(rig.loop,
                      client,
                      withFields(get("http://example.test/"), { { std::string { name }, "chunked" } }),
                      &answer);
            std::ignore = rig.loop.drain();
            auto const& error = failureOf(answer);
            CHECK(error.code == HttpErrorCode::ProtocolError);
            CHECK(error.fieldName == std::optional<std::string> { std::string { name } });
            CHECK(rig.connector.connects() == 0);
        }
    }
}

TEST_CASE("HttpClient.aMalformedHostIsRefusedBeforeTheDial", "[http][client]")
{
    constexpr auto Hosts = std::array<std::string_view, 9> {
        "", "[]", "a b", "x/y", "a@b", "fe80::1%eth0", "fe80::1%251", "example.test:8080", "cafe:8080",
    };
    for (auto const host: Hosts)
    {
        DYNAMIC_SECTION("'" << host << "'")
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto request = get("http://example.test/");
            request.origin.host = std::string { host };
            auto answer = Answer {};
            startSend(rig.loop, client, std::move(request), &answer);
            std::ignore = rig.loop.drain();
            CHECK(failureOf(answer).code == HttpErrorCode::RequestUriInvalid);
            CHECK(rig.connector.connects() == 0);
        }
    }
}

TEST_CASE("HttpClient.anUploadCountsTowardTheResponseTimeout", "[http][client]")
{
    // The first-byte timeout follows wasmtime's (P13 Task 16 ruling): it runs from the start of the
    // exchange, the upload included. A body slower than it fails the request, and the write parked on
    // the body source stops with the read, so the answer comes at the deadline, not after the body.
    auto rig = Rig {};
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     nullptr,
                     optionsWith(HttpTimeouts { .connect = 0ms, .firstByte = 100ms, .betweenBytes = 0ms }) };
    auto request = get("http://example.test/upload", "PUT");
    request.body = std::make_unique<SlowBodySource>(
        rig.loop, std::vector<std::string> { "aaaa", "bbbb", "cccc" }, 300ms);
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    rig.clock.advance(99ms);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.clock.advance(1ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ResponseTimeout);
}

TEST_CASE("HttpClient.aServerThatNeverReadsIsBoundedByTheResponseTimeout", "[http][client]")
{
    // The upload stalls on a 16-byte window the server never drains; the response timeout ends the
    // request, and its close ends the stalled write.
    auto rig = Rig { 16 };
    auto client =
        HttpClient { rig.loop,
                     rig.connector,
                     nullptr,
                     optionsWith(HttpTimeouts { .connect = 0ms, .firstByte = 100ms, .betweenBytes = 0ms }) };
    auto request = get("http://example.test/upload", "PUT");
    request.body = std::make_unique<BufferBodySource>(std::string(1000, 'x'));
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = rig.connector.acceptNext();
    REQUIRE(server != nullptr);
    rig.clock.advance(99ms);
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(answer.has_value());
    rig.clock.advance(1ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ResponseTimeout);
}

TEST_CASE("HttpClient.aReadFailureStopsAWriteParkedOnItsSource", "[http][client]")
{
    // The body source waits an hour for its next chunk and honours its flow's stop token; the server
    // answers garbage. The read's failure is the answer at once, not in an hour.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto request = get("http://example.test/upload", "PUT");
    request.body =
        std::make_unique<SlowBodySource>(rig.loop, std::vector<std::string> { "aaaa", "bbbb" }, 1h);
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = rig.connector.acceptNext();
    REQUIRE(server != nullptr);
    rig.loop.spawn(writeOnly(server.get(), "this is no status line\r\n\r\n"));
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ProtocolError);
}

TEST_CASE("HttpClient.anOriginWithoutAPortUsesTheSchemesDefault", "[http][client]")
{
    struct Row
    {
        core::http::Scheme scheme;
        std::uint16_t port;
    };
    constexpr auto Rows =
        std::array { Row { core::http::Scheme::Http, 80 }, Row { core::http::Scheme::Https, 443 } };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION("port " << row.port)
        {
            auto origin = core::http::Origin {};
            origin.scheme = row.scheme;
            CHECK(origin.effectivePort() == row.port);
            auto rig = Rig {};
            auto tls = ScriptedTlsContext { rig.loop, scriptOf(HandshakeOutcome::Succeeds) };
            auto client = HttpClient { rig.loop, rig.connector, &tls };
            auto request = get("http://example.test/");
            request.origin.scheme = row.scheme;
            REQUIRE_FALSE(request.origin.port.has_value());
            auto answer = Answer {};
            startSend(rig.loop, client, std::move(request), &answer);
            std::ignore = rig.loop.drain();
            REQUIRE(rig.connector.connects() == 1);
            CHECK(rig.connector.dials()[0].port == row.port);
            auto server = acceptServer(rig);
            auto served = std::optional<ServedRequest> {};
            rig.loop.spawn(into(server->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
            std::ignore = rig.loop.drain();
            REQUIRE(served.has_value());
            CHECK(fieldValues(*served, "Host") == std::vector<std::string> { "example.test" });
        }
    }
}

TEST_CASE("HttpClient.aFailedUploadAnswersItsOwnFailure", "[http][client]")
{
    // The body source fails: the write's failure is the answer, and it closes the connection, so the
    // response read -- unbounded here -- ends with it instead of waiting on a server that waits too.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto request = get("http://example.test/upload", "PUT");
    request.body =
        std::make_unique<SlowBodySource>(rig.loop, std::vector<std::string> { "aaaa", "bbbb" }, 10ms, 1);
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer({}, AfterReply::StaysOpen), &served));
    rig.clock.advance(10ms);
    std::ignore = rig.loop.drain();
    rig.clock.advance(10ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::InternalError);
}

TEST_CASE("HttpClient.aFailedReadEndsTheUpload", "[http][client]")
{
    // The server answers garbage and reads nothing, so the upload stalls on a 16-byte window: the
    // read's failure closes the connection, which ends the stalled write, and is the answer.
    auto rig = Rig { 16 };
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto request = get("http://example.test/upload", "PUT");
    request.body = std::make_unique<BufferBodySource>(std::string(1000, 'x'));
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = rig.connector.acceptNext();
    REQUIRE(server != nullptr);
    rig.loop.spawn(writeOnly(server.get(), "this is no status line\r\n\r\n"));
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ProtocolError);
}

TEST_CASE("HttpClient.anUploadThePeerCutStillHearsItsAnswer", "[http][client]")
{
    // A server that refuses an upload answers and closes without reading it; the write fails because
    // the peer is gone, and the response it left is still read.
    auto rig = Rig { 16 };
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto request = get("http://example.test/upload", "PUT");
    request.body = std::make_unique<BufferBodySource>(std::string(1000, 'x'));
    auto answer = Answer {};
    startSend(rig.loop, client, std::move(request), &answer);
    std::ignore = rig.loop.drain();
    auto server = rig.connector.acceptNext();
    REQUIRE(server != nullptr);
    rig.loop.spawn(core::http::test::writeAndClose(
        server.get(), "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
    std::ignore = rig.loop.drain();
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK((*answer)->head.status == 413);
}

TEST_CASE("HttpClient.aResponseFramingRefusalIsTheAnswer", "[http][client]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(server->answer("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
                                       AfterReply::StaysOpen),
                        &served));
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answer).code == HttpErrorCode::ProtocolError);
}

TEST_CASE("HttpClient.aHeadRequestsResponseHasNoBody", "[http][client]")
{
    // The response is framed by the request's method: a HEAD's announces a length it does not send.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto answer = Answer {};
    startSend(rig.loop, client, get("http://example.test/", "HEAD"), &answer);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(
        server->answer("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    auto const body = bodyOf(rig, answer);
    CHECK(body.text.empty());
    CHECK_FALSE(body.error.has_value());
}

// -------------------------------------------------------------------------------------------------
// The keep-alive pool: what goes back, what does not, how long it stays, how many there may be, and
// which failures of a pooled connection are sent again.

namespace
{

/// One request as a server end saw it, and the client's answer to it.
struct Exchanged
{
    Answer answer;
    std::optional<ServedRequest> served;  ///< A connection's first request, read by a @c ScriptedServer.
    std::optional<std::string> startLine; ///< A later one's, read raw (@c answerHeadOnly).
};

/// Sends @p request and lets @p server answer it with @p reply, staying open. A null @p server
/// accepts the request's dial and reads the request through a @c ScriptedServer; a server already set
/// must be the connection the request reuses, and reads the (bodiless) request's head raw.
/// @param out Where the answer and what the server read go; must outlive the case's last drain.
void exchangeOn(Rig& rig,
                HttpClient& client,
                OutgoingRequest request,
                std::unique_ptr<ScriptedServer>& server,
                std::string_view reply,
                Exchanged* out)
{
    startSend(rig.loop, client, std::move(request), &out->answer);
    std::ignore = rig.loop.drain();
    if (server == nullptr)
    {
        server = acceptServer(rig);
        rig.loop.spawn(into(server->answer(std::string { reply }, AfterReply::StaysOpen), &out->served));
    }
    else
        rig.loop.spawn(
            into(answerHeadOnly(&server->connection().socket(), std::string { reply }), &out->startLine));
    std::ignore = rig.loop.drain();
}

/// @return The target the server read; a request it did not read fails the case.
std::string targetOf(Exchanged const& exchanged)
{
    if (exchanged.startLine.has_value())
    {
        auto const& line = *exchanged.startLine;
        auto const from = line.find(' ');
        REQUIRE(from != std::string::npos);
        return line.substr(from + 1, line.find(' ', from + 1) - from - 1);
    }
    REQUIRE(exchanged.served.has_value());
    REQUIRE(exchanged.served->head.has_value());
    return std::string { exchanged.served->head->target.text() };
}

/// @return Whether @p socket's peer has closed it: a read to its end, on @p rig's loop, finished. A
///         read still parked is ended by closing @p socket, so it never outlives the answer it fills.
bool peerClosed(Rig& rig, core::net::ISocket& socket)
{
    auto rest = std::optional<std::string> {};
    rig.loop.spawn(into(readUntilClosed(&socket), &rest));
    std::ignore = rig.loop.drain();
    auto const closed = rest.has_value();
    if (!closed)
    {
        socket.close();
        std::ignore = rig.loop.drain();
    }
    return closed;
}

/// @return The client options with @p change applied to the defaults.
template <typename Change>
HttpClientOptions optionsWhere(Change change)
{
    auto options = HttpClientOptions {};
    change(options);
    return options;
}

} // namespace

TEST_CASE("HttpClient.aFullyReadResponseReturnsItsConnection", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/one"), server, OkReply, &first);
    CHECK(targetOf(first) == "/one");
    CHECK(client.idleConnections() == 0); // the body is still unread
    CHECK(bodyOf(rig, first.answer).text == "ok");
    CHECK(client.idleConnections() == 1);

    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/two"), server, OkReply, &second);
    CHECK(rig.connector.connects() == 1);
    CHECK(targetOf(second) == "/two");
    CHECK(client.idleConnections() == 0); // lent out again
    CHECK(bodyOf(rig, second.answer).text == "ok");
    CHECK(client.idleConnections() == 1);
}

TEST_CASE("HttpClient.anOriginWithoutAPortSharesTheDefaultPortsConnections", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig, client, get("http://Example.TEST/one"), server, OkReply, &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test:80/two"), server, OkReply, &second);
    CHECK(rig.connector.connects() == 1);
    CHECK(targetOf(second) == "/two");
    CHECK(bodyOf(rig, second.answer).text == "ok");
}

TEST_CASE("HttpClient.aPooledResponseKeepsItsTrailers", "[http][client][pool]")
{
    // The connection goes back at the body's end and the next request reuses it; the first response's
    // trailers are still its own.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one"),
               server,
               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\nX-Sum: 7\r\n\r\n",
               &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    CHECK(client.idleConnections() == 1);
    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/two"), server, OkReply, &second);
    CHECK(rig.connector.connects() == 1);
    CHECK(bodyOf(rig, second.answer).text == "ok");

    auto trailers = std::optional<HttpResult<core::http::Fields>> {};
    rig.loop.spawn(into((*first.answer)->body->trailers(), &trailers));
    std::ignore = rig.loop.drain();
    REQUIRE(trailers.has_value());
    REQUIRE(trailers->has_value());
    auto const sum = (*trailers)->get("X-Sum");
    REQUIRE(std::ranges::distance(sum) == 1);
    CHECK(*sum.begin() == "7");
}

TEST_CASE("HttpClient.anAbandonedBodyClosesItsConnection", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc",
               &first);
    REQUIRE(first.answer.has_value());
    REQUIRE(first.answer->has_value());
    (*first.answer)->body.reset();
    CHECK(client.idleConnections() == 0);
    CHECK(peerClosed(rig, server->connection().socket()));

    auto second = Answer {};
    startSend(rig.loop, client, get("http://example.test/two"), &second);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    auto secondServer = acceptServer(rig);
    secondServer->connection().close(); // ends the parked send while `second` is in scope
    std::ignore = rig.loop.drain();
    CHECK(second.has_value());
}

TEST_CASE("HttpClient.aBodyDroppedUnderAParkedReadClosesItsConnection", "[http][client][pool]")
{
    // The read in flight keeps the connection's state alive past the reader; the drop still closes
    // the connection, which ends that read instead of leaving it parked on an open socket.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n",
               &first);
    REQUIRE(first.answer.has_value());
    REQUIRE(first.answer->has_value());
    auto chunk = std::optional<HttpResult<core::http::BodyChunk>> {};
    rig.loop.spawn(into((*first.answer)->body->read(100), &chunk));
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(chunk.has_value());
    (*first.answer)->body.reset();
    std::ignore = rig.loop.drain();
    CHECK(chunk.has_value());
    CHECK(client.idleConnections() == 0);
    CHECK(peerClosed(rig, server->connection().socket()));
}

TEST_CASE("HttpClient.aBodilessResponseReturnsItsConnectionAtOnce", "[http][client][pool]")
{
    // A HEAD's response has no body to read, so nothing would ever reach its end: the connection goes
    // back with the head.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one", "HEAD"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n",
               &first);
    REQUIRE(first.answer.has_value());
    REQUIRE(first.answer->has_value());
    CHECK(client.idleConnections() == 1);
    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/two"), server, OkReply, &second);
    CHECK(rig.connector.connects() == 1);
    CHECK(bodyOf(rig, second.answer).text == "ok");
    // The first response's body reader still answers that its body is over.
    CHECK(bodyOf(rig, first.answer).text.empty());
}

TEST_CASE("HttpClient.aConnectionWithUnreadBytesIsNotPooled", "[http][client][pool]")
{
    // A HEAD's answer framed by Content-Length carries no body, so the ten bytes the server sends after
    // it are no part of any response: a pooled connection would hand them to the next request as its
    // answer. The connection is closed instead and the next request dials afresh.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one", "HEAD"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789",
               &first);
    REQUIRE(first.answer.has_value());
    REQUIRE(first.answer->has_value());
    CHECK(client.idleConnections() == 0);
    auto fresh = std::unique_ptr<ScriptedServer> {};
    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/two"), fresh, OkReply, &second);
    CHECK(rig.connector.connects() == 2);
    CHECK(bodyOf(rig, second.answer).text == "ok");
}

TEST_CASE("HttpClient.aConnectionTheResponseClosesIsNotPooled", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               client,
               get("http://example.test/one"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok",
               &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    CHECK(client.idleConnections() == 0);
    CHECK(peerClosed(rig, server->connection().socket()));
}

TEST_CASE("HttpClient.theIdleCapClosesTheSurplus", "[http][client][pool]")
{
    auto const cap = GENERATE(std::size_t { 0 }, std::size_t { 1 });
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([cap](HttpClientOptions& o) {
                                   o.maxIdlePerOrigin = cap;
                               }) };
    auto answers = std::array<Answer, 2> {};
    startSend(rig.loop, client, get("http://example.test/a"), answers.data());
    startSend(rig.loop, client, get("http://example.test/b"), &answers[1]);
    std::ignore = rig.loop.drain();
    REQUIRE(rig.connector.connects() == 2);
    auto servers = std::array { acceptServer(rig), acceptServer(rig) };
    auto served = std::array<std::optional<ServedRequest>, 2> {};
    for (auto const index: { std::size_t { 0 }, std::size_t { 1 } })
        rig.loop.spawn(into(servers.at(index)->answer(std::string { OkReply }, AfterReply::StaysOpen),
                            &served.at(index)));
    std::ignore = rig.loop.drain();
    CHECK(bodyOf(rig, answers[0]).text == "ok");
    CHECK(bodyOf(rig, answers[1]).text == "ok");
    CHECK(client.idleConnections() == cap);
    // The surplus is closed, not merely forgotten: the second connection back is the one past the cap.
    CHECK(peerClosed(rig, servers[1]->connection().socket()));
}

TEST_CASE("HttpClient.anIdleConnectionExpiresOnTheClientsClock", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.idleTimeout = 1000ms;
                               }) };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/one"), server, OkReply, &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    rig.clock.advance(999ms);
    CHECK(client.idleConnections() == 1);
    SECTION("reused before its deadline")
    {
        auto second = Exchanged {};
        exchangeOn(rig, client, get("http://example.test/two"), server, OkReply, &second);
        CHECK(rig.connector.connects() == 1);
        CHECK(targetOf(second) == "/two");
    }
    SECTION("closed at its deadline")
    {
        rig.clock.advance(1ms);
        CHECK(client.idleConnections() == 0);
        auto second = Answer {};
        startSend(rig.loop, client, get("http://example.test/two"), &second);
        std::ignore = rig.loop.drain();
        CHECK(rig.connector.connects() == 2);
        CHECK(peerClosed(rig, server->connection().socket())); // swept by the send
        auto secondServer = acceptServer(rig);
        secondServer->connection().close();
        std::ignore = rig.loop.drain();
        CHECK(second.has_value());
    }
}

TEST_CASE("HttpClient.aConnectionIsReusedOnlyUnderItsOwnDeadlines", "[http][client][pool]")
{
    // A connection keeps the read deadlines it was opened with, so a request with others of its own
    // gets a connection of its own; at the connection cap the idle one makes room.
    auto const cap = GENERATE(std::size_t { 1 }, std::size_t { 16 });
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([cap](HttpClientOptions& o) {
                                   o.maxConnectionsPerOrigin = cap;
                               }) };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/one"), server, OkReply, &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    REQUIRE(client.idleConnections() == 1);

    auto request = get("http://example.test/two");
    request.timeouts = HttpTimeouts { .connect = 0ms, .firstByte = 100ms, .betweenBytes = 0ms };
    auto second = Answer {};
    startSend(rig.loop, client, std::move(request), &second);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    CHECK(client.idleConnections() == (cap == 1 ? 0U : 1U));
    if (cap == 1)
        CHECK(peerClosed(rig, server->connection().socket())); // closed to make room, not forgotten
    rig.clock.advance(100ms);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(second).code == HttpErrorCode::ResponseTimeout);
}

TEST_CASE("HttpClient.theConnectionCapRefusesOneMore", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxConnectionsPerOrigin = 2;
                               }) };
    auto answers = std::array<Answer, 5> {};
    startSend(rig.loop, client, get("http://example.test/a"), answers.data());
    startSend(rig.loop, client, get("http://example.test/b"), &answers[1]);
    startSend(rig.loop, client, get("http://example.test/c"), &answers[2]);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    CHECK(failureOf(answers[2]).code == HttpErrorCode::ConnectionLimitReached);

    // The cap is per origin.
    startSend(rig.loop, client, get("http://example.test:8080/d"), &answers[3]);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 3);

    // A request that failed gives its place back.
    auto failed = rig.connector.acceptNext();
    REQUIRE(failed != nullptr);
    failed->close();
    std::ignore = rig.loop.drain();
    CHECK(failureOf(answers[0]).code == HttpErrorCode::ConnectionTerminated);
    startSend(rig.loop, client, get("http://example.test/e"), &answers[4]);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 4);

    // End every parked send while `answers` is in scope.
    while (auto rest = rig.connector.acceptNext())
        rest->close();
    std::ignore = rig.loop.drain();
    for (auto const& answer: answers)
        CHECK(answer.has_value());
}

TEST_CASE("HttpClient.aRefusedDialGivesItsPlaceBack", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxConnectionsPerOrigin = 1;
                               }) };
    rig.connector.refuseNext(makeNetError(NetErrorCode::ConnRefused, 0, "scripted"));
    auto first = Answer {};
    startSend(rig.loop, client, get("http://example.test/a"), &first);
    std::ignore = rig.loop.drain();
    CHECK(failureOf(first).code == HttpErrorCode::ConnectionRefused);
    auto server = std::unique_ptr<ScriptedServer> {};
    auto second = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/b"), server, OkReply, &second);
    CHECK(rig.connector.connects() == 2);
    CHECK(bodyOf(rig, second.answer).text == "ok");
}

TEST_CASE("HttpClient.theClientDestroyedMidBodyLetsTheReadFinish", "[http][client][pool]")
{
    // The body reader holds its connection, and the pool only weakly: with the client gone the read
    // still finishes, and the connection is closed at its end instead of going back.
    auto rig = Rig {};
    auto client = std::make_unique<HttpClient>(rig.loop, rig.connector, nullptr);
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig,
               *client,
               get("http://example.test/one"),
               server,
               "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nab",
               &first);
    REQUIRE(first.answer.has_value());
    REQUIRE(first.answer->has_value());
    client.reset();
    auto body = std::optional<ReadBody> {};
    rig.loop.spawn(into(readAll((*first.answer)->body.get(), 1000), &body));
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(body.has_value());
    rig.loop.spawn(writeOnly(&server->connection().socket(), "cd"));
    std::ignore = rig.loop.drain();
    REQUIRE(body.has_value());
    CHECK(body->text == "abcd");
    CHECK_FALSE(body->error.has_value());
    CHECK(peerClosed(rig, server->connection().socket()));
}

namespace
{

/// What a stale-connection row sends as its body.
enum class RowBody : std::uint8_t
{
    None,              ///< No source at all.
    EmptyOnce,         ///< A source of known length zero that hands its one chunk over once.
    EmptyWithTrailers, ///< A source of known length zero with trailers: chunked, so not empty.
    Data,              ///< Four bytes.
};

/// A body of known length zero whose @c next may be called once, as @c IBodySource says: a second
/// call fails the body, so a retry that asked the spent source again would show.
class OneShotEmptySource final: public core::http::IBodySource
{
  public:
    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override { return 0; }
    [[nodiscard]] core::http::TrailerIntent trailerIntent() const noexcept override
    {
        return core::http::TrailerIntent::None;
    }
    [[nodiscard]] core::async::Task<HttpResult<core::http::BodyChunk>> next() override
    {
        return chunk(++_calls);
    }
    [[nodiscard]] core::async::Task<HttpResult<core::http::Fields>> trailers() override { return none(); }

  private:
    static core::async::Task<HttpResult<core::http::BodyChunk>> chunk(std::size_t call)
    {
        if (call > 1)
        {
            auto error = core::http::HttpError {};
            error.code = HttpErrorCode::InternalError;
            error.detail = "asked twice";
            co_return std::unexpected(std::move(error));
        }
        co_return core::http::BodyChunk { .bytes = {}, .end = core::http::BodyEnd::Reached };
    }

    static core::async::Task<HttpResult<core::http::Fields>> none() { co_return core::http::Fields {}; }

    std::size_t _calls = 0;
};

/// A request to retry, and how the server ends the pooled connection it is sent on.
struct StaleRow
{
    std::string_view name;
    std::string_view method;
    RowBody body;
};

/// How a server ends a kept-alive connection while it sits in the client's pool.
enum class ServerEnds : std::uint8_t
{
    Closes,     ///< In full: the client's write draws a reset, which its read reports as an OS error.
    HalfCloses, ///< Its write side only: the client's write goes through and its read sees the end.
};

/// Wraps @c net::ISocket::shutdownWrite as a task.
core::async::Task<void> halfClose(core::net::ISocket* socket)
{
    std::ignore = co_await socket->shutdownWrite();
}

/// Ends @p server's side of its connection as @p ends says.
void endServer(Rig& rig, ScriptedServer& server, ServerEnds ends)
{
    if (ends == ServerEnds::Closes)
        server.connection().close();
    else
        rig.loop.spawn(halfClose(&server.connection().socket()));
    std::ignore = rig.loop.drain();
}

/// Reads a request off @p socket, writes @p wire and closes: a server that starts an answer and dies.
core::async::Task<void> answerThenClose(core::net::ISocket* socket, std::string wire)
{
    std::ignore = co_await answerHeadOnly(socket, std::move(wire));
    socket->close();
}

/// Leaves @p server's pooled connection to the client after one exchange read to its end.
void poolOne(Rig& rig, HttpClient& client, std::unique_ptr<ScriptedServer>& server, Exchanged* first)
{
    exchangeOn(rig, client, get("http://example.test/one"), server, OkReply, first);
    REQUIRE(bodyOf(rig, first->answer).text == "ok");
    REQUIRE(client.idleConnections() == 1);
}

/// @return A request for `/two` by @p row.
OutgoingRequest requestOf(StaleRow const& row)
{
    auto request = get("http://example.test/two", row.method);
    switch (row.body)
    {
        case RowBody::None: break;
        case RowBody::EmptyOnce: request.body = std::make_unique<OneShotEmptySource>(); break;
        case RowBody::EmptyWithTrailers: {
            auto trailers = core::http::Fields {};
            REQUIRE(trailers.append("X-Sum", "0").has_value());
            request.body = std::make_unique<BufferBodySource>(std::string {}, std::move(trailers));
            break;
        }
        case RowBody::Data: request.body = std::make_unique<BufferBodySource>(std::string { "data" }); break;
    }
    return request;
}

} // namespace

TEST_CASE("HttpClient.aStaleReusedConnectionIsRetriedOnceForASafeRequest", "[http][client][pool]")
{
    // The server closed the kept-alive connection while it sat in the pool. A request whose method is
    // idempotent and whose body is empty is sent again, once, on a fresh connection.
    constexpr auto Rows = std::array {
        StaleRow { .name = "GET", .method = "GET", .body = RowBody::None },
        StaleRow { .name = "HEAD", .method = "HEAD", .body = RowBody::None },
        StaleRow { .name = "DELETE", .method = "DELETE", .body = RowBody::None },
        StaleRow { .name = "PUT, empty", .method = "PUT", .body = RowBody::EmptyOnce },
    };
    auto const ends = GENERATE(ServerEnds::Closes, ServerEnds::HalfCloses);
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name << (ends == ServerEnds::Closes ? ", closed" : ", half-closed"))
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto server = std::unique_ptr<ScriptedServer> {};
            auto first = Exchanged {};
            poolOne(rig, client, server, &first);
            endServer(rig, *server, ends);

            auto fresh = std::unique_ptr<ScriptedServer> {};
            auto second = Exchanged {};
            exchangeOn(rig,
                       client,
                       requestOf(row),
                       fresh,
                       row.method == "HEAD" ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n" : OkReply,
                       &second);
            CHECK(rig.connector.connects() == 2);
            CHECK(targetOf(second) == "/two");
            REQUIRE(second.answer.has_value());
            REQUIRE(second.answer->has_value());
            CHECK((*second.answer)->head.status == 200);
            if (row.body == RowBody::EmptyOnce)
                CHECK(fieldValues(*second.served, "Content-Length") == std::vector<std::string> { "0" });
        }
    }
}

TEST_CASE("HttpClient.aStaleReusedConnectionIsNotRetriedForAnUnsafeRequest", "[http][client][pool]")
{
    // A method that is not idempotent, or a body that was handed over and cannot be handed over again,
    // fails with the stale connection's error, and nothing is dialled. The body is the only
    // difference from the retried rows, so the PUT and GET rows hold the body rule alone.
    constexpr auto Rows = std::array {
        StaleRow { .name = "POST", .method = "POST", .body = RowBody::None },
        StaleRow { .name = "PATCH", .method = "PATCH", .body = RowBody::None },
        StaleRow { .name = "PUT with a body", .method = "PUT", .body = RowBody::Data },
        StaleRow { .name = "GET with a body", .method = "GET", .body = RowBody::Data },
        StaleRow { .name = "PUT, empty with trailers", .method = "PUT", .body = RowBody::EmptyWithTrailers },
    };
    auto const ends = GENERATE(ServerEnds::Closes, ServerEnds::HalfCloses);
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name << (ends == ServerEnds::Closes ? ", closed" : ", half-closed"))
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto server = std::unique_ptr<ScriptedServer> {};
            auto first = Exchanged {};
            poolOne(rig, client, server, &first);
            endServer(rig, *server, ends);
            auto second = Answer {};
            startSend(rig.loop, client, requestOf(row), &second);
            std::ignore = rig.loop.drain();
            CHECK(failureOf(second).code
                  == (ends == ServerEnds::Closes ? HttpErrorCode::InternalError
                                                 : HttpErrorCode::ConnectionTerminated));
            CHECK(rig.connector.connects() == 1);
            // A half-closed server still reads: end its side while `second` is in scope.
            server->connection().close();
            std::ignore = rig.loop.drain();
        }
    }
}

TEST_CASE("HttpClient.aReusedConnectionThatAnsweredIsNotRetried", "[http][client][pool]")
{
    // A connection that closes cleanly after part of the first head, or after an interim head, is not
    // stale: the server has seen the request. (A reset inside the first head is retried, as the class
    // documents; HttpClient.aResetInsideTheFirstHeadIsRetried pins that.)
    struct Row
    {
        std::string_view name;
        std::string_view wire;
        HttpErrorCode code;
    };
    constexpr auto Rows = std::array {
        Row { .name = "inside the head", .wire = "HTTP/1.1 2", .code = HttpErrorCode::ResponseIncomplete },
        Row { .name = "after an interim response",
              .wire = "HTTP/1.1 100 Continue\r\n\r\n",
              .code = HttpErrorCode::ConnectionTerminated },
    };
    for (auto const& row: Rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto rig = Rig {};
            auto client = HttpClient { rig.loop, rig.connector, nullptr };
            auto server = std::unique_ptr<ScriptedServer> {};
            auto first = Exchanged {};
            poolOne(rig, client, server, &first);
            auto second = Answer {};
            startSend(rig.loop, client, get("http://example.test/two"), &second);
            std::ignore = rig.loop.drain();
            rig.loop.spawn(answerThenClose(&server->connection().socket(), std::string { row.wire }));
            std::ignore = rig.loop.drain();
            CHECK(failureOf(second).code == row.code);
            CHECK(rig.connector.connects() == 1);
        }
    }
}

TEST_CASE("HttpClient.aFreshConnectionsFailureIsFinal", "[http][client][pool]")
{
    // The retry is the stale connection's only: a request that fails on a connection dialled for it,
    // the retry's own included, is answered with that failure.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    SECTION("a first request")
    {
        auto answer = Answer {};
        startSend(rig.loop, client, get("http://example.test/one"), &answer);
        std::ignore = rig.loop.drain();
        auto server = rig.connector.acceptNext();
        REQUIRE(server != nullptr);
        server->close();
        std::ignore = rig.loop.drain();
        CHECK(failureOf(answer).code == HttpErrorCode::ConnectionTerminated);
        CHECK(rig.connector.connects() == 1);
    }
    SECTION("the retry")
    {
        auto server = std::unique_ptr<ScriptedServer> {};
        auto first = Exchanged {};
        poolOne(rig, client, server, &first);
        endServer(rig, *server, ServerEnds::Closes);
        auto second = Answer {};
        startSend(rig.loop, client, get("http://example.test/two"), &second);
        std::ignore = rig.loop.drain();
        REQUIRE(rig.connector.connects() == 2);
        auto fresh = rig.connector.acceptNext();
        REQUIRE(fresh != nullptr);
        fresh->close();
        std::ignore = rig.loop.drain();
        CHECK(failureOf(second).code == HttpErrorCode::ConnectionTerminated);
        CHECK(rig.connector.connects() == 2);
        // A third dial would be parked on a server end nobody closes: end it while `second` is in scope.
        while (auto rest = rig.connector.acceptNext())
            rest->close();
        std::ignore = rig.loop.drain();
    }
}

TEST_CASE("HttpClient.aFailedBodyGivesItsPlaceBack", "[http][client][pool]")
{
    // A body that fails is over: its connection is closed at the failure, not when the reader is
    // dropped, so it holds no place under the cap.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxConnectionsPerOrigin = 1;
                               }) };
    auto first = Answer {};
    startSend(rig.loop, client, get("http://example.test/one"), &first);
    std::ignore = rig.loop.drain();
    auto server = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(
        into(server->answer("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nab", AfterReply::Closes), &served));
    std::ignore = rig.loop.drain();
    auto const body = bodyOf(rig, first);
    REQUIRE(body.error.has_value());
    CHECK(body.error->code == HttpErrorCode::ResponseIncomplete);

    auto second = Answer {};
    startSend(rig.loop, client, get("http://example.test/two"), &second);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    while (auto rest = rig.connector.acceptNext())
        rest->close();
    std::ignore = rig.loop.drain();
    CHECK(second.has_value());
}

TEST_CASE("HttpClient.aResetInsideTheFirstHeadIsRetried", "[http][client][pool]")
{
    // The documented approximation: a reset reaches the client as ConnectionTerminated whether or not
    // part of the head came before it, so a pooled connection reset inside its first head is stale
    // like one reset before it. The request is idempotent and bodiless, so the retry is safe.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    poolOne(rig, client, server, &first);
    auto second = Answer {};
    startSend(rig.loop, client, get("http://example.test/two"), &second);
    std::ignore = rig.loop.drain();
    // Part of a head, read by the client, then a close over the request the server never read: a reset.
    rig.loop.spawn(writeOnly(&server->connection().socket(), "HTTP/1.1 2"));
    std::ignore = rig.loop.drain();
    REQUIRE_FALSE(second.has_value());
    server->connection().close();
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    auto fresh = acceptServer(rig);
    auto served = std::optional<ServedRequest> {};
    rig.loop.spawn(into(fresh->answer(std::string { OkReply }, AfterReply::StaysOpen), &served));
    std::ignore = rig.loop.drain();
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->head.status == 200);
}

TEST_CASE("HttpClient.anExpiredIdleConnectionHoldsNoRoomUnderTheIdleCap", "[http][client][pool]")
{
    // With room for one idle connection, a connection coming back finds that place taken by one past
    // its deadline: that one is closed and the newcomer kept, rather than the other way round.
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxIdlePerOrigin = 1;
                                   o.idleTimeout = 1000ms;
                               }) };
    auto answers = std::array<Answer, 2> {};
    startSend(rig.loop, client, get("http://example.test/a"), answers.data());
    startSend(rig.loop, client, get("http://example.test/b"), &answers[1]);
    std::ignore = rig.loop.drain();
    REQUIRE(rig.connector.connects() == 2);
    auto servers = std::array { acceptServer(rig), acceptServer(rig) };
    auto served = std::array<std::optional<ServedRequest>, 2> {};
    for (auto const index: { std::size_t { 0 }, std::size_t { 1 } })
        rig.loop.spawn(into(servers.at(index)->answer(std::string { OkReply }, AfterReply::StaysOpen),
                            &served.at(index)));
    std::ignore = rig.loop.drain();
    CHECK(bodyOf(rig, answers[0]).text == "ok");
    REQUIRE(client.idleConnections() == 1);
    rig.clock.advance(1000ms);
    CHECK(client.idleConnections() == 0);
    CHECK(bodyOf(rig, answers[1]).text == "ok");
    CHECK(client.idleConnections() == 1);
    CHECK(peerClosed(rig, servers[0]->connection().socket()));
    CHECK_FALSE(peerClosed(rig, servers[1]->connection().socket()));
}

TEST_CASE("HttpClient.theTotalCapEvictsTheOldestIdleConnectionOfAnyOrigin", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxConnectionsTotal = 2;
                               }) };
    auto first = std::unique_ptr<ScriptedServer> {};
    auto a = Exchanged {};
    exchangeOn(rig, client, get("http://a.test/"), first, OkReply, &a);
    CHECK(bodyOf(rig, a.answer).text == "ok");
    rig.clock.advance(1ms);
    auto second = std::unique_ptr<ScriptedServer> {};
    auto b = Exchanged {};
    exchangeOn(rig, client, get("http://b.test/"), second, OkReply, &b);
    CHECK(bodyOf(rig, b.answer).text == "ok");
    REQUIRE(client.idleConnections() == 2);

    auto third = std::unique_ptr<ScriptedServer> {};
    auto c = Exchanged {};
    exchangeOn(rig, client, get("http://c.test/"), third, OkReply, &c);
    CHECK(rig.connector.connects() == 3);
    CHECK(targetOf(c) == "/");
    CHECK(client.idleConnections() == 1);
    CHECK(peerClosed(rig, first->connection().socket()));        // the oldest idle made room
    CHECK_FALSE(peerClosed(rig, second->connection().socket())); // the newer one stays
}

TEST_CASE("HttpClient.theTotalCapRefusesWhenNoConnectionIsIdle", "[http][client][pool]")
{
    auto rig = Rig {};
    auto client = HttpClient { rig.loop, rig.connector, nullptr, optionsWhere([](HttpClientOptions& o) {
                                   o.maxConnectionsTotal = 2;
                               }) };
    auto answers = std::array<Answer, 4> {};
    startSend(rig.loop, client, get("http://a.test/"), answers.data());
    startSend(rig.loop, client, get("http://b.test/"), &answers[1]);
    startSend(rig.loop, client, get("http://c.test/"), &answers[2]);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 2);
    CHECK(failureOf(answers[2]).code == HttpErrorCode::ConnectionLimitReached);
    // A connection given up frees room under the total too.
    auto gone = rig.connector.acceptNext();
    REQUIRE(gone != nullptr);
    gone->close();
    std::ignore = rig.loop.drain();
    startSend(rig.loop, client, get("http://c.test/"), &answers[3]);
    std::ignore = rig.loop.drain();
    CHECK(rig.connector.connects() == 3);
    while (auto rest = rig.connector.acceptNext())
        rest->close();
    std::ignore = rig.loop.drain();
    for (auto const& answer: answers)
        CHECK(answer.has_value());
}

TEST_CASE("HttpClient.aStalePooledHttpsConnectionIsRetriedOverAFreshHandshake", "[http][client][pool]")
{
    // Over the scripted TLS context (R29: the http binaries link no net_tls): the retry dials, wraps
    // for the same host and handshakes again; the pool never hands an https connection to http.
    auto rig = Rig {};
    auto tls = ScriptedTlsContext { rig.loop, scriptOf(HandshakeOutcome::Succeeds) };
    auto client = HttpClient { rig.loop, rig.connector, &tls };
    auto server = std::unique_ptr<ScriptedServer> {};
    auto first = Exchanged {};
    exchangeOn(rig, client, get("https://example.test/one"), server, OkReply, &first);
    CHECK(bodyOf(rig, first.answer).text == "ok");
    REQUIRE(client.idleConnections() == 1);
    REQUIRE(tls.record().handshakes == 1);
    endServer(rig, *server, ServerEnds::Closes);

    auto fresh = std::unique_ptr<ScriptedServer> {};
    auto second = Exchanged {};
    exchangeOn(rig, client, get("https://example.test/two"), fresh, OkReply, &second);
    CHECK(rig.connector.connects() == 2);
    CHECK(tls.record().wraps == 2);
    CHECK(tls.record().handshakes == 2);
    CHECK(tls.record().serverNames == std::vector<std::string> { "example.test", "example.test" });
    CHECK(targetOf(second) == "/two");
    CHECK(bodyOf(rig, second.answer).text == "ok");

    // The same host in the clear is another origin: it dials rather than reuse the TLS connection.
    auto plain = std::unique_ptr<ScriptedServer> {};
    auto third = Exchanged {};
    exchangeOn(rig, client, get("http://example.test/three"), plain, OkReply, &third);
    CHECK(rig.connector.connects() == 3);
    CHECK(tls.record().wraps == 2);
}
