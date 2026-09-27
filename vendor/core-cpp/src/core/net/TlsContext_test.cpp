// SPDX-License-Identifier: Apache-2.0
//
// The TLS contexts and the material behind them: generated certificates, named ones loaded from
// PEM or from files, fingerprints, and the seam every accept path goes through.
//
// Ported from fastcached's `Net/TlsContext_test.cpp` (0708dd54) onto core-cpp's API. Upstream loads
// a certificate checked in under `testdata/tls/`; core-cpp generates every certificate it tests at
// run time, because a committed certificate EXPIRES, and it expires into a test failure that looks
// like a TLS defect years after anyone remembers the fixture exists. `generateSelfSignedCertificate`
// is therefore the one fixture every TLS case shares.
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/StrictTlsPeer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::SelfSignedOptions;
using core::net::testing::StrictTlsPeer;

namespace
{

/// A directory of its own under the system's temporary directory, removed with this object.
class ScratchDirectory
{
  public:
    ScratchDirectory():
        _path(std::filesystem::temp_directory_path()
              / ("core-cpp-tls-" + std::to_string(std::random_device {}()) + "-"
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(_path);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(_path, ignored);
    }

    /// Writes @p contents to a file named @p name here.
    /// @return The file's path.
    [[nodiscard]] std::filesystem::path write(std::string_view name, std::string_view contents) const
    {
        auto const file = _path / name;
        auto stream = std::ofstream { file, std::ios::binary };
        stream << contents;
        return file;
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    std::filesystem::path _path;
};

/// Handshakes @p context's socket against a strict client and reports what that client saw.
struct PeerView
{
    bool handshaken = false;
    std::string commonName;
    std::string fingerprint;
    int chainLength = 0;
};

PeerView presentTo(core::net::ITlsContext& context)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());
    auto server = context.wrap(std::move(pair->first), loop);
    REQUIRE(server != nullptr);
    auto peer = StrictTlsPeer::client();
    REQUIRE(peer.has_value());

    auto const serverSide = [](core::net::ISocket* tls) -> Task<void> {
        std::ignore = co_await tls->handshakeIfNeeded();
    };
    auto const clientSide = [](StrictTlsPeer* p, core::net::ISocket* wire, bool* ok) -> Task<void> {
        *ok = (co_await p->handshake(wire)).has_value();
    };
    auto view = PeerView {};
    auto const finished = loop.blockOn(core::net::withTimeout(
        &loop,
        core::net::testing::allOf(serverSide(server.get()),
                                  clientSide(peer->get(), pair->second.get(), &view.handshaken)),
        std::chrono::milliseconds { 10'000 }));
    REQUIRE(finished);
    view.commonName = (*peer)->peerCommonName();
    view.fingerprint = (*peer)->peerFingerprint();
    view.chainLength = (*peer)->peerChainLength();
    return view;
}

[[nodiscard]] bool isLowerHex(std::string_view text) noexcept
{
    return text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

} // namespace

TEST_CASE("makeSelfSignedServerContext puts the requested common name in the certificate",
          "[net][tls][context]")
{
    auto const context =
        core::net::makeSelfSignedServerContext(SelfSignedOptions { .commonName = "core-test" });
    REQUIRE(context.has_value());

    auto const seen = presentTo(**context);
    REQUIRE(seen.handshaken);
    CHECK(seen.commonName == "core-test");
}

TEST_CASE("A generated certificate is valid for the names it was given, and no others", "[net][tls][context]")
{
    // The names are what a client actually checks: every modern client ignores the common name
    // when a subjectAltName is present, so a name missing there matches nothing.
    auto const material = core::net::generateSelfSignedCertificate(
        SelfSignedOptions { .commonName = "core-test",
                            .subjectNames = { "localhost", "127.0.0.1", "::1", "buildnode-3.internal" } });
    REQUIRE(material.has_value());
    auto const& pem = material->certPem;

    CHECK(core::net::testing::certificateMatchesHost(pem, "localhost"));
    CHECK(core::net::testing::certificateMatchesHost(pem, "buildnode-3.internal"));
    CHECK(core::net::testing::certificateMatchesIp(pem, "127.0.0.1"));
    CHECK(core::net::testing::certificateMatchesIp(pem, "::1"));
    CHECK_FALSE(core::net::testing::certificateMatchesHost(pem, "elsewhere.internal"));
    CHECK(core::net::testing::certificateCommonName(pem) == "core-test");
}

TEST_CASE("With no subject names, a certificate is valid for its common name", "[net][tls][context]")
{
    auto const material =
        core::net::generateSelfSignedCertificate(SelfSignedOptions { .commonName = "the-daemon" });
    REQUIRE(material.has_value());
    CHECK(core::net::testing::certificateMatchesHost(material->certPem, "the-daemon"));
    CHECK_FALSE(core::net::testing::certificateMatchesHost(material->certPem, "localhost"));
}

TEST_CASE("A subject name is classified by what it parses as, not by how it looks", "[net][tls][context]")
{
    // Guessing from the spelling -- a digit at the front, a colon somewhere -- gets a host called
    // `10things` and the address `2001:db8::1` wrong in opposite directions.
    auto const material = core::net::generateSelfSignedCertificate(
        SelfSignedOptions { .commonName = "core-test", .subjectNames = { "10things", "2001:db8::1" } });
    REQUIRE(material.has_value());
    CHECK(core::net::testing::certificateMatchesHost(material->certPem, "10things"));
    CHECK(core::net::testing::certificateMatchesIp(material->certPem, "2001:db8::1"));
}

TEST_CASE("Self-signed options that name nothing coherent are refused", "[net][tls][context]")
{
    SECTION("an empty common name")
    {
        auto const refused = core::net::generateSelfSignedCertificate(SelfSignedOptions { .commonName = "" });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("common name"));
    }
    SECTION("only empty subject names")
    {
        auto const refused = core::net::generateSelfSignedCertificate(
            SelfSignedOptions { .commonName = "core-test", .subjectNames = { "" } });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("subject name"));
    }
    SECTION("a subject name carrying the list separator")
    {
        // The names are pasted into OpenSSL's comma-separated subjectAltName grammar, so a comma
        // inside one silently adds a name nobody asked for.
        auto const refused = core::net::generateSelfSignedCertificate(SelfSignedOptions {
            .commonName = "core-test", .subjectNames = { "localhost", "a,DNS:internal.example.com" } });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("comma"));
    }
    SECTION("a colon is NOT refused: it is what an IPv6 literal is made of")
    {
        CHECK(
            core::net::generateSelfSignedCertificate(SelfSignedOptions { .commonName = "::1" }).has_value());
    }
}

TEST_CASE("A context reports the fingerprint a client sees, and a fresh one per certificate",
          "[net][tls][context]")
{
    auto const material = core::net::generateSelfSignedCertificate();
    REQUIRE(material.has_value());
    auto const context = core::net::makeTlsServerContext(material->certPem, material->keyPem);
    REQUIRE(context.has_value());

    auto const reported = (*context)->certificateFingerprint();
    CHECK(reported.size() == 64); // SHA-256, lower-case hex
    CHECK(isLowerHex(reported));
    CHECK(core::net::certificateFingerprint(material->certPem) == reported);
    CHECK(presentTo(**context).fingerprint == reported);

    auto const other = core::net::makeSelfSignedServerContext();
    REQUIRE(other.has_value());
    CHECK((*other)->certificateFingerprint() != reported);

    auto const client = core::net::makeTlsClientContext();
    REQUIRE(client.has_value());
    CHECK((*client)->certificateFingerprint().empty()); // presents nothing

    CHECK_FALSE(core::net::certificateFingerprint("not a certificate").has_value());
}

TEST_CASE("makeTlsServerContextFromFiles loads a chain and refuses what it cannot use", "[net][tls][context]")
{
    auto const scratch = ScratchDirectory {};
    auto const material =
        core::net::generateSelfSignedCertificate(SelfSignedOptions { .commonName = "from-files" });
    REQUIRE(material.has_value());
    auto const certFile = scratch.write("server.crt", material->certPem);
    auto const keyFile = scratch.write("server.key", material->keyPem);

    SECTION("a certificate and its key")
    {
        auto const context = core::net::makeTlsServerContextFromFiles(certFile, keyFile);
        REQUIRE(context.has_value());
        CHECK(presentTo(**context).commonName == "from-files");
    }
    SECTION("a missing file is named")
    {
        auto const refused = core::net::makeTlsServerContextFromFiles(scratch.path() / "absent.crt", keyFile);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("absent.crt"));
    }
    SECTION("a certificate where the key belongs")
    {
        CHECK_FALSE(core::net::makeTlsServerContextFromFiles(certFile, certFile).has_value());
    }
    SECTION("a key that belongs to another certificate")
    {
        auto const stranger = core::net::generateSelfSignedCertificate();
        REQUIRE(stranger.has_value());
        auto const strangerKey = scratch.write("stranger.key", stranger->keyPem);
        auto const refused = core::net::makeTlsServerContextFromFiles(certFile, strangerKey);
        REQUIRE_FALSE(refused.has_value());
        // OpenSSL refuses it at SSL_CTX_use_PrivateKey ("key values mismatch") before the explicit
        // check runs, so the reason names the key rather than one fixed phrase.
        CHECK(refused.error().contains("key"));
    }
    SECTION("every certificate in the file is served, not only the first")
    {
        // A named certificate arrives with its intermediates after it. Serving only the first left
        // a client unable to build the chain to its anchor, failing a handshake the operator had
        // configured correctly.
        auto const intermediate =
            core::net::generateSelfSignedCertificate(SelfSignedOptions { .commonName = "ca" });
        REQUIRE(intermediate.has_value());
        auto const chainFile = scratch.write("chain.crt", material->certPem + intermediate->certPem);
        auto const context = core::net::makeTlsServerContextFromFiles(chainFile, keyFile);
        REQUIRE(context.has_value());
        CHECK(presentTo(**context).chainLength == 2);
    }
}

TEST_CASE("wrapTls encrypts only when a context is configured", "[net][tls][context]")
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto* const plain = pair->first.get();
    auto unchanged = core::net::wrapTls(std::move(pair->first), nullptr, loop);
    CHECK(unchanged.get() == plain); // plaintext: the very same socket

    auto const context = core::net::makeSelfSignedServerContext();
    REQUIRE(context.has_value());
    auto* const raw = pair->second.get();
    auto wrapped = core::net::wrapTls(std::move(pair->second), context->get(), loop);
    REQUIRE(wrapped != nullptr);
    CHECK(wrapped.get() != raw);
}

TEST_CASE("constantTimeEquals compares every byte and the length", "[net][tls][context]")
{
    CHECK(core::net::constantTimeEquals("", ""));
    CHECK(core::net::constantTimeEquals("secret-token", "secret-token"));
    CHECK_FALSE(core::net::constantTimeEquals("secret-token", "secret-tokeN"));
    CHECK_FALSE(core::net::constantTimeEquals("Secret-token", "secret-token"));
    CHECK_FALSE(core::net::constantTimeEquals("secret", "secret-token"));
    CHECK_FALSE(core::net::constantTimeEquals("", "x"));
}
