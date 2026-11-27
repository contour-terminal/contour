// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The macOS chain verifier: Security.framework's trust evaluation, which honours the keychains'
/// trust settings and never fetches an intermediate a certificate's Authority Information Access
/// names. Private to `core::net_tls`.

#include <core/async/IExecutor.hpp>
#include <core/net/Tls.hpp>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace core::net::detail
{

/// Builds the trust object a verification of @p chain evaluates, and reads back whether it may
/// fetch from the network; for the case that proves it may not.
/// @param chain The presented chain, leaf first.
/// @param serverName The name it is checked for.
/// @return Whether network fetching is allowed, or why the trust object could not be built.
[[nodiscard]] std::expected<bool, std::string> networkFetchAllowed(std::vector<DerCertificate> const& chain,
                                                                   std::string const& serverName);

/// Builds the verifier `SecTrustEvaluateWithError` decides for.
/// @param blocking Where the evaluation runs. Must outlive it.
/// @return The verifier.
[[nodiscard]] std::shared_ptr<IChainVerifier> makeDarwinChainVerifier(async::IExecutor& blocking);

} // namespace core::net::detail
