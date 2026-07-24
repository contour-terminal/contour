// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// TLS for the async socket layer: a TLS `ISocket` that decorates any other
/// `ISocket` with OpenSSL, driven entirely through memory BIOs so the handshake
/// and record I/O ride the SAME coroutine reactor as the plaintext transport
/// (no blocking, no extra thread). OpenSSL is reached ONLY through
/// `ITlsContext` — dependency injection — so no OpenSSL type appears above
/// `net/`, and a fake context can be injected in tests.

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <net/ISocket.h>

namespace net
{

/// A configured TLS role: a SERVER context holds the certificate + private key;
/// a CLIENT context holds the peer-verification policy. `wrap()` layers TLS onto
/// an already-connected transport; the handshake runs lazily on first I/O.
class ITlsContext
{
  public:
    virtual ~ITlsContext() = default;

    ITlsContext(ITlsContext const&) = delete;
    ITlsContext& operator=(ITlsContext const&) = delete;
    ITlsContext(ITlsContext&&) = delete;
    ITlsContext& operator=(ITlsContext&&) = delete;

    /// Wraps @p inner in a TLS layer (this context's role). The returned socket
    /// performs the handshake on its first read/write. Null on allocation failure.
    /// @param inner The connected transport to encrypt (owned by the result).
    [[nodiscard]] virtual std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner) = 0;

  protected:
    ITlsContext() = default;
};

/// A self-signed certificate and its matching private key, both PEM-encoded.
struct CertKeyPem
{
    std::string certPem; ///< The X.509 certificate in PEM.
    std::string keyPem;  ///< The private key in PEM (unencrypted).
};

/// Generates a fresh self-signed certificate and key (RSA-2048, 10-year
/// validity, CN=@p commonName) entirely through the OpenSSL library — no
/// `openssl` CLI, so it works identically on Windows and every UNIX. Use it to
/// mint dev certificates for the daemon's `--tls-cert`/`--tls-key`, or as a test
/// fixture; the same material backs `makeSelfSignedServerContext()`.
/// @param commonName The subject/issuer CN to stamp into the certificate.
/// @return The PEM cert+key pair, or a human-readable error on a crypto failure.
[[nodiscard]] std::expected<CertKeyPem, std::string> generateSelfSignedCertificate(
    std::string_view commonName = "contour-daemon");

/// Builds a SERVER TLS context from a PEM certificate (chain) and private key.
/// @param certPem The certificate chain in PEM.
/// @param keyPem The matching private key in PEM.
/// @return The context, or a human-readable error if the material is invalid.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContext(
    std::string_view certPem, std::string_view keyPem);

/// Builds a SERVER TLS context with a freshly generated, in-memory, self-signed
/// certificate (RSA-2048, 10-year validity). This is the daemon's zero-config
/// default: TLS provides confidentiality while the preshared token authenticates
/// (the client trusts on first use / by fingerprint, not a CA).
/// @return The context, or a human-readable error on a crypto failure.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeSelfSignedServerContext();

/// Builds a CLIENT TLS context. When @p caPem is empty the peer certificate is
/// NOT verified against a CA (the TOFU posture for a self-signed daemon cert,
/// where the preshared token authenticates and TLS only encrypts); a non-empty
/// @p caPem pins that trust anchor and requires verification.
/// @param caPem The trust-anchor certificate in PEM, or empty for TOFU.
/// @return The context, or a human-readable error if @p caPem is invalid.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    std::string_view caPem = {});

} // namespace net
