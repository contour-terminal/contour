// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows chain verifier: `CertGetCertificateChain` and the SSL chain policy, which honour the
/// Disallowed store and Automatic Root Update, and never fetch an intermediate a certificate's
/// Authority Information Access names. Private to `core::net_tls`.

#include <core/async/IExecutor.hpp>
#include <core/net/Tls.hpp>

#include <cstdint>
#include <memory>

namespace core::net::detail
{

/// @return The flags every chain is built with: `CERT_CHAIN_DISABLE_AIA`, so the engine fetches no
///         intermediate from a URL the server's certificate names.
[[nodiscard]] std::uint32_t chainBuildFlags() noexcept;

/// Builds the verifier `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)` decides for.
/// @param blocking Where the evaluation runs. Must outlive it.
/// @return The verifier.
[[nodiscard]] std::shared_ptr<IChainVerifier> makeWindowsChainVerifier(async::IExecutor& blocking);

} // namespace core::net::detail
