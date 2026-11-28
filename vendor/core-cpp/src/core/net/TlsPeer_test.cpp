// SPDX-License-Identifier: Apache-2.0
//
// The per-connection TLS peer, and that it fails closed.
//
// `ITlsContext::wrapClient` binds one connection to the name it was dialled for: SNI for a DNS
// name, the certificate check for a DNS name or an IP literal. Every way that check can fail is a
// failed handshake that `ITlsSession::failure()` names as a certificate failure -- an expired
// certificate, a wrong name, an untrusted root, an address the certificate does not carry -- and no
// option turns it off. The other end is `testing::StrictTlsPeer`, OpenSSL driven by hand, so what
// the server saw (the SNI, the ALPN offer) is reported by something that is not the code under
// test.
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Tls.hpp>
#include <core/net/detail/Alpn.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/net/testing/StrictTlsPeer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "TlsTestSupport.hpp"

using core::net::CertKeyPem;
using core::net::ISocket;
using core::net::ITlsContext;
using core::net::NetErrorCode;
using core::net::TlsClientOptions;
using core::net::TlsFailureKind;
using core::net::TlsPeer;
using core::net::testing::handshakeBoth;
using core::net::testing::makeTlsLoopback;
using core::net::testing::StrictTlsPeer;

namespace
{

/// The certificate every case presents unless it says otherwise: a DNS name and an IP address.
CertKeyPem const& localhostMaterial()
{
    static auto const material = [] {
        auto generated = core::net::generateSelfSignedCertificate(
            { .commonName = "localhost", .subjectNames = { "localhost", "127.0.0.1" } });
        REQUIRE(generated.has_value());
        return *generated;
    }();
    return material;
}

/// A client context that trusts exactly the certificate in @p pem.
std::shared_ptr<ITlsContext> clientTrusting(std::string_view pem, TlsClientOptions const& options = {})
{
    auto anchors = core::net::trustAnchorsFromPem(pem);
    REQUIRE(anchors.has_value());
    auto context = core::net::makeTlsClientContext(*anchors, options);
    REQUIRE(context.has_value());
    return std::move(*context);
}

/// Wraps @p transport for @p serverName through @p context, which must accept it.
std::unique_ptr<ISocket> dial(ITlsContext& context,
                              std::unique_ptr<ISocket> transport,
                              core::async::IExecutor& executor,
                              std::string serverName)
{
    auto socket =
        context.wrapClient(std::move(transport), executor, TlsPeer { .serverName = std::move(serverName) });
    REQUIRE(socket.has_value());
    REQUIRE(*socket != nullptr);
    return std::move(*socket);
}

/// An `ITlsContext` that implements only `wrap`, as a context written before `wrapClient` existed
/// does: it hands the transport back and counts how often it was asked.
/// What a `WrapOnlyContext` answers.
enum class WrapAnswer : std::uint8_t
{
    Inner = 0, ///< The transport, unwrapped.
    Null,      ///< Nothing, as a wrap that could not allocate.
};

class WrapOnlyContext final: public ITlsContext
{
  public:
    explicit WrapOnlyContext(WrapAnswer answer = WrapAnswer::Inner): _answer { answer } {}

    [[nodiscard]] std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner,
                                                core::async::IExecutor& /*executor*/) override
    {
        ++wraps;
        if (_answer == WrapAnswer::Null)
            return nullptr;
        return inner;
    }

    int wraps = 0;

  private:
    WrapAnswer _answer;
};

/// The session of a socket the case made TLS; a socket without one fails the case.
/// @param socket The socket a TLS context wrapped.
/// @return Its session.
[[nodiscard]] core::net::ITlsSession const& sessionOf(ISocket const& socket)
{
    auto const* const session = socket.tlsSession();
    REQUIRE(session != nullptr);
    return *session;
}

} // namespace

TEST_CASE("the default wrapClient refuses a server name", "[net][tls][tlspeer]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::client());

    SECTION("a name is refused, and the transport is never wrapped without it")
    {
        auto context = WrapOnlyContext {};
        auto const socket =
            context.wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = "x" });
        REQUIRE_FALSE(socket.has_value());
        CHECK(socket.error().code == NetErrorCode::Unsupported);
        CHECK(context.wraps == 0);
    }

    SECTION("no name is wrap()")
    {
        auto context = WrapOnlyContext {};
        auto const* const raw = loopback->transport.get();
        auto const socket = context.wrapClient(std::move(loopback->transport), loopback->loop, {});
        REQUIRE(socket.has_value());
        CHECK(socket->get() == raw);
        CHECK(context.wraps == 1);
    }

    SECTION("a wrap that could not allocate is an error, not a null socket")
    {
        auto context = WrapOnlyContext { WrapAnswer::Null };
        auto const socket = context.wrapClient(std::move(loopback->transport), loopback->loop, {});
        REQUIRE_FALSE(socket.has_value());
        CHECK(socket.error().code == NetErrorCode::ResourceExhausted);
    }
}

TEST_CASE("contexts from the pinned-name factory refuse a per-connection name", "[net][tls][tlspeer]")
{
    // The legacy factory binds its name at construction (or verifies nothing, for trust on first
    // use). A per-connection name it silently dropped would be a connection that is not checked for
    // the host it was dialled for, so it is refused instead.
    auto loopback = makeTlsLoopback(StrictTlsPeer::client());
    auto const pinned = core::net::makeTlsClientContext(localhostMaterial().certPem, "localhost");
    auto const trusting = core::net::makeTlsClientContext();
    REQUIRE(pinned.has_value());
    REQUIRE(trusting.has_value());

    for (auto* const context: { pinned->get(), trusting->get() })
    {
        auto pair = core::net::testing::makeSocketPair(loopback->loop);
        REQUIRE(pair.has_value());
        auto const named =
            context->wrapClient(std::move(pair->first), loopback->loop, { .serverName = "localhost" });
        REQUIRE_FALSE(named.has_value());
        CHECK(named.error().code == NetErrorCode::Unsupported);

        auto const unnamed = context->wrapClient(std::move(pair->second), loopback->loop, {});
        REQUIRE(unnamed.has_value());
        CHECK(*unnamed != nullptr);
    }
}

TEST_CASE("a DNS name is sent as SNI and verified", "[net][tls][tlspeer]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const context = clientTrusting(localhostMaterial().certPem);
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    CHECK(outcome.local.has_value());
    CHECK(outcome.peer.has_value());
    CHECK(loopback->peer->receivedServerName() == "localhost");

    auto const* const session = socket->tlsSession();
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->failure().has_value());
    CHECK(session->negotiatedProtocol().empty()); // nothing was offered
}

TEST_CASE("a verifying context refuses an empty server name", "[net][tls][tlspeer]")
{
    // With no name, a chain the anchors vouch for would be accepted whoever it was issued to.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const context = clientTrusting(localhostMaterial().certPem);
    auto const refused = context->wrapClient(std::move(loopback->transport), loopback->loop, TlsPeer {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == NetErrorCode::AddressError);
}

TEST_CASE("Tls.ipLiteralAuthority", "[net][tls][tlspeer]")
{
    // An https authority that is an IP literal is checked against the certificate's IP
    // subjectAltNames (X509_VERIFY_PARAM_set1_ip_asc), never as a DNS name, and sends no SNI: RFC
    // 6066 section 3 forbids a literal address in server_name.
    SECTION("127.0.0.1 sends no SNI and verifies against the IP SAN")
    {
        auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
        auto const context = clientTrusting(localhostMaterial().certPem);
        auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "127.0.0.1");

        auto const outcome = handshakeBoth(*loopback, *socket);
        REQUIRE(outcome.finished);
        CHECK(outcome.local.has_value());
        CHECK(outcome.peer.has_value());
        CHECK(loopback->peer->receivedServerName().empty());
    }

    SECTION("a bracketed IPv6 literal, as a URL authority spells it, is an address too")
    {
        auto const material =
            core::net::generateSelfSignedCertificate({ .commonName = "v6", .subjectNames = { "::1" } });
        REQUIRE(material.has_value());
        auto loopback = makeTlsLoopback(StrictTlsPeer::server(*material));
        auto const context = clientTrusting(material->certPem);
        auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "[::1]");

        auto const outcome = handshakeBoth(*loopback, *socket);
        REQUIRE(outcome.finished);
        CHECK(outcome.local.has_value());
        CHECK(loopback->peer->receivedServerName().empty());
    }

    SECTION("a zone is local routing: the address without it is verified, and no SNI is sent")
    {
        auto const material =
            core::net::generateSelfSignedCertificate({ .commonName = "link", .subjectNames = { "fe80::1" } });
        REQUIRE(material.has_value());
        for (auto const* const name: { "fe80::1%eth0", "[fe80::1%eth0]" })
        {
            auto loopback = makeTlsLoopback(StrictTlsPeer::server(*material));
            auto const context = clientTrusting(material->certPem);
            auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, name);

            auto const outcome = handshakeBoth(*loopback, *socket);
            REQUIRE(outcome.finished);
            CHECK(outcome.local.has_value());
            CHECK(loopback->peer->receivedServerName().empty());
        }
    }

    SECTION("brackets or a zone around anything but IPv6 are refused before anything is sent")
    {
        for (auto const* const name:
             { "[localhost]", "[127.0.0.1]", "127.0.0.1%eth0", "localhost%eth0", "[]" })
        {
            auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
            auto const context = clientTrusting(localhostMaterial().certPem);
            auto const socket =
                context->wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = name });
            CAPTURE(name);
            REQUIRE_FALSE(socket.has_value());
            CHECK(socket.error().code == NetErrorCode::AddressError);
        }
    }
}

TEST_CASE("a whole-label wildcard matches the one label it stands for", "[net][tls][tlspeer]")
{
    auto const material = core::net::generateSelfSignedCertificate(
        { .commonName = "wildcard", .subjectNames = { "*.example.test" } });
    REQUIRE(material.has_value());
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(*material));
    auto const context = clientTrusting(material->certPem);
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "www.example.test");

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    CHECK(outcome.local.has_value());
    CHECK(loopback->peer->receivedServerName() == "www.example.test");
}

TEST_CASE("the pinned-name factory never accepts a name from the common name", "[net][tls][tlspeer]")
{
    // RFC 9525 section 6.3: a client checks the subjectAltNames only. This certificate names
    // "localhost" in its subject and carries only an IP entry, which OpenSSL's default would let
    // the common name stand in for.
    auto const material = core::net::generateSelfSignedCertificate(
        { .commonName = "localhost", .subjectNames = { "127.0.0.1" } });
    REQUIRE(material.has_value());
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(*material));
    auto const pinned = core::net::makeTlsClientContext(material->certPem, "localhost");
    REQUIRE(pinned.has_value());
    auto const socket = (*pinned)->wrap(std::move(loopback->transport), loopback->loop);
    REQUIRE(socket != nullptr);

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    CHECK_FALSE(outcome.local.has_value());
    auto const failure = sessionOf(*socket).failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::Certificate);
}

TEST_CASE("every verification failure fails the handshake as a certificate failure", "[net][tls][tlspeer]")
{
    auto const& material = localhostMaterial();
    auto const expired = core::net::testing::expiredCopyOf(material);
    REQUIRE(expired.has_value());
    auto const stranger = core::net::generateSelfSignedCertificate({ .commonName = "localhost" });
    REQUIRE(stranger.has_value());
    auto const partial = core::net::generateSelfSignedCertificate(
        { .commonName = "partial", .subjectNames = { "w*.example.test" } });
    REQUIRE(partial.has_value());
    auto const commonNameOnly = core::net::generateSelfSignedCertificate(
        { .commonName = "localhost", .subjectNames = { "127.0.0.1" } });
    REQUIRE(commonNameOnly.has_value());

    struct Row
    {
        std::string_view name;
        CertKeyPem const* presented; ///< What the server serves.
        std::string_view trusted;    ///< What the client trusts.
        std::string_view serverName; ///< What the client dialled.
        std::string_view because;    ///< A fragment of OpenSSL's verification reason.
    };
    auto const rows = std::array {
        Row { "expired", &*expired, expired->certPem, "localhost", "expired" },
        Row { "wrong name", &material, material.certPem, "other.test", "hostname mismatch" },
        Row { "untrusted root", &material, stranger->certPem, "localhost", "self" },
        Row { "IP mismatch", &material, material.certPem, "127.0.0.2", "IP address mismatch" },
        // OpenSSL's default would match a wildcard inside a label; an https client must not.
        Row { "partial wildcard", &*partial, partial->certPem, "www.example.test", "hostname mismatch" },
        // RFC 9525: the subject's common name never stands in for a DNS subjectAltName.
        Row {
            "common name only", &*commonNameOnly, commonNameOnly->certPem, "localhost", "hostname mismatch" },
        // `127.1` is shorthand getaddrinfo accepts, and no literal by the strict parse the dial path
        // uses: a name, which no DNS entry matches.
        Row { "shorthand address", &material, material.certPem, "127.1", "hostname mismatch" },
    };

    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto loopback = makeTlsLoopback(StrictTlsPeer::server(*row.presented));
            auto const context = clientTrusting(row.trusted);
            auto const socket = dial(
                *context, std::move(loopback->transport), loopback->loop, std::string { row.serverName });

            auto const outcome = handshakeBoth(*loopback, *socket);
            REQUIRE(outcome.finished);
            CHECK_FALSE(outcome.local.has_value());
            CHECK_FALSE(outcome.peer.has_value());

            auto const* const session = socket->tlsSession();
            REQUIRE(session != nullptr);
            auto const failure = session->failure();
            REQUIRE(failure.has_value());
            CHECK(failure->kind == TlsFailureKind::Certificate);
            CHECK_FALSE(failure->alert.has_value());
            CHECK(failure->message.contains(row.because));
        }
    }
}

TEST_CASE("a server's alert surfaces as AlertReceived with its id", "[net][tls][tlspeer]")
{
    constexpr auto AccessDenied = std::uint8_t { 49 };
    auto loopback =
        makeTlsLoopback(StrictTlsPeer::server(localhostMaterial(), { .refuseWithAlert = AccessDenied }));
    auto const context = clientTrusting(localhostMaterial().certPem);
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    CHECK_FALSE(outcome.local.has_value());

    auto const failure = sessionOf(*socket).failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::AlertReceived);
    CHECK(failure->alert == AccessDenied);
    CHECK_FALSE(failure->message.empty());
}

TEST_CASE("a record-layer failure after a handshake that verified nothing is a protocol failure",
          "[net][tls][tlspeer]")
{
    // Trust on first use completes the handshake with OpenSSL's verification result still saying the
    // chain was not trusted. What breaks the connection afterwards is the record layer, and calling
    // it a certificate failure would blame what nothing checked.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const trusting = core::net::makeTlsClientContext();
    REQUIRE(trusting.has_value());
    auto const socket = (*trusting)->wrap(std::move(loopback->transport), loopback->loop);
    REQUIRE(socket != nullptr);
    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    REQUIRE(outcome.local.has_value());

    // An application-data record header and five bytes no key encrypted.
    static constexpr auto Garbage =
        std::array<std::uint8_t, 10> { 0x17, 0x03, 0x03, 0x00, 0x05, 'n', 'o', 'p', 'e', '!' };
    auto failed = false;

    SECTION("read")
    {
        auto const read = [](ISocket* tls, ISocket* wire, bool* out) -> core::async::Task<void> {
            std::ignore = co_await wire->write(std::as_bytes(std::span { Garbage }));
            auto buffer = std::array<std::byte, 64> {};
            *out = !(co_await tls->read(buffer)).has_value();
        };
        REQUIRE(core::net::testing::blockOnWithin(loopback->loop,
                                                  read(socket.get(), loopback->wire.get(), &failed),
                                                  core::net::testing::TlsExchangeBound));
    }

    SECTION("waitReadable")
    {
        auto const watch = [](ISocket* tls, ISocket* wire, bool* out) -> core::async::Task<void> {
            std::ignore = co_await wire->write(std::as_bytes(std::span { Garbage }));
            *out = !(co_await tls->waitReadable()).has_value();
        };
        REQUIRE(core::net::testing::blockOnWithin(loopback->loop,
                                                  watch(socket.get(), loopback->wire.get(), &failed),
                                                  core::net::testing::TlsExchangeBound));
    }

    CHECK(failed);
    auto const failure = sessionOf(*socket).failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::Protocol);
    CHECK_FALSE(failure->message.empty());
}

TEST_CASE("a name the wire cannot carry is refused before anything is sent", "[net][tls][tlspeer]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const context = clientTrusting(localhostMaterial().certPem);

    SECTION("an embedded NUL")
    {
        // It would cut the name short in SNI and in the check: a certificate for "127.0.0.1"
        // accepted for a host that was asked for as something else.
        auto const socket = context->wrapClient(std::move(loopback->transport),
                                                loopback->loop,
                                                { .serverName = std::string { "127.0.0.1\0.evil", 15 } });
        REQUIRE_FALSE(socket.has_value());
        CHECK(socket.error().code == NetErrorCode::AddressError);
    }

    SECTION("a name longer than SNI's 255 bytes")
    {
        auto const socket = context->wrapClient(std::move(loopback->transport),
                                                loopback->loop,
                                                { .serverName = std::string(300, 'a') + ".test" });
        REQUIRE_FALSE(socket.has_value());
        CHECK(socket.error().code == NetErrorCode::AddressError);
    }
}

TEST_CASE("a client offers ALPN and reports what the server selected", "[net][tls][tlspeer][alpn]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial(), { .alpn = { "http/1.1" } }));
    auto const context = clientTrusting(localhostMaterial().certPem, { .alpn = { "h2", "http/1.1" } });
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    REQUIRE(outcome.local.has_value());
    CHECK(loopback->peer->negotiatedProtocol() == "http/1.1");
    REQUIRE(socket->tlsSession() != nullptr);
    CHECK(sessionOf(*socket).negotiatedProtocol() == "http/1.1");
}

TEST_CASE("a pinned-name client context offers the ALPN names it was built with", "[net][tls][tlspeer][alpn]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial(), { .alpn = { "http/1.1" } }));
    auto const context =
        core::net::makeTlsClientContext(localhostMaterial().certPem, "localhost", { .alpn = { "http/1.1" } });
    REQUIRE(context.has_value());
    auto const socket = (*context)->wrap(std::move(loopback->transport), loopback->loop);
    REQUIRE(socket != nullptr);

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    REQUIRE(outcome.local.has_value());
    CHECK(sessionOf(*socket).negotiatedProtocol() == "http/1.1");
}

TEST_CASE("a client context refuses an ALPN name the wire cannot carry", "[net][tls][tlspeer][alpn]")
{
    auto anchors = core::net::trustAnchorsFromPem(localhostMaterial().certPem);
    REQUIRE(anchors.has_value());
    CHECK_FALSE(core::net::makeTlsClientContext(*anchors, { .alpn = { "" } }).has_value());
    CHECK_FALSE(core::net::makeTlsClientContext(*anchors, { .alpn = { std::string(256, 'a') } }).has_value());
    CHECK(core::net::makeTlsClientContext(*anchors, { .alpn = { std::string(255, 'a') } }).has_value());

    auto const& material = localhostMaterial();
    CHECK_FALSE(core::net::makeTlsClientContext(material.certPem, "", { .alpn = { "" } }).has_value());
    CHECK_FALSE(
        core::net::makeTlsServerContext(material.certPem, material.keyPem, { .alpn = { "" } }).has_value());
    CHECK_FALSE(core::net::makeTlsServerContext(
                    material.certPem, material.keyPem, { .alpn = { std::string(256, 'a') } })
                    .has_value());
}

TEST_CASE("a client context needs a trust anchor", "[net][tls][tlspeer]")
{
    // No anchor trusts nothing; a context that accepted it would either refuse every server or --
    // were "nothing configured" ever read as "verify nothing" -- accept every one.
    CHECK_FALSE(core::net::makeTlsClientContext(core::net::TrustAnchors {}).has_value());
    CHECK_FALSE(core::net::trustAnchorsFromPem("").has_value());
    CHECK_FALSE(
        core::net::trustAnchorsFromPem("-----BEGIN CERTIFICATE-----\nnope\n-----END CERTIFICATE-----\n")
            .has_value());

    auto const two =
        core::net::trustAnchorsFromPem(localhostMaterial().certPem + localhostMaterial().certPem);
    REQUIRE(two.has_value());
    CHECK(two->certificates.size() == 2);
    // A block that is there and does not parse is an error, not the end of the bundle.
    CHECK_FALSE(
        core::net::trustAnchorsFromPem(localhostMaterial().certPem
                                       + "-----BEGIN CERTIFICATE-----\nnope\n-----END CERTIFICATE-----\n")
            .has_value());

    // An anchor is one whole DER certificate: a byte after it is not part of any.
    auto trailing = *two;
    trailing.certificates.resize(1);
    CHECK(core::net::makeTlsClientContext(trailing).has_value());
    trailing.certificates.front().push_back(std::byte { 0 });
    CHECK_FALSE(core::net::makeTlsClientContext(trailing).has_value());
}

TEST_CASE("a server context selects ALPN in its own order of preference", "[net][tls][tlspeer][alpn]")
{
    auto const server = core::net::makeTlsServerContext(
        localhostMaterial().certPem, localhostMaterial().keyPem, { .alpn = { "http/1.1", "h2" } });
    REQUIRE(server.has_value());

    SECTION("the first of its own names the client offered")
    {
        auto loopback = makeTlsLoopback(StrictTlsPeer::client({ .alpn = { "h2", "http/1.1" } }));
        auto const socket = (*server)->wrap(std::move(loopback->transport), loopback->loop);
        REQUIRE(socket != nullptr);

        auto const outcome = handshakeBoth(*loopback, *socket);
        REQUIRE(outcome.finished);
        REQUIRE(outcome.local.has_value());
        CHECK(loopback->peer->negotiatedProtocol() == "http/1.1");
        CHECK(sessionOf(*socket).negotiatedProtocol() == "http/1.1");
    }

    SECTION("no name in common is refused with no_application_protocol (RFC 7301)")
    {
        auto loopback = makeTlsLoopback(StrictTlsPeer::client({ .alpn = { "spdy/3" } }));
        auto const socket = (*server)->wrap(std::move(loopback->transport), loopback->loop);
        REQUIRE(socket != nullptr);

        auto const outcome = handshakeBoth(*loopback, *socket);
        REQUIRE(outcome.finished);
        CHECK_FALSE(outcome.local.has_value());
        REQUIRE_FALSE(outcome.peer.has_value());
        // The alert, by its words or, where OpenSSL has none for it (3.0), by its reason code:
        // SSL_AD_REASON_OFFSET (1000) plus no_application_protocol (120).
        INFO("the peer's error: " << outcome.peer.error());
        CHECK((outcome.peer.error().contains("no application protocol")
               || outcome.peer.error().contains("reason(1120)")));
    }

    SECTION("a client that offers nothing is served without ALPN")
    {
        auto loopback = makeTlsLoopback(StrictTlsPeer::client());
        auto const socket = (*server)->wrap(std::move(loopback->transport), loopback->loop);
        REQUIRE(socket != nullptr);

        auto const outcome = handshakeBoth(*loopback, *socket);
        REQUIRE(outcome.finished);
        CHECK(outcome.local.has_value());
        CHECK(sessionOf(*socket).negotiatedProtocol().empty());
    }
}

TEST_CASE("a decorator forwards the TLS session, and tlsSessionOf sees through it", "[net][tls][tlspeer]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const context = clientTrusting(localhostMaterial().certPem);
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");
    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    REQUIRE(outcome.local.has_value());

    auto const decorated = core::net::testing::SocketDecorator { *socket };
    REQUIRE(socket->tlsSession() != nullptr);
    CHECK(decorated.tlsSession() == socket->tlsSession());
    CHECK(core::net::tlsSessionOf(decorated) == socket->tlsSession());
    CHECK(core::net::tlsSessionOf(*socket) == socket->tlsSession());
    CHECK(loopback->wire->tlsSession() == nullptr);
    CHECK(core::net::tlsSessionOf(*loopback->wire) == nullptr);
}

TEST_CASE("a warning before a fatal alert is not the alert that ended the handshake", "[net][tls][tlspeer]")
{
    // A TLS 1.2 server may warn `unrecognized_name` (112) and carry on; this one then finds no ALPN
    // name in common and refuses with `no_application_protocol` (120). The fatal one is the failure.
    constexpr auto UnrecognizedName = std::uint8_t { 112 };
    constexpr auto NoApplicationProtocol = std::uint8_t { 120 };
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial(),
                                                          { .maximumVersion = core::net::TlsVersion::Tls12,
                                                            .alpn = { "spdy/3" },
                                                            .warnWithAlert = UnrecognizedName }));
    auto const context = clientTrusting(localhostMaterial().certPem, { .alpn = { "http/1.1" } });
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");

    auto const outcome = handshakeBoth(*loopback, *socket);
    REQUIRE(outcome.finished);
    CHECK_FALSE(outcome.local.has_value());
    auto const failure = sessionOf(*socket).failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::AlertReceived);
    CHECK(failure->alert == NoApplicationProtocol);
}

TEST_CASE("failure() keeps the first failure", "[net][tls][tlspeer]")
{
    // The record that broke the connection is the failure; what a later call on the broken session
    // answers is a consequence of it, not a second cause.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const trusting = core::net::makeTlsClientContext();
    REQUIRE(trusting.has_value());
    auto const socket = (*trusting)->wrap(std::move(loopback->transport), loopback->loop);
    REQUIRE(socket != nullptr);
    REQUIRE(handshakeBoth(*loopback, *socket).local.has_value());

    static constexpr auto Garbage =
        std::array<std::uint8_t, 10> { 0x17, 0x03, 0x03, 0x00, 0x05, 'n', 'o', 'p', 'e', '!' };
    auto const readTwice = [](ISocket* tls,
                              ISocket* wire,
                              std::optional<core::net::TlsFailure>* first,
                              bool* secondFailed) -> core::async::Task<void> {
        std::ignore = co_await wire->write(std::as_bytes(std::span { Garbage }));
        auto buffer = std::array<std::byte, 64> {};
        std::ignore = co_await tls->read(buffer);
        *first = sessionOf(*tls).failure();
        *secondFailed = !(co_await tls->read(buffer)).has_value();
    };
    auto first = std::optional<core::net::TlsFailure> {};
    auto secondFailed = false;
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        readTwice(socket.get(), loopback->wire.get(), &first, &secondFailed),
        core::net::testing::TlsExchangeBound));
    REQUIRE(first.has_value());
    CHECK(secondFailed);
    auto const kept = sessionOf(*socket).failure();
    REQUIRE(kept.has_value());
    CHECK(kept->kind == first->kind);
    CHECK(kept->message == first->message);
}

TEST_CASE("a transport that ends mid-handshake is no TLS failure", "[net][tls][tlspeer]")
{
    // The peer read the ClientHello and went away: the operation's NetError says so, and failure()
    // stays empty -- an https client maps it to a connection error, not a TLS one.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto const context = clientTrusting(localhostMaterial().certPem);
    auto const socket = dial(*context, std::move(loopback->transport), loopback->loop, "localhost");

    auto result = std::expected<void, core::net::NetError> {};
    auto const client = [](ISocket* tls,
                           std::expected<void, core::net::NetError>* out) -> core::async::Task<void> {
        *out = co_await tls->handshakeIfNeeded();
    };

    SECTION("an orderly end: the whole ClientHello read, then a FIN")
    {
        auto const hangUp = [](ISocket* wire) -> core::async::Task<void> {
            auto buffer = std::array<std::byte, 16384> {};
            std::ignore = co_await wire->read(buffer); // the whole flight, written at once
            std::ignore = co_await wire->shutdownWrite();
        };
        REQUIRE(core::net::testing::blockOnWithin(
            loopback->loop,
            core::net::testing::allOf(client(socket.get(), &result), hangUp(loopback->wire.get())),
            core::net::testing::TlsExchangeBound));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == NetErrorCode::Eof);
    }

    SECTION("a reset: closed with the ClientHello half read")
    {
        auto const hangUp = [](ISocket* wire) -> core::async::Task<void> {
            auto buffer = std::array<std::byte, 16> {};
            std::ignore = co_await wire->read(buffer);
            wire->close();
        };
        REQUIRE(core::net::testing::blockOnWithin(
            loopback->loop,
            core::net::testing::allOf(client(socket.get(), &result), hangUp(loopback->wire.get())),
            core::net::testing::TlsExchangeBound));
        REQUIRE_FALSE(result.has_value());
        // A reset where the pair resets; where it only ends (macOS's AF_UNIX), the end it is.
        if constexpr (core::net::testing::NativeUnreadClose == core::net::testing::UnreadClose::Resets)
            CHECK(result.error().code != NetErrorCode::Eof);
        else
            CHECK(result.error().code == NetErrorCode::Eof);
    }

    CHECK_FALSE(sessionOf(*socket).failure().has_value());
}

TEST_CASE("a server's ALPN choice is its own first name the client offered", "[net][tls][alpn]")
{
    auto const own = core::net::detail::alpnWireFormat(std::vector<std::string> { "http/1.1", "h2" });
    REQUIRE(own.has_value());
    auto const chosen = [&](std::vector<unsigned char> const& offered) -> std::string {
        auto const name = core::net::detail::selectAlpn(*own, offered);
        return name ? std::string { name->begin(), name->end() } : std::string { "<none>" };
    };
    auto const wire = [](std::vector<std::string> const& names) {
        auto encoded = core::net::detail::alpnWireFormat(names);
        REQUIRE(encoded.has_value());
        return *encoded;
    };

    CHECK(chosen(wire({ "h2", "http/1.1" })) == "http/1.1");
    CHECK(chosen(wire({ "h2" })) == "h2");
    CHECK(chosen(wire({ "spdy/3" })) == "<none>");
    CHECK(chosen({}) == "<none>");
    // A malformed list ends where it stops parsing: a length that runs past the end, or a zero
    // length, hides what follows rather than being read through.
    CHECK(chosen({ 9, 'h', 't', 't', 'p', '/', '1', '.', '1' }) == "<none>");
    CHECK(chosen({ 0, 2, 'h', '2' }) == "<none>");
    CHECK(chosen({ 255, 2, 'h', '2' }) == "<none>");
    // A prefix of a name is not the name.
    CHECK(chosen({ 4, 'h', 't', 't', 'p' }) == "<none>");
    CHECK(core::net::detail::selectAlpn({}, wire({ "h2" })) == std::nullopt);
}

namespace
{

/// A transport that breaks once the peer has spoken: every write after a read has returned bytes
/// fails. Keyed to that STATE, not to a count of writes, so however a TLS library splits its first
/// flight, what fails is the flight after the server's -- the alert a refused certificate queues.
class FailingOncePeerSpoke final: public core::net::testing::SocketDecorator
{
  public:
    using SocketDecorator::SocketDecorator;

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return core::net::IoAwaitable { observedRead(buffer) };
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        if (!peerSpoke)
            return SocketDecorator::write(buffer);
        ++refusedWrites;
        return core::net::IoAwaitable { std::unexpected(
            core::net::makeNetError(NetErrorCode::ConnReset, 0, "the transport broke")) };
    }

    bool peerSpoke = false;
    int refusedWrites = 0;

  private:
    core::async::Task<core::net::IoResult> observedRead(std::span<std::byte> buffer)
    {
        auto const n = co_await SocketDecorator::read(buffer);
        if (n && *n > 0)
            peerSpoke = true;
        co_return n;
    }
};

} // namespace

TEST_CASE("a handshake failure is classified even when its alert cannot be sent", "[net][tls][tlspeer]")
{
    // The client refuses an untrusted certificate and queues a fatal alert; flushing it fails. The
    // refusal is what ended the handshake, and the transport's failure must not hide it.
    auto const stranger = core::net::generateSelfSignedCertificate({ .commonName = "stranger" });
    REQUIRE(stranger.has_value());
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(localhostMaterial()));
    auto failing = std::make_unique<FailingOncePeerSpoke>(*loopback->transport);
    auto const* const transport = failing.get();
    auto const context = clientTrusting(stranger->certPem);
    auto const socket = dial(*context, std::move(failing), loopback->loop, "localhost");

    auto result = std::expected<void, core::net::NetError> {};
    auto const local = [](ISocket* tls,
                          std::expected<void, core::net::NetError>* out) -> core::async::Task<void> {
        *out = co_await tls->handshakeIfNeeded();
    };
    // The server never hears the alert, so it waits for the client's Finished; it loses the race.
    auto const remote = [](StrictTlsPeer* peer, ISocket* wire) -> core::async::Task<void> {
        std::ignore = co_await peer->handshake(wire);
    };
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::anyOf(local(socket.get(), &result),
                                  remote(loopback->peer.get(), loopback->wire.get())),
        core::net::testing::TlsExchangeBound));

    CHECK(transport->refusedWrites >= 1); // the flush after the server's flight was refused
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == NetErrorCode::ConnReset);
    auto const failure = sessionOf(*socket).failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::Certificate);
}
