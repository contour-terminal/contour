// SPDX-License-Identifier: Apache-2.0
//
// The Windows chain verifier refuses what it must, without the network: the table in
// ChainVerifierTestSupport.hpp, against CryptoAPI's chain engine and SSL policy.
#include <core/async/IExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

// After <windows.h>, which declares the types it uses; every core-cpp target builds with
// WIN32_LEAN_AND_MEAN, under which <windows.h> does not include it.
#include <wincrypt.h>

#include "../ChainVerifierTestSupport.hpp"
#include "WindowsChainVerifier.hpp"

TEST_CASE("the Windows chain verifier refuses what no trust store vouches for",
          "[net][tls][verifier][platform]")
{
    core::net::testing::requireVerifierRefusals([](core::async::IExecutor& blocking) {
        return core::net::detail::makeWindowsChainVerifier(blocking);
    });
}

TEST_CASE("the Windows chain verifier never follows a certificate's AIA URL",
          "[net][tls][verifier][platform]")
{
    // An intermediate fetched from a URL the server's certificate names is a request to a host the
    // server chose; an incomplete chain is refused instead, as OpenSSL refuses it elsewhere.
    CHECK((core::net::detail::chainBuildFlags() & CERT_CHAIN_DISABLE_AIA) != 0);
}
