// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The refusals every platform `IChainVerifier` must make, as one table, and the case that runs it.
///
/// A verifier answering `{}` for everything would pass every TLS case that uses the scripted one,
/// so the real verifiers are held to this directly. None of the rows needs the network: an empty
/// chain, bytes that are no certificate, and a self-signed certificate no trust store holds, asked
/// for its own name, for no name and for another name. Each must be refused as a certificate
/// failure, from the blocking executor.
///
/// Test-only, beside the tests, because it needs Catch2: it is compiled into no module.

#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/Tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::net::testing
{

/// What a refusal row presents.
enum class PresentedChain : std::uint8_t
{
    Empty = 0,  ///< No certificate at all.
    Garbage,    ///< Bytes that start like DER and are no certificate.
    SelfSigned, ///< A certificate for `localhost` that no trust store holds.
};

/// One chain a platform verifier must refuse.
struct VerifierRefusal
{
    std::string_view what; ///< The row, as a section name.
    PresentedChain chain;  ///< What is presented.
    std::string_view name; ///< The name it is asked for.
};

/// The refusals, in the order they are run.
inline constexpr auto VerifierRefusals = std::array {
    VerifierRefusal { .what = "an empty chain", .chain = PresentedChain::Empty, .name = "localhost" },
    VerifierRefusal { .what = "garbage DER", .chain = PresentedChain::Garbage, .name = "localhost" },
    VerifierRefusal { .what = "an untrusted self-signed certificate, its own name",
                      .chain = PresentedChain::SelfSigned,
                      .name = "localhost" },
    VerifierRefusal { .what = "an untrusted self-signed certificate, no name",
                      .chain = PresentedChain::SelfSigned,
                      .name = "" },
    VerifierRefusal { .what = "an untrusted self-signed certificate, another name",
                      .chain = PresentedChain::SelfSigned,
                      .name = "other.test" },
};

/// A self-signed certificate for `localhost`, DER-encoded; generated once.
[[nodiscard]] inline DerCertificate const& untrustedLocalhost()
{
    static auto const der = [] {
        auto material =
            generateSelfSignedCertificate({ .commonName = "localhost", .subjectNames = { "localhost" } });
        REQUIRE(material.has_value());
        auto anchors = trustAnchorsFromPem(material->certPem);
        REQUIRE(anchors.has_value());
        REQUIRE(anchors->certificates.size() == 1);
        return anchors->certificates.front();
    }();
    return der;
}

/// @return The chain @p kind names.
[[nodiscard]] inline std::vector<DerCertificate> presented(PresentedChain kind)
{
    switch (kind)
    {
        case PresentedChain::Empty: return {};
        case PresentedChain::Garbage: return { DerCertificate(16, std::byte { 0x30 }) };
        case PresentedChain::SelfSigned: return { untrustedLocalhost() };
    }
    return {};
}

/// Runs every row of @c VerifierRefusals against the verifier @p make builds, each in its own
/// section: the verification must park on the blocking executor, and answer a certificate failure
/// with a reason.
/// @param make Builds the verifier under test from a blocking executor.
template <typename MakeVerifier>
void requireVerifierRefusals(MakeVerifier make)
{
    for (auto const& row: VerifierRefusals)
    {
        DYNAMIC_SECTION(row.what)
        {
            auto blocking = async::testing::ManualExecutor {};
            std::shared_ptr<IChainVerifier> const verifier = make(blocking);
            REQUIRE(verifier != nullptr);
            auto task = verifier->verify(presented(row.chain), std::string { row.name });
            task.handle().resume();
            CHECK(blocking.pending() == 1);
            blocking.drain();
            // Finished on the blocking executor: nothing is handed anywhere else.
            REQUIRE(task.done());
            auto const verdict = task.result();
            REQUIRE_FALSE(verdict.has_value());
            CHECK(verdict.error().kind == TlsFailureKind::Certificate);
            CHECK_FALSE(verdict.error().message.empty());
        }
    }
}

} // namespace core::net::testing
