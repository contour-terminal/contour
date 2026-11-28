// SPDX-License-Identifier: Apache-2.0
//
// The macOS chain verifier refuses what it must, without the network: the table in
// ChainVerifierTestSupport.hpp, against Security.framework's trust evaluation.
#include <core/async/IExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../ChainVerifierTestSupport.hpp"
#include "DarwinChainVerifier.hpp"

TEST_CASE("the macOS chain verifier refuses what no trust store vouches for",
          "[net][tls][verifier][platform]")
{
    core::net::testing::requireVerifierRefusals([](core::async::IExecutor& blocking) {
        return core::net::detail::makeDarwinChainVerifier(blocking);
    });
}

TEST_CASE("the macOS chain verifier never follows a certificate's AIA URL", "[net][tls][verifier][platform]")
{
    // An intermediate fetched from a URL the server's certificate names is a request to a host the
    // server chose; an incomplete chain is refused instead, as OpenSSL refuses it elsewhere.
    auto const allowed =
        core::net::detail::networkFetchAllowed({ core::net::testing::untrustedLocalhost() }, "localhost");
    REQUIRE(allowed.has_value());
    CHECK_FALSE(*allowed);
}
