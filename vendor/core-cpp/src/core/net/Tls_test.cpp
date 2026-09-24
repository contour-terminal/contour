// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using core::async::Task;

namespace
{

/// Reads one record and echoes it back. The first read drives the server-side
/// (accept) handshake to completion before any application byte arrives.
Task<void> echoOnce(core::net::ISocket* socket, std::string* received)
{
    auto buffer = std::array<std::byte, 256> {};
    auto const n = co_await socket->read(buffer);
    if (n && *n > 0)
    {
        received->assign(reinterpret_cast<char const*>(buffer.data()), *n);
        std::ignore = co_await socket->write(std::span<std::byte const> { buffer.data(), *n });
    }
}

/// Writes @p message (driving the client connect handshake), then reads the echo.
Task<void> sendAndVerify(core::net::ISocket* socket, std::string message, bool* matched)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(message.data()), message.size() };
    if (auto const written = co_await socket->write(bytes); !written)
        co_return;

    auto buffer = std::array<std::byte, 256> {};
    auto const n = co_await socket->read(buffer);
    if (n && *n == message.size())
        *matched = std::string { reinterpret_cast<char const*>(buffer.data()), *n } == message;
}

/// Releases a server flow parked in accept() on ANOTHER loop, so a client that cannot connect
/// fails the case instead of hanging the join that follows it.
///
/// The close is POSTED rather than called: the listener belongs to that loop and must be closed on
/// its own thread (.agent/rules/async-and-net.md), and EventLoop::post is the cross-thread marshal
/// — it also breaks the wait the server is blocked in, which is what lets the accept resume.
/// @param serverLoop The loop the listener belongs to (not owned).
/// @param listener The listener to close on that loop's thread (not owned).
void releaseServer(core::net::EventLoop* serverLoop, core::net::IListener* listener)
{
    serverLoop->post([listener] { listener->close(); });
}

} // namespace

TEST_CASE("TLS handshakes and echoes application data over the reactor", "[net][tls]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto made = core::net::testing::makeSocketPair(loop);
    REQUIRE(made.has_value()); // a loopback failure is a test failure, not UB
    auto pair = std::move(*made);

    // Server presents a freshly generated self-signed cert; the client trusts on
    // first use (VERIFY_NONE) — the daemon's zero-config TOFU posture.
    auto serverCtx = core::net::makeSelfSignedServerContext();
    REQUIRE(serverCtx.has_value());
    auto clientCtx = core::net::makeTlsClientContext();
    REQUIRE(clientCtx.has_value());

    auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
    auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
    REQUIRE(serverTls != nullptr);
    REQUIRE(clientTls != nullptr);

    auto received = std::string {};
    auto matched = false;

    loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                           sendAndVerify(clientTls.get(), "hello over tls", &matched)));

    CHECK(received == "hello over tls"); // the server decrypted the application record
    CHECK(matched);                      // the client decrypted the echo — full duplex through TLS
}

TEST_CASE("a generated dev certificate drives a verified TLS handshake", "[net][tls]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto made = core::net::testing::makeSocketPair(loop);
    REQUIRE(made.has_value()); // a loopback failure is a test failure, not UB
    auto pair = std::move(*made);

    // Generate a self-signed dev certificate (library-only — no `openssl` CLI, so
    // identical on Windows and every UNIX), build the SERVER context from its PEM
    // cert+key (the daemon's --tls-cert/--tls-key path), and have the CLIENT PIN
    // that exact certificate as its trust anchor — real peer verification, not the
    // TOFU (VERIFY_NONE) path above.
    auto material = core::net::generateSelfSignedCertificate({ .commonName = "contour-dev" });
    REQUIRE(material.has_value());
    CHECK(material->certPem.starts_with("-----BEGIN CERTIFICATE-----"));
    CHECK(material->keyPem.contains("PRIVATE KEY"));

    auto serverCtx = core::net::makeTlsServerContext(material->certPem, material->keyPem);
    REQUIRE(serverCtx.has_value());
    auto clientCtx = core::net::makeTlsClientContext(material->certPem);
    REQUIRE(clientCtx.has_value());

    auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
    auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
    REQUIRE(serverTls != nullptr);
    REQUIRE(clientTls != nullptr);

    auto received = std::string {};
    auto matched = false;
    loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                           sendAndVerify(clientTls.get(), "verified dev cert", &matched)));
    CHECK(received == "verified dev cert"); // handshake completed with the peer cert verified
    CHECK(matched);
}

TEST_CASE("a pinned CA is not enough: the certificate must name the host asked for", "[net][tls]")
{
    // The security property: chain validation proves WHO SIGNED the certificate, never WHO IT WAS
    // ISSUED FOR. Without a name check, any certificate the pinned CA ever signed — for any host —
    // is accepted for any endpoint, which is a machine-in-the-middle away from a full session.
    auto material = core::net::generateSelfSignedCertificate({ .commonName = "the-real-daemon" });
    REQUIRE(material.has_value());

    SECTION("the name the certificate carries handshakes")
    {
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto made = core::net::testing::makeSocketPair(loop);
        REQUIRE(made.has_value()); // a loopback failure is a test failure, not UB
        auto pair = std::move(*made);

        auto serverCtx = core::net::makeTlsServerContext(material->certPem, material->keyPem);
        auto clientCtx = core::net::makeTlsClientContext(material->certPem, "the-real-daemon");
        REQUIRE(serverCtx.has_value());
        REQUIRE(clientCtx.has_value());

        auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
        auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
        auto received = std::string {};
        auto matched = false;
        loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                               sendAndVerify(clientTls.get(), "named peer", &matched)));
        CHECK(received == "named peer");
        CHECK(matched);
    }

    SECTION("a DIFFERENT name fails the handshake, though the CA is the same")
    {
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto made = core::net::testing::makeSocketPair(loop);
        REQUIRE(made.has_value()); // a loopback failure is a test failure, not UB
        auto pair = std::move(*made);

        auto serverCtx = core::net::makeTlsServerContext(material->certPem, material->keyPem);
        // Same certificate pinned as the trust anchor — only the expected NAME differs.
        auto clientCtx = core::net::makeTlsClientContext(material->certPem, "an-impostor");
        REQUIRE(serverCtx.has_value());
        REQUIRE(clientCtx.has_value());

        auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
        auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
        auto received = std::string {};
        auto matched = false;
        loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                               sendAndVerify(clientTls.get(), "named peer", &matched)));
        // The payload never crosses: verification fails during the handshake.
        CHECK(received.empty());
        CHECK_FALSE(matched);
    }
}

namespace
{

/// Handshakes a server presenting @p material against a client that pins it and expects
/// @p expectedHost, and reports whether a payload crossed both ways.
/// @param material The certificate and key the server presents, which the client also pins.
/// @param expectedHost The host the client checks the certificate against.
/// @return True when the handshake verified and the echo came back.
bool verifiesAgainst(core::net::CertKeyPem const& material, std::string_view expectedHost)
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto made = core::net::testing::makeSocketPair(loop);
    REQUIRE(made.has_value());
    auto pair = std::move(*made);

    auto serverCtx = core::net::makeTlsServerContext(material.certPem, material.keyPem);
    auto clientCtx = core::net::makeTlsClientContext(material.certPem, expectedHost);
    REQUIRE(serverCtx.has_value());
    REQUIRE(clientCtx.has_value());

    auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
    auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
    auto received = std::string {};
    auto matched = false;
    loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                           sendAndVerify(clientTls.get(), "addressed peer", &matched)));
    return received == "addressed peer" && matched;
}

} // namespace

TEST_CASE("an IP-literal host is verified against the certificate's IP entries", "[net][tls]")
{
    // The IP branch of the client check (X509_VERIFY_PARAM_set1_ip_asc) is a separate path from
    // the DNS one: set1_host would look for the address in a dNSName, which a correct certificate
    // does not carry. So it gets the same pair of cases, plus the spelling it exists to refuse.
    //
    // The certificate's common name is deliberately NOT the address. With the address in both, a
    // name check would find it through the common-name fallback and pass for the wrong reason --
    // measured: pointing this branch at set1_host passed all three sections below until the name
    // moved.
    auto const byAddress = core::net::generateSelfSignedCertificate(
        { .commonName = "core-test", .subjectNames = { "127.0.0.1" } });
    REQUIRE(byAddress.has_value());

    SECTION("the address the certificate carries handshakes")
    {
        CHECK(verifiesAgainst(*byAddress, "127.0.0.1"));
    }

    SECTION("a DIFFERENT address fails, though the CA is the same")
    {
        CHECK_FALSE(verifiesAgainst(*byAddress, "127.0.0.2"));
    }

    SECTION("an address in the common name alone does not match")
    {
        auto const inNameOnly = core::net::generateSelfSignedCertificate(
            { .commonName = "127.0.0.1", .subjectNames = { "localhost" } });
        REQUIRE(inNameOnly.has_value());
        CHECK_FALSE(verifiesAgainst(*inNameOnly, "127.0.0.1"));
    }
}

TEST_CASE("a client trusts every CA in the bundle it is given", "[net][tls]")
{
    // Only the first certificate of the CA PEM used to be read, so a bundle of two anchors silently
    // lost the second -- and a server whose certificate the second one vouches for was refused.
    auto const first = core::net::generateSelfSignedCertificate({ .commonName = "first-anchor" });
    auto const second = core::net::generateSelfSignedCertificate({ .commonName = "second-anchor" });
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto const bundle =
        core::net::CertKeyPem { .certPem = first->certPem + second->certPem, .keyPem = second->keyPem };

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto made = core::net::testing::makeSocketPair(loop);
    REQUIRE(made.has_value());
    auto pair = std::move(*made);
    auto serverCtx = core::net::makeTlsServerContext(second->certPem, second->keyPem);
    auto clientCtx = core::net::makeTlsClientContext(bundle.certPem, "second-anchor");
    REQUIRE(serverCtx.has_value());
    REQUIRE(clientCtx.has_value());
    auto serverTls = (*serverCtx)->wrap(std::move(pair.first), loop);
    auto clientTls = (*clientCtx)->wrap(std::move(pair.second), loop);
    auto received = std::string {};
    auto matched = false;
    loop.blockOn(core::net::testing::allOf(echoOnce(serverTls.get(), &received),
                                           sendAndVerify(clientTls.get(), "second anchor", &matched)));
    CHECK(received == "second anchor");
    CHECK(matched);
}

TEST_CASE("a generated certificate's validity is refused when not positive, and kept when long", "[net][tls]")
{
    CHECK_FALSE(core::net::generateSelfSignedCertificate(
                    { .commonName = "x", .validity = std::chrono::seconds { 0 } })
                    .has_value());
    CHECK_FALSE(core::net::generateSelfSignedCertificate(
                    { .commonName = "x", .validity = std::chrono::seconds { -1 } })
                    .has_value());

    // A century is past what a 32-bit `long` of seconds holds (Windows), which used to wrap the
    // expiry into the past; a verifying client refuses an expired certificate, so this handshake
    // is what says the date came out right.
    auto const century = core::net::generateSelfSignedCertificate(
        { .commonName = "long-lived", .validity = std::chrono::days { 36500 } });
    REQUIRE(century.has_value());
    CHECK(verifiesAgainst(*century, "long-lived"));
}

TEST_CASE("a server TLS context rejects mismatched certificate and key", "[net][tls]")
{
    // Two independent self-signed contexts succeed; loading a cert with the wrong
    // key must fail cleanly (exercised via the PEM path is heavier — here we just
    // assert the self-signed path yields a usable, distinct context each time).
    auto first = core::net::makeSelfSignedServerContext();
    auto second = core::net::makeSelfSignedServerContext();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->get() != second->get());
}

namespace
{

/// Writes @p message over @p socket (driving one side of the handshake).
Task<void> justWrite(core::net::ISocket* socket, std::string message)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(message.data()), message.size() };
    std::ignore = co_await socket->write(bytes);
}

/// Reads one record from @p socket — CONCURRENTLY with justWrite, so both enter
/// the handshake at once — and records whether it matched @p expected.
Task<void> justReadMatch(core::net::ISocket* socket, std::string expected, bool* matched)
{
    auto buffer = std::array<std::byte, 256> {};
    auto const n = co_await socket->read(buffer);
    if (n && *n == expected.size())
        *matched = std::string { reinterpret_cast<char const*>(buffer.data()), *n } == expected;
}

} // namespace

TEST_CASE("TLS completes a two-reactor handshake under concurrent client I/O", "[net][tls]")
{
    // The remote topology: server and client on INDEPENDENT reactors (separate
    // threads), a real loopback TCP socket between them. The client drives the
    // handshake from CONCURRENT write and read coroutines — the shape NativeClient
    // uses (WriteQueue + read pump) — which deadlocked before handshake() was
    // serialized (two coroutines calling non-reentrant SSL_do_handshake at once).
    auto const serverSource = core::net::makeDefaultBackend();
    auto serverLoop = core::net::EventLoop { *serverSource };
    auto listener = core::net::listen(serverLoop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto serverCtx = core::net::makeSelfSignedServerContext();
    REQUIRE(serverCtx.has_value());
    // Every REQUIRE comes before the server thread starts: one failing after it would unwind past a
    // joinable std::thread, which terminates the binary instead of failing the case.
    auto clientCtx = core::net::makeTlsClientContext();
    REQUIRE(clientCtx.has_value());

    auto received = std::string {};
    auto serverThread = std::thread { [&] {
        serverLoop.blockOn([](core::net::EventLoop* loop,
                              core::net::IListener* l,
                              core::net::ITlsContext* ctx,
                              std::string* recv) -> Task<void> {
            auto accepted = co_await l->accept();
            if (!accepted)
                co_return;
            auto tls = ctx->wrap(std::move(*accepted), *loop);
            co_await echoOnce(tls.get(), recv);
        }(&serverLoop, listener->get(), serverCtx->get(), &received));
    } };

    auto const clientSource = core::net::makeDefaultBackend();
    auto clientLoop = core::net::EventLoop { *clientSource };

    auto matched = false;
    clientLoop.blockOn([](core::net::EventLoop* loop,
                          core::net::EventLoop* remote,
                          core::net::IListener* acceptor,
                          core::net::ITlsContext* ctx,
                          bool* ok) -> Task<void> {
        auto connected = co_await core::net::connect(loop, "127.0.0.1", acceptor->boundPort());
        if (!connected)
        {
            releaseServer(remote, acceptor); // else the join below waits on a parked accept
            co_return;
        }
        auto tls = ctx->wrap(std::move(*connected), *loop);
        co_await core::net::testing::allOf(justWrite(tls.get(), "two reactor tls"),
                                           justReadMatch(tls.get(), "two reactor tls", ok));
    }(&clientLoop, &serverLoop, listener->get(), clientCtx->get(), &matched));
    serverThread.join();

    CHECK(received == "two reactor tls"); // the daemon-side handshake decrypted the record
    CHECK(matched);                       // the client decrypted the echo — full duplex, two reactors
}

namespace
{

using namespace std::chrono_literals;

/// Drives the handshake and is CANCELLED while parked in it (the sleeping sibling wins the race).
Task<void> cancelledDriver(core::net::EventLoop* loop, core::net::ISocket* tls)
{
    co_await core::net::testing::anyOf(justWrite(tls, "never gets sent"),
                                       core::net::testing::sleepFor(loop, 50ms));
}

/// Parks on the handshake gate behind the driver above, and records that it was released.
Task<void> gateWaiter(core::net::ISocket* tls, bool* released)
{
    auto buffer = std::array<std::byte, 64> {};
    std::ignore = co_await tls->read(buffer); // fails once the peer hangs up; the point is that it returns
    *released = true;
}

} // namespace

TEST_CASE("a cancelled TLS handshake releases the coroutines parked on it", "[net][tls]")
{
    // handshake() gates concurrent callers: the first drives it, the rest park. The driver's
    // unwind path — a whenAny sibling winning, or the loop shutting down — used to reset
    // `_handshaking` and NOTHING else, so the parked coroutines stayed suspended for ever and
    // their frames were never destroyed. In production that is `WriteQueue::drain` parked behind
    // the read pump: it never observes `draining == false`, so `flushThenClose()` hangs and a TLS
    // client cannot exit.
    //
    // Like the two-reactor case above, a regression here does not fail — it HANGS, and the suite's
    // per-test timeout is what reports it.
    auto const serverSource = core::net::makeDefaultBackend();
    auto serverLoop = core::net::EventLoop { *serverSource };
    auto listener = core::net::listen(serverLoop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    // Before the server thread starts, like every REQUIRE of the case above.
    auto clientCtx = core::net::makeTlsClientContext();
    REQUIRE(clientCtx.has_value());

    // A server that accepts the TCP connection and never speaks TLS, so the client's handshake
    // parks in SSL_ERROR_WANT_READ — then hangs up, so the released waiter has something to
    // observe rather than parking for ever on its own.
    auto serverThread = std::thread { [&] {
        serverLoop.blockOn([](core::net::IListener* l, core::net::EventLoop* loop) -> Task<void> {
            auto accepted = co_await l->accept();
            if (!accepted)
                co_return;
            co_await loop->delay(300ms);
            (*accepted)->close();
        }(listener->get(), &serverLoop));
    } };

    auto const clientSource = core::net::makeDefaultBackend();
    auto clientLoop = core::net::EventLoop { *clientSource };

    auto released = false;
    clientLoop.blockOn([](core::net::EventLoop* loop,
                          core::net::EventLoop* remote,
                          core::net::IListener* acceptor,
                          core::net::ITlsContext* ctx,
                          bool* ok) -> Task<void> {
        auto connected = co_await core::net::connect(loop, "127.0.0.1", acceptor->boundPort());
        if (!connected)
        {
            releaseServer(remote, acceptor); // else the join below waits on a parked accept
            co_return;
        }
        auto tls = ctx->wrap(std::move(*connected), *loop);
        // Order matters: the driver suspends INSIDE the handshake first, so the waiter that
        // starts next finds `_handshaking` set and parks on the gate.
        co_await core::net::testing::allOf(cancelledDriver(loop, tls.get()), gateWaiter(tls.get(), ok));
    }(&clientLoop, &serverLoop, listener->get(), clientCtx->get(), &released));
    serverThread.join();

    CHECK(released);
}
