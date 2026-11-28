// SPDX-License-Identifier: Apache-2.0
//
// Trust, and that it fails closed.
//
// A file trust store reads its anchors from what the environment names, or else from the first
// bundle the distribution table finds; a name that is set but leads nowhere, an empty store, an
// unreadable file and a malformed bundle are each a refusal, never a quiet fallback to the next
// source. Every case runs over `platform::testing::InMemoryFileSystem` and
// `core::testing::FakeEnvironment`, so it runs the same on every OS and touches no real file.
//
// A context built over an `IChainVerifier` -- what macOS and Windows use, so the platform's own
// trust settings decide -- completes the TLS handshake with OpenSSL's chain check off and moves no
// application byte until the verifier has answered: `ScriptedChainVerifier` answers from a blocking
// executor the case drains by hand, so "before it answered" is a state the case can stand in.
#include <core/Environment.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Tls.hpp>
#include <core/net/TrustStore.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/StrictTlsPeer.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "TlsTestSupport.hpp"

using core::async::testing::ManualExecutor;
using core::net::CertKeyPem;
using core::net::IChainVerifier;
using core::net::ISocket;
using core::net::ITlsContext;
using core::net::NetErrorCode;
using core::net::TlsFailureKind;
using core::net::TrustAnchors;
using core::net::TrustBundleKind;
using core::net::testing::ChainVerdict;
using core::net::testing::makeTlsLoopback;
using core::net::testing::ScriptedChainVerifier;
using core::net::testing::StreamEnd;
using core::net::testing::StrictTlsPeer;
using core::platform::testing::InMemoryFileSystem;
using core::testing::FakeEnvironment;

namespace
{

/// A certificate of its own for each of three indices, so a case can tell which bundle was read by
/// what it holds. Generated once: cheap, but not free.
CertKeyPem const& material(std::size_t index)
{
    static auto const materials = [] {
        auto generated = std::array<CertKeyPem, 3> {};
        for (auto& one: generated)
        {
            auto made = core::net::generateSelfSignedCertificate(
                { .commonName = "localhost", .subjectNames = { "localhost", "127.0.0.1" } });
            REQUIRE(made.has_value());
            one = std::move(*made);
        }
        return generated;
    }();
    return materials.at(index);
}

/// A certificate for every name the name table asks about: a DNS name and three addresses.
CertKeyPem const& everyNameMaterial()
{
    static auto const material = [] {
        auto made = core::net::generateSelfSignedCertificate(
            { .commonName = "localhost", .subjectNames = { "localhost", "127.0.0.1", "::1", "fe80::1" } });
        REQUIRE(made.has_value());
        return *made;
    }();
    return material;
}

/// A certificate whose only DNS-looking name is its common name: its one subjectAltName is an
/// address. RFC 9525 never reads the common name as a name.
CertKeyPem const& commonNameOnlyMaterial()
{
    static auto const material = [] {
        auto made = core::net::generateSelfSignedCertificate(
            { .commonName = "localhost", .subjectNames = { "127.0.0.1" } });
        REQUIRE(made.has_value());
        return *made;
    }();
    return material;
}

std::string const& pem(std::size_t index)
{
    return material(index).certPem;
}

/// The DER of @p pemText's one certificate.
core::net::DerCertificate derOf(std::string_view pemText)
{
    auto anchors = core::net::trustAnchorsFromPem(pemText);
    REQUIRE(anchors.has_value());
    REQUIRE(anchors->certificates.size() == 1);
    return std::move(anchors->certificates.front());
}

/// What a file trust store's `load()` answered, and whether it went through the blocking executor.
struct LoadOutcome
{
    std::expected<TrustAnchors, std::string> anchors = std::unexpected(std::string { "never answered" });
    bool offloaded = false; ///< The blocking executor held the work before anything was read.
};

/// Loads the file trust store over @p files and @p environment, draining both executors by hand.
LoadOutcome loadFileStore(core::platform::FileSystem const& files, core::Environment const& environment)
{
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto const store = core::net::makeFileTrustStore(files, environment, home, blocking);
    REQUIRE(store != nullptr);
    auto task = store->load();
    task.handle().resume();
    auto outcome = LoadOutcome {};
    outcome.offloaded = !task.done() && blocking.pending() == 1 && home.pending() == 0;
    blocking.drain();
    home.drain();
    REQUIRE(task.done());
    outcome.anchors = task.result();
    return outcome;
}

/// The anchors' count, or -1 for a refusal -- so a refusal and a wrong count read differently.
int anchorCount(LoadOutcome const& outcome)
{
    return outcome.anchors ? static_cast<int>(outcome.anchors->certificates.size()) : -1;
}

/// Builds a verifying client over @p verifier.
std::shared_ptr<ITlsContext> clientOver(std::shared_ptr<IChainVerifier> verifier,
                                        core::net::TlsClientOptions const& options = {})
{
    auto context = core::net::makeTlsClientContext(std::move(verifier), options);
    REQUIRE(context.has_value());
    return std::move(*context);
}

std::span<std::byte const> asBytes(std::string_view text)
{
    return std::as_bytes(std::span { text.data(), text.size() });
}

/// What one exchange through a verifier-checked client saw.
struct VerifiedExchange
{
    bool finished = false;             ///< All three parties answered within the bound.
    bool verifierHeld = false;         ///< The verification was parked on the blocking executor.
    std::size_t receivedWhileHeld = 1; ///< Plaintext bytes the server had while it was parked.
    std::expected<std::size_t, core::net::NetError> written =
        std::unexpected(core::net::makeNetError(NetErrorCode::Timeout));
    std::string received;              ///< Every plaintext byte the server read.
    StreamEnd end = StreamEnd::Failed; ///< How the server's stream ended.
};

/// Writes `hello` through @p socket to the loopback's server, holding the verification open until
/// the case has looked, then ends the stream: with `close_notify` if the write went through, by
/// closing the transport if it did not.
VerifiedExchange exchangeThroughVerifier(core::net::testing::TlsLoopback& loopback,
                                         ISocket& socket,
                                         ManualExecutor& blocking)
{
    auto outcome = VerifiedExchange {};
    auto const local = [](ISocket* tls, VerifiedExchange* out) -> core::async::Task<void> {
        out->written = co_await tls->write(asBytes("hello"));
        if (out->written)
            std::ignore = co_await tls->shutdownWrite();
        else
            tls->close();
    };
    auto const remote =
        [](StrictTlsPeer* peer, ISocket* wire, VerifiedExchange* out) -> core::async::Task<void> {
        if (auto const handshaken = co_await peer->handshake(wire); !handshaken)
            co_return;
        out->end = co_await peer->readToEnd(wire, &out->received);
    };
    auto const hold = [](core::net::EventLoop* loop,
                         ManualExecutor* executor,
                         VerifiedExchange* out) -> core::async::Task<void> {
        out->verifierHeld =
            co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; });
        out->receivedWhileHeld = out->received.size();
        executor->drain();
    };
    outcome.finished = core::net::testing::blockOnWithin(
        loopback.loop,
        core::net::testing::allOf(local(&socket, &outcome),
                                  remote(loopback.peer.get(), loopback.wire.get(), &outcome),
                                  hold(&loopback.loop, &blocking, &outcome)),
        core::net::testing::TlsExchangeBound);
    return outcome;
}

} // namespace

TEST_CASE("a file trust store reads the first bundle the table names, off the loop", "[net][tls][truststore]")
{
    auto const files = InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", pem(0) + pem(1) } };
    auto const outcome = loadFileStore(files, FakeEnvironment {});
    CHECK(outcome.offloaded);
    REQUIRE(outcome.anchors.has_value());
    CHECK(outcome.anchors->certificates.size() == 2);
    CHECK(outcome.anchors->certificates.front() == derOf(pem(0)));
}

TEST_CASE("the first existing table row wins, and a later one is never read", "[net][tls][truststore]")
{
    // Debian's bundle comes before Fedora's in the table, so the one certificate is what is read.
    auto const files = InMemoryFileSystem { { "/etc/ssl/certs/ca-certificates.crt", pem(0) },
                                            { "/etc/pki/tls/certs/ca-bundle.crt", pem(0) + pem(1) } };
    CHECK(anchorCount(loadFileStore(files, FakeEnvironment {})) == 1);

    // The first EXISTING row: a malformed one is not skipped for the next.
    auto const broken = InMemoryFileSystem { { "/etc/ssl/certs/ca-certificates.crt", "not a bundle" },
                                             { "/etc/pki/tls/certs/ca-bundle.crt", pem(0) } };
    CHECK(anchorCount(loadFileStore(broken, FakeEnvironment {})) == -1);
}

TEST_CASE("SSL_CERT_FILE names the bundle, and a named but missing one is an error", "[net][tls][truststore]")
{
    auto const files = InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", pem(0) + pem(1) },
                                            { "/present.pem", pem(2) } };

    SECTION("a missing file is an error, not a fallback to the table")
    {
        auto const outcome = loadFileStore(files, FakeEnvironment { { { "SSL_CERT_FILE", "/custom.pem" } } });
        REQUIRE_FALSE(outcome.anchors.has_value());
        CHECK(outcome.anchors.error().contains("SSL_CERT_FILE"));
        CHECK(outcome.anchors.error().contains("/custom.pem"));
    }

    SECTION("a present file is what is read")
    {
        auto const outcome =
            loadFileStore(files, FakeEnvironment { { { "SSL_CERT_FILE", "/present.pem" } } });
        REQUIRE(outcome.anchors.has_value());
        REQUIRE(outcome.anchors->certificates.size() == 1);
        CHECK(outcome.anchors->certificates.front() == derOf(pem(2)));
    }

    SECTION("it wins over SSL_CERT_DIR")
    {
        auto const environment =
            FakeEnvironment { { { "SSL_CERT_FILE", "/present.pem" }, { "SSL_CERT_DIR", "/nowhere" } } };
        CHECK(anchorCount(loadFileStore(files, environment)) == 1);
    }

    SECTION("a directory is not a bundle")
    {
        auto const withDirectory = InMemoryFileSystem { { .path = "/certs", .isDirectory = true } };
        CHECK(anchorCount(loadFileStore(withDirectory, FakeEnvironment { { { "SSL_CERT_FILE", "/certs" } } }))
              == -1);
    }

    SECTION("an empty value is unset")
    {
        CHECK(anchorCount(loadFileStore(files, FakeEnvironment { { { "SSL_CERT_FILE", "" } } })) == 2);
    }
}

TEST_CASE("SSL_CERT_DIR loads every hashed file, and only those", "[net][tls][truststore]")
{
    // OpenSSL looks a hashed directory up by `<subject hash>.<n>`, the hash in lower case; a CRL is
    // `<hash>.r<n>`, and anything else -- the PEM files the links point at, a README, a directory
    // that happens to be named like a certificate -- is not what it reads.
    auto const files = InMemoryFileSystem { { "/certs/0123abcd.0", pem(0) },
                                            { "/certs/89abcdef.1", pem(1) },
                                            { "/certs/0123abcd.r0", "a CRL, not a certificate" },
                                            { "/certs/README", "not a certificate" },
                                            { "/certs/ca.pem", pem(2) },
                                            { "/certs/0123ABCD.0", pem(2) },
                                            { "/certs/0123abcd.0x", pem(2) },
                                            { .path = "/certs/abcdef01.0", .isDirectory = true } };

    SECTION("both hashed files are read")
    {
        auto const outcome = loadFileStore(files, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } });
        CHECK(outcome.offloaded);
        REQUIRE(outcome.anchors.has_value());
        REQUIRE(outcome.anchors->certificates.size() == 2);
        // Counted, not compared as a std::set: GCC 14 at -O3 reads the set's ordering of byte
        // vectors as a memcmp beyond any object.
        auto const& loaded = outcome.anchors->certificates;
        CHECK(std::ranges::count(loaded, derOf(pem(0))) == 1);
        CHECK(std::ranges::count(loaded, derOf(pem(1))) == 1);
    }

    SECTION("a missing directory is an error")
    {
        CHECK(anchorCount(loadFileStore(files, FakeEnvironment { { { "SSL_CERT_DIR", "/elsewhere" } } }))
              == -1);
    }

    SECTION("a directory with no hashed file is an empty store, and refused")
    {
        auto const unhashed = InMemoryFileSystem { { "/certs/ca.pem", pem(0) } };
        CHECK(anchorCount(loadFileStore(unhashed, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } }))
              == -1);
    }

    SECTION("one hashed file that does not parse refuses the whole directory")
    {
        auto const broken =
            InMemoryFileSystem { { "/certs/0123abcd.0", pem(0) }, { "/certs/89abcdef.0", "garbage" } };
        CHECK(anchorCount(loadFileStore(broken, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } })) == -1);
    }
}

TEST_CASE("a named file that is not a regular file is refused without being read", "[net][tls][truststore]")
{
    // A device has no end: reading /dev/zero as a bundle would never return. Through the native
    // file system, because the in-memory one models no device.
    auto const files = core::platform::NativeFileSystem {};
    if (!files.exists("/dev/zero"))
        SKIP("this system has no /dev/zero");
    auto const outcome = loadFileStore(files, FakeEnvironment { { { "SSL_CERT_FILE", "/dev/zero" } } });
    REQUIRE_FALSE(outcome.anchors.has_value());
    CHECK(outcome.anchors.error().contains("not a readable regular file"));
}

TEST_CASE("a bundle or a directory larger than a trust store reads is refused", "[net][tls][truststore]")
{
    // Padding after the certificate is text PEM skips, so the size is all that differs.
    auto const padded = [](std::string body, std::uintmax_t size) {
        body.resize(static_cast<std::size_t>(size), '\n');
        return body;
    };
    constexpr auto Limit = core::net::MaxTrustBundleBytes;

    SECTION("a bundle of exactly the limit is read, one byte more is not")
    {
        auto const atLimit =
            InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", padded(pem(0), Limit) } };
        CHECK(anchorCount(loadFileStore(atLimit, FakeEnvironment {})) == 1);
        auto const over =
            InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", padded(pem(0), Limit + 1) } };
        auto const outcome = loadFileStore(over, FakeEnvironment {});
        REQUIRE_FALSE(outcome.anchors.has_value());
        CHECK(outcome.anchors.error().contains("more than"));
    }

    SECTION("a directory's files are bounded together")
    {
        constexpr auto Half = core::net::MaxTrustDirectoryBytes / 2;
        auto const atLimit = InMemoryFileSystem { { "/certs/0123abcd.0", padded(pem(0), Half) },
                                                  { "/certs/89abcdef.0", padded(pem(1), Half) } };
        CHECK(anchorCount(loadFileStore(atLimit, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } })) == 2);
        auto const over = InMemoryFileSystem { { "/certs/0123abcd.0", padded(pem(0), Half) },
                                               { "/certs/89abcdef.0", padded(pem(1), Half + 1) } };
        auto const outcome = loadFileStore(over, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } });
        REQUIRE_FALSE(outcome.anchors.has_value());
        CHECK(outcome.anchors.error().contains("more than"));
    }
}

TEST_CASE("an empty store, an unreadable file or a malformed bundle is a refusal", "[net][tls][truststore]")
{
    SECTION("nothing configured and no table row present")
    {
        auto const outcome = loadFileStore(InMemoryFileSystem {}, FakeEnvironment {});
        REQUIRE_FALSE(outcome.anchors.has_value());
        CHECK(outcome.anchors.error().contains("SSL_CERT_FILE"));
    }

    SECTION("an empty bundle")
    {
        auto const files = InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", "" } };
        CHECK(anchorCount(loadFileStore(files, FakeEnvironment {})) == -1);
    }

    SECTION("a bundle that cannot be read")
    {
        auto files = InMemoryFileSystem { { "/etc/pki/tls/certs/ca-bundle.crt", pem(0) } };
        files.denyAccess("/etc/pki/tls/certs/ca-bundle.crt");
        CHECK(anchorCount(loadFileStore(files, FakeEnvironment {})) == -1);
    }

    SECTION("a hashed file that cannot be read")
    {
        auto files = InMemoryFileSystem { { "/certs/0123abcd.0", pem(0) }, { "/certs/89abcdef.0", pem(1) } };
        files.denyAccess("/certs/89abcdef.0");
        CHECK(anchorCount(loadFileStore(files, FakeEnvironment { { { "SSL_CERT_DIR", "/certs" } } })) == -1);
    }
}

TEST_CASE("the trust-source table names each location once, every bundle before any directory",
          "[net][tls][truststore]")
{
    auto const rows = core::net::trustBundleLocations();
    auto paths = std::set<std::string_view> {};
    for (auto const& row: rows)
    {
        CHECK(paths.insert(row.path).second);
        CHECK_FALSE(row.distribution.empty());
    }
    for (auto const required: { "/etc/ssl/certs/ca-certificates.crt",
                                "/etc/pki/tls/certs/ca-bundle.crt",
                                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                                "/etc/ssl/ca-bundle.pem",
                                "/etc/ssl/cert.pem",
                                "/usr/local/share/certs/ca-root-nss.crt",
                                "/etc/ssl/certs" })
        CHECK(paths.contains(required));

    // A hashed directory is the fallback, never ahead of a bundle: on Debian `/etc/ssl/certs` exists
    // beside the bundle in it, and reading its links instead would read the same anchors slower.
    auto const firstDirectory =
        std::ranges::find(rows, TrustBundleKind::HashedDirectory, &core::net::TrustBundleLocation::kind);
    CHECK(std::ranges::none_of(std::ranges::subrange { firstDirectory, rows.end() },
                               [](auto const& row) { return row.kind == TrustBundleKind::Bundle; }));
}

TEST_CASE("every table row, alone on a machine, is what the store reads", "[net][tls][truststore]")
{
    for (auto const& row: core::net::trustBundleLocations())
    {
        DYNAMIC_SECTION(row.path << " (" << row.distribution << ")")
        {
            auto const path = std::string { row.path };
            // Filled by calls rather than chosen between two initializer lists: GCC 14 reads the
            // lists' backing arrays as dangling across the conditional (-Wdangling-pointer).
            auto files = InMemoryFileSystem {};
            if (row.kind == TrustBundleKind::Bundle)
                files.addFile(path, pem(0) + pem(1));
            else
            {
                files.addFile(path + "/0123abcd.0", pem(0));
                files.addFile(path + "/89abcdef.0", pem(1));
            }
            CHECK(anchorCount(loadFileStore(files, FakeEnvironment {})) == 2);
        }
    }
}

TEST_CASE("a PEM trust store answers the anchors of its bundle", "[net][tls][truststore]")
{
    auto const two = core::async::syncRun(core::net::makePemTrustStore(pem(0) + pem(1))->load());
    REQUIRE(two.has_value());
    CHECK(two->certificates.size() == 2);

    CHECK_FALSE(core::async::syncRun(core::net::makePemTrustStore("")->load()).has_value());
    CHECK_FALSE(core::async::syncRun(core::net::makePemTrustStore("garbage")->load()).has_value());
}

TEST_CASE("a client context over a trust store's anchors verifies through TrustSource",
          "[net][tls][truststore]")
{
    auto anchors = core::async::syncRun(core::net::makePemTrustStore(pem(0))->load());
    REQUIRE(anchors.has_value());
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0), { .alpn = { "http/1.1" } }));
    auto const context = core::net::makeTlsClientContext(std::move(*anchors), { .alpn = { "http/1.1" } });
    REQUIRE(context.has_value());
    auto socket =
        (*context)->wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto const outcome = core::net::testing::handshakeBoth(*loopback, **socket);
    REQUIRE(outcome.finished);
    REQUIRE(outcome.local.has_value());
    auto const* const session = (*socket)->tlsSession();
    REQUIRE(session != nullptr);
    // Guarded again: GCC 14 cannot see that REQUIRE leaves no null pointer behind it.
    CHECK((session != nullptr && session->negotiatedProtocol() == "http/1.1"));
}

TEST_CASE("a verifier-checked client moves no application byte until the verifier accepts",
          "[net][tls][truststore][verifier]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0), { .alpn = { "http/1.1" } }));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto const context = clientOver(verifier, { .alpn = { "http/1.1" } });
    auto socket =
        context->wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto const outcome = exchangeThroughVerifier(*loopback, **socket, blocking);
    REQUIRE(outcome.finished);
    CHECK(outcome.verifierHeld);
    CHECK(outcome.receivedWhileHeld == 0);
    REQUIRE(outcome.written.has_value());
    CHECK(*outcome.written == 5);
    CHECK(outcome.received == "hello");
    CHECK(outcome.end == StreamEnd::CloseNotify);

    // It was asked once, about the chain the server presented, for the name the connection was for.
    REQUIRE(verifier->chains.size() == 1);
    REQUIRE_FALSE(verifier->chains.front().empty());
    CHECK(verifier->chains.front().front() == derOf(pem(0)));
    CHECK(verifier->serverNames == std::vector<std::string> { "localhost" });
    CHECK(loopback->peer->receivedServerName() == "localhost");

    auto const* const session = (*socket)->tlsSession();
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->failure().has_value());
    CHECK(session->negotiatedProtocol() == "http/1.1");
    CHECK(session->negotiatedVersion() == core::net::TlsVersion::Tls13);
    CHECK(session->channelBinding().has_value());
    CHECK(session->peerCertificateDigest().has_value());
}

TEST_CASE("an operation that arrives while the chain is being verified waits for the verdict",
          "[net][tls][truststore][verifier]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto socket = clientOver(verifier)->wrapClient(
        std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    struct Seen
    {
        bool held = false;
        bool writeDoneWhileHeld = true;
        std::size_t receivedWhileHeld = 1;
        std::expected<std::size_t, core::net::NetError> written =
            std::unexpected(core::net::makeNetError(NetErrorCode::Timeout));
        std::string received;
        StreamEnd end = StreamEnd::Failed;
    };
    auto seen = Seen {};
    auto writeDone = false;
    // The handshake is driven by one flow; the write arrives from another once the verifier holds.
    auto const driver = [](ISocket* tls) -> core::async::Task<void> {
        std::ignore = co_await tls->handshakeIfNeeded();
    };
    auto const writer = [](core::net::EventLoop* loop,
                           ManualExecutor* executor,
                           ISocket* tls,
                           Seen* out,
                           bool* done) -> core::async::Task<void> {
        if (!co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; }))
            co_return;
        out->written = co_await tls->write(asBytes("early"));
        *done = true;
        if (out->written)
            std::ignore = co_await tls->shutdownWrite();
    };
    auto const remote = [](StrictTlsPeer* peer, ISocket* wire, Seen* out) -> core::async::Task<void> {
        if (auto const handshaken = co_await peer->handshake(wire); !handshaken)
            co_return;
        out->end = co_await peer->readToEnd(wire, &out->received);
    };
    auto const hold = [](core::net::EventLoop* loop,
                         ManualExecutor* executor,
                         Seen* out,
                         bool const* done) -> core::async::Task<void> {
        out->held =
            co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; });
        // Time for the write to reach the wire, were it let through.
        co_await loop->delay(std::chrono::milliseconds { 50 });
        out->writeDoneWhileHeld = *done;
        out->receivedWhileHeld = out->received.size();
        executor->drain();
    };
    auto* const tls = socket->get();
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::allOf(driver(tls),
                                  writer(&loopback->loop, &blocking, tls, &seen, &writeDone),
                                  remote(loopback->peer.get(), loopback->wire.get(), &seen),
                                  hold(&loopback->loop, &blocking, &seen, &writeDone)),
        core::net::testing::TlsExchangeBound));
    CHECK(seen.held);
    CHECK_FALSE(seen.writeDoneWhileHeld);
    CHECK(seen.receivedWhileHeld == 0);
    REQUIRE(seen.written.has_value());
    CHECK(seen.received == "early");
    CHECK(verifier->chains.size() == 1);
}

TEST_CASE("a refusing verifier fails the connection closed", "[net][tls][truststore][verifier]")
{
    // A refusal is a certificate failure whatever the verifier called it.
    auto const verdict = GENERATE(ChainVerdict::Refuse, ChainVerdict::RefuseAsProtocol);
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0), { .alpn = { "http/1.1" } }));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(verdict, blocking);
    auto const context = clientOver(verifier, { .alpn = { "http/1.1" } });
    auto socket =
        context->wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto const outcome = exchangeThroughVerifier(*loopback, **socket, blocking);
    REQUIRE(outcome.finished);
    CHECK(outcome.verifierHeld);
    REQUIRE_FALSE(outcome.written.has_value());
    CHECK(outcome.received.empty());
    CHECK(verifier->chains.size() == 1);

    auto const* const session = (*socket)->tlsSession();
    REQUIRE(session != nullptr);
    auto const failure = session->failure();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == TlsFailureKind::Certificate);
    CHECK(failure->message == ScriptedChainVerifier::RefusalMessage);

    // A refused peer's session answers as one that never finished: no identity, no channel to
    // bind to, nothing negotiated -- though OpenSSL's handshake did finish under it.
    CHECK_FALSE(session->channelBinding().has_value());
    CHECK_FALSE(session->peerCertificateDigest().has_value());
    CHECK_FALSE(session->negotiatedVersion().has_value());
    CHECK(session->negotiatedProtocol().empty());

    // Sticky: the next operation answers the refusal again, and asks the verifier nothing more.
    auto const again = core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::allOf(
            [](ISocket* tls) -> core::async::Task<void> {
                auto const handshaken = co_await tls->handshakeIfNeeded();
                CHECK_FALSE(handshaken.has_value());
            }(socket->get()),
            // Answers a second verification, should one be asked: a regression then fails the count
            // below rather than parking the handshake on an executor nobody drains.
            [](core::net::EventLoop* loop, ManualExecutor* executor) -> core::async::Task<void> {
                std::ignore = co_await core::net::testing::waitUntil(
                    loop, [executor] { return executor->pending() == 1; }, 50);
                executor->drain();
            }(&loopback->loop, &blocking)),
        core::net::testing::TlsExchangeBound);
    CHECK(again);
    CHECK(verifier->chains.size() == 1);
}

TEST_CASE("a verifier is asked for the name as the certificate check reads it",
          "[net][tls][truststore][verifier]")
{
    struct Row
    {
        std::string_view given;   ///< `TlsPeer::serverName`.
        std::string_view checked; ///< What the verifier is asked about.
        std::string_view sni;     ///< What the server received as SNI.
    };
    auto const rows = std::vector<Row> {
        { .given = "localhost", .checked = "localhost", .sni = "localhost" },
        { .given = "127.0.0.1", .checked = "127.0.0.1", .sni = "" },
        { .given = "[::1]", .checked = "::1", .sni = "" },
        { .given = "fe80::1%eth0", .checked = "fe80::1", .sni = "" },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION("'" << row.given << "'")
        {
            auto loopback = makeTlsLoopback(StrictTlsPeer::server(everyNameMaterial()));
            auto blocking = ManualExecutor {};
            auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
            auto socket = clientOver(verifier)->wrapClient(
                std::move(loopback->transport), loopback->loop, { .serverName = std::string { row.given } });
            REQUIRE(socket.has_value());
            auto const outcome = exchangeThroughVerifier(*loopback, **socket, blocking);
            REQUIRE(outcome.finished);
            CHECK(outcome.received == "hello");
            CHECK(verifier->serverNames == std::vector { std::string { row.checked } });
            CHECK(loopback->peer->receivedServerName() == row.sni);
        }
    }
}

TEST_CASE("a verifier context checks the leaf's name itself, before any verifier is asked",
          "[net][tls][truststore][verifier]")
{
    // The same rules on every platform -- whole-label wildcards, subjectAltNames only -- whatever
    // the platform verifier would have said: none of these chains reaches it.
    struct Row
    {
        std::string_view what;
        CertKeyPem const* material;
        std::string_view name;
    };
    auto const rows = std::vector<Row> {
        { .what = "another DNS name", .material = &everyNameMaterial(), .name = "other.test" },
        { .what = "another address", .material = &everyNameMaterial(), .name = "127.0.0.2" },
        { .what = "another IPv6 address", .material = &everyNameMaterial(), .name = "[::2]" },
        { .what = "a name only the common name carries",
          .material = &commonNameOnlyMaterial(),
          .name = "localhost" },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.what)
        {
            auto loopback = makeTlsLoopback(StrictTlsPeer::server(*row.material));
            auto blocking = ManualExecutor {};
            auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
            auto socket = clientOver(verifier)->wrapClient(
                std::move(loopback->transport), loopback->loop, { .serverName = std::string { row.name } });
            REQUIRE(socket.has_value());
            auto const outcome = exchangeThroughVerifier(*loopback, **socket, blocking);
            REQUIRE(outcome.finished);
            CHECK_FALSE(outcome.verifierHeld);
            CHECK_FALSE(outcome.written.has_value());
            CHECK(outcome.received.empty());
            CHECK(verifier->chains.empty());
            auto const* const session = (*socket)->tlsSession();
            REQUIRE(session != nullptr);
            auto const failure = session->failure();
            REQUIRE(failure.has_value());
            CHECK(failure->kind == TlsFailureKind::Certificate);
        }
    }
}

TEST_CASE("a verifier context refuses a name it could not check, and wrap verifies the chain alone",
          "[net][tls][truststore][verifier]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto const context = clientOver(verifier);

    SECTION("a bracketed name that is no IPv6 literal")
    {
        auto refused = context->wrapClient(
            std::move(loopback->transport), loopback->loop, { .serverName = "[localhost]" });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == NetErrorCode::AddressError);
        CHECK(verifier->chains.empty());
    }

    SECTION("an empty name, which would check the chain alone")
    {
        auto refused = context->wrapClient(std::move(loopback->transport), loopback->loop, {});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == NetErrorCode::AddressError);
        CHECK(verifier->chains.empty());
    }

    SECTION("wrap asks the verifier too, with no name")
    {
        auto socket = context->wrap(std::move(loopback->transport), loopback->loop);
        REQUIRE(socket != nullptr);
        auto const outcome = exchangeThroughVerifier(*loopback, *socket, blocking);
        REQUIRE(outcome.finished);
        CHECK(outcome.verifierHeld);
        CHECK(outcome.received == "hello");
        CHECK(verifier->serverNames == std::vector<std::string> { "" });
    }
}

TEST_CASE("a client context needs a verifier to be one", "[net][tls][truststore][verifier]")
{
    CHECK_FALSE(core::net::makeTlsClientContext(std::shared_ptr<IChainVerifier> {}).has_value());
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    CHECK_FALSE(core::net::makeTlsClientContext(verifier, { .alpn = { "" } }).has_value());
}

TEST_CASE("a socket destroyed while its chain is being verified unwinds the handshake",
          "[net][tls][truststore][verifier]")
{
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto socket = clientOver(verifier)->wrapClient(
        std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto unwound = false;
    auto const local = [](ISocket* tls, bool* out) -> core::async::Task<void> {
        try
        {
            std::ignore = co_await tls->handshakeIfNeeded();
        }
        catch (core::async::OperationCancelled const&)
        {
            *out = true;
        }
    };
    auto const remote = [](StrictTlsPeer* peer, ISocket* wire) -> core::async::Task<void> {
        std::ignore = co_await peer->handshake(wire);
    };
    auto held = false;
    auto const destroy = [](core::net::EventLoop* loop,
                            ManualExecutor* executor,
                            std::unique_ptr<ISocket>* tls,
                            bool* out) -> core::async::Task<void> {
        *out = co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; });
        tls->reset();
        executor->drain();
    };
    auto* const raw = socket->get();
    CHECK(core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::allOf(local(raw, &unwound),
                                  remote(loopback->peer.get(), loopback->wire.get()),
                                  destroy(&loopback->loop, &blocking, &*socket, &held)),
        core::net::testing::TlsExchangeBound));
    CHECK(held);
    CHECK(unwound);
}

namespace
{

/// Drives @p tls's handshake, bounded by @p limit on @p loop's clock.
/// @param out Set to whether the bound ran out first; otherwise to false, with @p result set.
core::async::Task<void> handshakeWithin(core::net::EventLoop* loop,
                                        ISocket* tls,
                                        std::chrono::milliseconds limit,
                                        std::optional<bool>* timedOut,
                                        std::optional<std::expected<void, core::net::NetError>>* result)
{
    auto const drive = [](ISocket* socket) -> core::async::Task<std::expected<void, core::net::NetError>> {
        co_return co_await socket->handshakeIfNeeded();
    };
    auto answer = co_await core::net::withTimeout(loop, drive(tls), limit);
    *timedOut = !answer.has_value();
    if (answer.has_value())
        *result = std::move(*answer);
}

/// The server's half of a handshake, its outcome ignored.
core::async::Task<void> serverHandshake(StrictTlsPeer* peer, ISocket* wire)
{
    std::ignore = co_await peer->handshake(wire);
}

} // namespace

TEST_CASE("a stop ends the wait for a verifier that has not answered, and a retry joins it",
          "[net][tls][truststore][verifier]")
{
    // The verification runs on a blocking executor nobody drains, so only the bound can end the
    // handshake: the stop must reach the wait, not the evaluation, which is left to finish alone.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto socket = clientOver(verifier)->wrapClient(
        std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto timedOut = std::optional<bool> {};
    auto result = std::optional<std::expected<void, core::net::NetError>> {};
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::allOf(
            handshakeWithin(
                &loopback->loop, socket->get(), std::chrono::milliseconds { 50 }, &timedOut, &result),
            serverHandshake(loopback->peer.get(), loopback->wire.get())),
        core::net::testing::TlsExchangeBound));
    CHECK(timedOut == true);
    CHECK(blocking.pending() == 1);

    // The evaluation answers while nobody waits; the retry is handed that answer rather than asking
    // the verifier a second time.
    blocking.drain();
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        handshakeWithin(
            &loopback->loop, socket->get(), core::net::testing::TlsExchangeBound, &timedOut, &result),
        core::net::testing::TlsExchangeBound));
    CHECK(timedOut == false);
    REQUIRE(result.has_value());
    CHECK(result->has_value());
    CHECK(verifier->chains.size() == 1);
}

TEST_CASE("a verification outlives the socket and the loop a stop left it running for",
          "[net][tls][truststore][verifier]")
{
    // The evaluation finishes on the blocking executor and only settles the call: nothing of it
    // resumes on the socket's loop, which is gone by the time the platform answers.
    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto const context = clientOver(verifier);
    auto socket =
        context->wrapClient(std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
    REQUIRE(socket.has_value());

    auto timedOut = std::optional<bool> {};
    auto result = std::optional<std::expected<void, core::net::NetError>> {};
    REQUIRE(core::net::testing::blockOnWithin(
        loopback->loop,
        core::net::testing::allOf(
            handshakeWithin(
                &loopback->loop, socket->get(), std::chrono::milliseconds { 50 }, &timedOut, &result),
            serverHandshake(loopback->peer.get(), loopback->wire.get())),
        core::net::testing::TlsExchangeBound));
    REQUIRE(timedOut == true);
    REQUIRE(blocking.pending() == 1);

    socket->reset();
    loopback.reset();
    blocking.drain();
    CHECK(blocking.pending() == 0);
    CHECK(verifier->chains.size() == 1);
}

TEST_CASE("verifications a stop left running count against the context's cap",
          "[net][tls][truststore][verifier]")
{
    auto first = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto second = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto const context = clientOver(verifier, { .maxVerificationsInFlight = 1 });

    auto held = context->wrapClient(std::move(first->transport), first->loop, { .serverName = "localhost" });
    REQUIRE(held.has_value());
    auto timedOut = std::optional<bool> {};
    auto result = std::optional<std::expected<void, core::net::NetError>> {};
    REQUIRE(core::net::testing::blockOnWithin(
        first->loop,
        core::net::testing::allOf(
            handshakeWithin(&first->loop, held->get(), std::chrono::milliseconds { 50 }, &timedOut, &result),
            serverHandshake(first->peer.get(), first->wire.get())),
        core::net::testing::TlsExchangeBound));
    REQUIRE(timedOut == true);
    REQUIRE(blocking.pending() == 1);

    // The first evaluation still holds the only slot, though nobody waits for it any more.
    auto refused =
        context->wrapClient(std::move(second->transport), second->loop, { .serverName = "localhost" });
    REQUIRE(refused.has_value());
    REQUIRE(core::net::testing::blockOnWithin(
        second->loop,
        core::net::testing::allOf(
            handshakeWithin(
                &second->loop, refused->get(), core::net::testing::TlsExchangeBound, &timedOut, &result),
            serverHandshake(second->peer.get(), second->wire.get())),
        core::net::testing::TlsExchangeBound));
    CHECK(timedOut == false);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error().code == NetErrorCode::ResourceExhausted);
    CHECK_FALSE(core::net::isDeadlineExpiry(result->error().code));
    CHECK(verifier->chains.empty()); // the held evaluation has not run; the refused one never started

    // The refusal is not sticky: once the slot is given back, the same socket verifies.
    blocking.drain();
    CHECK(verifier->chains.size() == 1);
    auto const drain = [](core::net::EventLoop* loop, ManualExecutor* executor) -> core::async::Task<void> {
        if (co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; }))
            executor->drain();
    };
    REQUIRE(core::net::testing::blockOnWithin(
        second->loop,
        core::net::testing::allOf(
            handshakeWithin(
                &second->loop, refused->get(), core::net::testing::TlsExchangeBound, &timedOut, &result),
            drain(&second->loop, &blocking)),
        core::net::testing::TlsExchangeBound));
    CHECK(timedOut == false);
    REQUIRE(result.has_value());
    CHECK(result->has_value());
    CHECK(verifier->chains.size() == 2);
}

TEST_CASE("one verifier answers each socket on the loop that socket runs on",
          "[net][tls][truststore][verifier]")
{
    // Two loops, one context: each handshake is driven by its own loop alone, so a verdict delivered
    // to any other executor would never arrive.
    auto first = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto second = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto blocking = ManualExecutor {};
    auto const verifier = std::make_shared<ScriptedChainVerifier>(ChainVerdict::Accept, blocking);
    auto const context = clientOver(verifier);
    auto const drain = [](core::net::EventLoop* loop, ManualExecutor* executor) -> core::async::Task<void> {
        if (co_await core::net::testing::waitUntil(loop, [executor] { return executor->pending() == 1; }))
            executor->drain();
    };
    for (auto* const loopback: { first.get(), second.get() })
    {
        auto socket = context->wrapClient(
            std::move(loopback->transport), loopback->loop, { .serverName = "localhost" });
        REQUIRE(socket.has_value());
        auto timedOut = std::optional<bool> {};
        auto result = std::optional<std::expected<void, core::net::NetError>> {};
        REQUIRE(core::net::testing::blockOnWithin(
            loopback->loop,
            core::net::testing::allOf(
                handshakeWithin(
                    &loopback->loop, socket->get(), core::net::testing::TlsExchangeBound, &timedOut, &result),
                serverHandshake(loopback->peer.get(), loopback->wire.get()),
                drain(&loopback->loop, &blocking)),
            core::net::testing::TlsExchangeBound));
        REQUIRE(result.has_value());
        CHECK(result->has_value());
    }
    CHECK(verifier->chains.size() == 2);
}

TEST_CASE("the system context verifies a public name", "[net][tls][truststore]")
{
    // Construction only, and no network: a context comes back that binds each connection to its
    // name. Where the machine has no trust configuration at all, there is nothing to build it from.
    auto const files = core::platform::NativeFileSystem {};
    auto const environment = core::LiveEnvironment {};
    auto home = ManualExecutor {};
    auto blocking = ManualExecutor {};
    auto task = core::net::makeSystemTlsClientContext(&files, &environment, &home, &blocking, {});
    task.handle().resume();
    blocking.drain();
    home.drain();
    REQUIRE(task.done());
    auto context = task.result();
    if (!context)
    {
        // As the store reads them: a variable set to the empty string is unset.
        auto const isSet = [&environment](std::string_view name) {
            auto const value = environment.get(name);
            return value && !value->empty();
        };
        auto const configured =
            isSet("SSL_CERT_FILE") || isSet("SSL_CERT_DIR")
            || std::ranges::any_of(core::net::trustBundleLocations(), [&files](auto const& row) {
                   return files.exists(std::string { row.path });
               });
        if (!configured)
            SKIP("this machine has no trust configuration: " << context.error());
    }
    REQUIRE(context.has_value());
    REQUIRE(*context != nullptr);

    auto loopback = makeTlsLoopback(StrictTlsPeer::server(material(0)));
    auto const socket = (*context)->wrapClient(
        std::move(loopback->transport), loopback->loop, { .serverName = "example.com" });
    CHECK(socket.has_value());
}
