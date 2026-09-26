// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// TLS for the async socket layer: an @c ISocket that decorates any other with OpenSSL, driven
/// entirely through memory BIOs so the handshake and the record pump ride the SAME event loop as
/// the plaintext transport -- no blocking, no extra thread.
///
/// **No OpenSSL type appears in this header, or in any other.** OpenSSL is linked PRIVATE to
/// `core::net_tls` and reached only through @c ITlsContext, so a consumer compiles against none of
/// its headers and a fake context can be injected in tests. `core-cpp.openssl-seam` is what makes
/// that a property of the tree rather than of this paragraph: it refuses an OpenSSL include
/// outside the units that implement the layer, and an OpenSSL type named in any header.
///
/// One layer, merged from two (Task B11):
/// fastcached's record pump (`FastCache/Net/TlsSocket.cpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`) -- a real half-close, a `waitReadable` that tells a
/// `close_notify` from data, an eager handshake, and an OpenSSL error queue cleared before every
/// call -- and contour's `ITlsContext` seam with client-side verification (a pinned CA and a host
/// name, or trust on first use), its concurrent-reader-and-writer handshake gate and its PEM
/// material functions.
///
/// **Failures are @c NetError values, and the vocabulary is not widened for them.** A record-pump
/// failure is @c NetErrorCode::SystemError with the OpenSSL reason in its context. The peer's
/// `close_notify` is a zero-byte read, exactly as a plaintext socket's EOF; a transport EOF BEFORE
/// one is @c NetErrorCode::ConnReset ("peer closed without close_notify"), because reading it as
/// an end would let a truncated stream pass as a complete one. A crypto-specific error
/// type was considered and rejected: a caller of a socket asks whether the connection works, and a
/// separate type would make every consumer of @c ISocket handle a failure only one transport can
/// produce. The material and context functions below return a human-readable string instead,
/// because what fails there is configuration, not a connection.

#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// A certificate and its matching private key, both PEM-encoded.
struct CertKeyPem
{
    std::string certPem; ///< The X.509 certificate in PEM.
    std::string keyPem;  ///< The private key in PEM (unencrypted).
};

/// What a generated self-signed certificate says, and for how long.
struct SelfSignedOptions
{
    /// How long a generated certificate is valid for when nobody says: ten years.
    ///
    /// Long rather than short, because the two uses pull in the same direction. A certificate
    /// generated in memory at startup is regenerated on every restart, so its expiry is a backstop
    /// rather than a rotation policy; one written out as a development certificate expires into a
    /// TLS failure that looks like a defect long after anyone remembers minting it.
    static constexpr std::chrono::seconds DefaultValidity { std::chrono::days { 3650 } };

    /// The subject and issuer common name. Must not be empty.
    std::string commonName = "localhost";

    /// The names the certificate is valid for, as subjectAltName entries: each is an IP entry when
    /// it parses as an address literal and a DNS entry otherwise. Empty means @c commonName alone.
    ///
    /// **This is what a client actually checks.** Every modern client ignores the common name when
    /// a subjectAltName is present, so a name missing from here matches nothing, however the
    /// certificate is labelled. A name containing a comma is refused: it is the separator of the
    /// list OpenSSL parses, so it would silently add an entry nobody asked for.
    std::vector<std::string> subjectNames {};

    /// How long the certificate is valid for, from now.
    std::chrono::seconds validity = DefaultValidity;
};

/// Generates a fresh self-signed certificate and key (P-256, SHA-256) entirely through the OpenSSL
/// library -- no `openssl` command line, so it behaves identically on Windows and every UNIX.
///
/// An elliptic-curve key rather than RSA because this runs on startup paths: an RSA-2048 keygen is
/// occasionally a second or more, while P-256 is sub-millisecond.
/// @param options What the certificate names and how long it lasts.
/// @return The PEM certificate and key, or a human-readable reason.
[[nodiscard]] std::expected<CertKeyPem, std::string> generateSelfSignedCertificate(
    SelfSignedOptions const& options = {});

/// Builds a SERVER context from a PEM certificate chain and private key held in memory.
/// @param certPem The leaf certificate followed by any intermediates, in PEM; every certificate in
///        it is served, so a client can build the chain to its trust anchor.
/// @param keyPem The leaf's private key in PEM.
/// @return The context, or a human-readable reason if the material is invalid or mismatched.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContext(
    std::string_view certPem, std::string_view keyPem);

/// Builds a SERVER context from a PEM certificate chain file and a PEM private key file -- the
/// `--tls-cert` / `--tls-key` shape.
/// @param certPath The certificate chain file.
/// @param keyPath The private key file.
/// @return The context, or a human-readable reason naming the file that could not be used.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContextFromFiles(
    std::filesystem::path const& certPath, std::filesystem::path const& keyPath);

/// Builds a SERVER context over a certificate generated here and now, held in memory only.
///
/// **It buys confidentiality, not identity**: nothing signs the certificate, so a client that has
/// not been told its fingerprint out of band cannot tell this server from anything else answering
/// on its address. @c ITlsContext::certificateFingerprint is what an operator compares.
/// @param options What the certificate names and how long it lasts.
/// @return The context, or a human-readable reason.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeSelfSignedServerContext(
    SelfSignedOptions const& options = {});

/// Builds a CLIENT context. When @p caPem is empty the peer certificate is NOT verified (the
/// trust-on-first-use posture for a self-signed server, where something else authenticates and TLS
/// only encrypts); a non-empty @p caPem pins that trust anchor and requires verification.
///
/// @p expectedHostName additionally binds the certificate to the host that was asked for. A chain
/// check alone only proves the certificate was signed by the pinned CA -- NOT that it was issued for
/// this peer, so any certificate that CA ever signed would be accepted for any endpoint. Empty skips
/// the name check (and it is moot without a CA, where nothing is verified).
///
/// The name is matched against the certificate's subjectAltName / CN as X509_VERIFY_PARAM does, so
/// an IP-literal host only matches a certificate that carries that IP. No SNI is sent: SNI must not
/// carry an IP literal, and a server here presents one certificate per listener.
///
/// @param caPem The trust-anchor certificate in PEM, or empty for trust on first use.
/// @param expectedHostName The host the caller connected to, or empty to skip the name check.
/// @return The context, or a human-readable reason if @p caPem is invalid.
[[nodiscard]] std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    std::string_view caPem = {}, std::string_view expectedHostName = {});

/// The SHA-256 fingerprint of a PEM certificate, as 64 lower-case hex digits -- the same form
/// @c ITlsContext::certificateFingerprint reports, so a client can compare one against the other.
/// @param certPem A certificate in PEM; only the first one is read.
/// @return The fingerprint, or a human-readable reason if @p certPem holds no certificate.
[[nodiscard]] std::expected<std::string, std::string> certificateFingerprint(std::string_view certPem);

/// Compares two secrets without leaking where they first differ.
///
/// `operator==` on a string returns as soon as it finds a mismatching byte, so the time it takes
/// tells a caller how much of a guessed secret was right -- enough to recover a preshared token
/// byte by byte over many attempts. This compares every byte regardless.
///
/// Lives here because this is where the vetted primitive (`CRYPTO_memcmp`) is reachable: OpenSSL is
/// a PRIVATE dependency of this target, and this signature keeps it that way.
///
/// The LENGTHS are compared normally, which is deliberate and not a leak worth closing: a length
/// difference is already observable in the frame that carried the secret.
///
/// @param lhs One secret.
/// @param rhs The other secret.
/// @return Whether the two are byte-for-byte equal.
[[nodiscard]] bool constantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept;

} // namespace core::net
