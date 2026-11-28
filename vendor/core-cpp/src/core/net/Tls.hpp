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

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/NetTlsExport.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
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
[[nodiscard]] CORE_NET_TLS_API std::expected<CertKeyPem, std::string> generateSelfSignedCertificate(
    SelfSignedOptions const& options = {});

/// What a context is built to require, beyond the floor every one shares.
struct TlsOptions
{
    /// The oldest protocol version the context negotiates. @c TlsVersion::Tls13 refuses a peer that
    /// can only speak 1.2.
    TlsVersion minimumVersion = TlsVersion::Tls12;

    /// ALPN (RFC 7301) protocol names, e.g. `"http/1.1"`, each 1 to 255 bytes. A SERVER selects the
    /// first of these, in this order of preference, that the client offered, and refuses a client
    /// that offered only others with the `no_application_protocol` alert; a client that offers none
    /// is served without ALPN. A CLIENT from the pinned-name @c makeTlsClientContext offers them.
    /// Empty: no ALPN.
    std::vector<std::string> alpn {};
};

/// What a verifying client context is built with. **There is no "do not verify" mode**: every
/// connection's peer certificate is checked, and trust on first use remains the pinned-name
/// @c makeTlsClientContext with an empty CA.
struct TlsClientOptions
{
    /// The oldest protocol version the context negotiates, as @c TlsOptions::minimumVersion.
    TlsVersion minimumVersion = TlsVersion::Tls12;

    /// The ALPN protocol names the client offers, in its order of preference, each 1 to 255 bytes;
    /// @c ITlsSession::negotiatedProtocol reports which one the server chose. Empty: no ALPN.
    std::vector<std::string> alpn {};

    /// Over an @c IChainVerifier only: the most verifications the context's sockets have started and
    /// the verifier has not answered yet, across every socket, whether or not anyone still waits for
    /// one. A stop ends a socket's wait, not the evaluation, so this is what bounds the evaluations a
    /// caller can leave running by starting and abandoning handshakes. A handshake past it fails with
    /// @c NetErrorCode::ResourceExhausted, which is not sticky: the socket stays unestablished, and a
    /// handshake driven again once a slot is free asks the verifier then. Zero refuses every one.
    std::size_t maxVerificationsInFlight = 256;
};

/// One certificate, DER-encoded.
using DerCertificate = std::vector<std::byte>;

/// The certificates a client trusts as roots: a chain is accepted when it leads to one of these.
struct TrustAnchors
{
    std::vector<DerCertificate> certificates; ///< The anchors, DER-encoded.
};

/// Reads every certificate out of a PEM bundle as a trust anchor.
/// @param pem One or more PEM certificates.
/// @return The anchors, in the bundle's order; a human-readable reason if it holds none, or a block
///         that does not parse.
[[nodiscard]] CORE_NET_TLS_API std::expected<TrustAnchors, std::string> trustAnchorsFromPem(
    std::string_view pem);

/// Builds a SERVER context from a PEM certificate chain and private key held in memory.
/// @param certPem The leaf certificate followed by any intermediates, in PEM; every certificate in
///        it is served, so a client can build the chain to its trust anchor.
/// @param keyPem The leaf's private key in PEM.
/// @param options The protocol floor.
/// @return The context, or a human-readable reason if the material is invalid or mismatched.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsServerContext(
    std::string_view certPem, std::string_view keyPem, TlsOptions const& options = {});

/// Builds a SERVER context from a PEM certificate chain file and a PEM private key file -- the
/// `--tls-cert` / `--tls-key` shape.
/// @param certPath The certificate chain file.
/// @param keyPath The private key file.
/// @param options The protocol floor.
/// @return The context, or a human-readable reason naming the file that could not be used.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::shared_ptr<ITlsContext>, std::string>
makeTlsServerContextFromFiles(std::filesystem::path const& certPath,
                              std::filesystem::path const& keyPath,
                              TlsOptions const& options = {});

/// Builds a SERVER context over a certificate generated here and now, held in memory only.
///
/// **It buys confidentiality, not identity**: nothing signs the certificate, so a client that has
/// not been told its fingerprint out of band cannot tell this server from anything else answering
/// on its address. @c ITlsContext::certificateFingerprint is what an operator compares.
/// @param options What the certificate names and how long it lasts.
/// @param tlsOptions The protocol floor.
/// @return The context, or a human-readable reason.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::shared_ptr<ITlsContext>, std::string>
makeSelfSignedServerContext(SelfSignedOptions const& options = {}, TlsOptions const& tlsOptions = {});

/// Builds a CLIENT context. When @p caPem is empty the peer certificate is NOT verified (the
/// trust-on-first-use posture for a self-signed server, where something else authenticates and TLS
/// only encrypts); a non-empty @p caPem pins that trust anchor and requires verification.
///
/// @p expectedHostName additionally binds the certificate to the host that was asked for. A chain
/// check alone only proves the certificate was signed by the pinned CA -- NOT that it was issued for
/// this peer, so any certificate that CA ever signed would be accepted for any endpoint. Empty skips
/// the name check (and it is moot without a CA, where nothing is verified).
///
/// The name is checked as @c TlsPeer::serverName describes: an IP literal against the certificate's
/// IP subjectAltNames, a DNS name against its DNS ones by whole labels, never the common name. No
/// SNI is sent: SNI must not carry an IP literal, and a server here presents one certificate per
/// listener.
///
/// **Its contexts bind no per-connection name**: @c ITlsContext::wrapClient refuses a non-empty
/// @c TlsPeer::serverName with @c NetErrorCode::Unsupported rather than drop it, so a connection
/// dialled for a host it cannot check for is never made. The verifying factory below is the one that
/// honours @c TlsPeer.
///
/// @param caPem The trust-anchor certificate in PEM, or empty for trust on first use.
/// @param expectedHostName The host the caller connected to, or empty to skip the name check.
/// @param options The protocol floor, and the ALPN names offered.
/// @return The context, or a human-readable reason if @p caPem is invalid.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    std::string_view caPem = {}, std::string_view expectedHostName = {}, TlsOptions const& options = {});

/// Decides whether a server's certificate chain is good for the name a connection was dialled for:
/// what a platform's own trust evaluation answers (macOS `SecTrustEvaluateWithError`, Windows
/// `CertGetCertificateChain` and the SSL chain policy), so the operating system's trust settings --
/// roots it fetched on demand, certificates an administrator distrusted -- are what decides.
///
/// A context built over one (@c makeTlsClientContext with a @c TrustSource holding it) asks it
/// after the handshake and before any application byte moves: until it has answered, the socket is
/// not established, and every read and write waits.
class IChainVerifier
{
  public:
    IChainVerifier() = default;
    virtual ~IChainVerifier() = default;
    IChainVerifier(IChainVerifier const&) = delete;
    IChainVerifier& operator=(IChainVerifier const&) = delete;
    IChainVerifier(IChainVerifier&&) = delete;
    IChainVerifier& operator=(IChainVerifier&&) = delete;

    /// Verifies a presented chain for a name.
    ///
    /// **It must fail closed**: an empty chain, a certificate that does not parse, and an error
    /// from the platform are each a refusal. It may block, so an implementation runs off the loop,
    /// on its blocking executor, and **finishes there**: the task never resumes on the loop of the
    /// socket that asked, which may be gone by the time the platform answers. The socket's own wait
    /// is what is handed back to its loop.
    ///
    /// **The socket does not wait on it directly.** It runs detached, and the handshake waits for its
    /// answer through a call a stop can abandon: a flow bounded by @c withTimeout or cancelled by its
    /// owner gives up the wait at once, and the evaluation finishes on its own, holding one of the
    /// context's @c TlsClientOptions::maxVerificationsInFlight slots until it answers. A handshake
    /// driven again on the same socket joins the evaluation already running rather than starting a
    /// second. The task is therefore never given a stop token.
    ///
    /// The TLS layer has already checked the leaf's name by RFC 9525's rules when it asks; a
    /// verifier checks it again by its platform's, which can only refuse more.
    /// @param chain The chain as the server sent it, leaf first, DER-encoded.
    /// @param serverName The name to check the leaf against, as the certificate check reads it: a
    ///        DNS name, or an IP literal without brackets or zone; empty to check the chain alone.
    /// @return Nothing if the chain is trusted for @p serverName, or a @c TlsFailure of kind
    ///         @c TlsFailureKind::Certificate saying why not; delivered on the verifier's own
    ///         executor.
    [[nodiscard]] virtual async::Task<std::expected<void, TlsFailure>> verify(
        std::vector<DerCertificate> chain, std::string serverName) = 0;
};

/// What a verifying client trusts: anchors OpenSSL checks the chain against, or a platform verifier
/// that decides for it.
using TrustSource = std::variant<TrustAnchors, std::shared_ptr<IChainVerifier>>;

/// Builds a verifying CLIENT context: every connection's chain is checked against @p trust, and
/// @c ITlsContext::wrapClient binds each connection to its @c TlsPeer.
///
/// **It fails closed.** A DNS @c TlsPeer::serverName is sent as SNI and must be named by a DNS
/// subjectAltName (no partial wildcard: `*` matches one whole leftmost label; the common name is
/// never consulted); an IP literal, bracketed or zoned or neither, sends no SNI and must be an IP
/// subjectAltName; only @c ITlsContext::wrap, which takes no name, verifies the chain alone. An empty
/// name, and a name the wire cannot carry (a NUL, a bracketed name that is no IPv6 literal), is
/// refused by `wrapClient` with @c NetErrorCode::AddressError.
///
/// - **Over @c TrustAnchors**, OpenSSL checks the chain and the name during the handshake. A
///   finished handshake also needs a peer certificate and an OK verification result, and no
///   anonymous or null TLS 1.2 suite is offered.
/// - **Over an @c IChainVerifier**, OpenSSL completes the handshake with its own chain check off.
///   The leaf's NAME is then checked here, by the rules above, so it means the same on every
///   platform (Windows' SSL policy would fall back to the common name); a mismatch fails without
///   asking the verifier. Only then is the verifier asked about the chain the server presented and
///   that name (an IP literal without its brackets and zone). Until it answers, the socket is not
///   established: a read, a write or @c ISocket::handshakeIfNeeded waits, and nothing of the
///   caller's reaches the wire. A refusal, or a server that presented no certificate, fails the
///   handshake. The verifier finishes on its own executor and is shared by every socket of the
///   context; each socket's wait is handed back to the loop that socket runs on.
///
///   **The verification is bounded by nothing here.** It is not a read, so the receive deadline
///   (@c ISocket::setReceiveDeadline) does not cover it, and a platform evaluation that fetches an
///   intermediate or a root can take as long as the network does. A stop does not interrupt an
///   evaluation already running on the blocking executor (@c async::offload reads the stop token
///   once, before the hop), so a caller that needs a bound must keep its own, and expect the
///   evaluation to finish in the background after it gave up.
///
/// Either way an untrusted, expired or mismatched certificate fails the handshake, the failure is
/// sticky, and @c ITlsSession::failure names it @c TlsFailureKind::Certificate.
/// @param trust The anchors, at least one; or the verifier, non-null. @c TrustAnchors converts to
///        it, so a call with anchors reads as one.
/// @param options The protocol floor and the ALPN names offered.
/// @return The context, or a human-readable reason: no anchor, an anchor that does not parse, a null
///         verifier, an ALPN name that is empty or longer than 255 bytes.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::shared_ptr<ITlsContext>, std::string> makeTlsClientContext(
    TrustSource trust, TlsClientOptions const& options = {});

/// Finds the TLS session behind @p socket: @c ISocket::tlsSession.
/// @param socket Any socket.
/// @return The session if @p socket is a TLS socket, or a decorator that forwards one; null for any
///         other (a plain TCP socket). Valid for as long as @p socket lives.
[[nodiscard]] CORE_NET_TLS_API ITlsSession const* tlsSessionOf(ISocket const& socket) noexcept;

/// Spells a SHA-256 digest the way `ssh-keygen -l` does: `SHA256:` and the unpadded base64.
/// @param sha256 The digest, e.g. of an SSH public key blob.
/// @return The text, e.g. `SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU`.
[[nodiscard]] CORE_NET_TLS_API std::string sshStyleFingerprint(std::array<std::byte, 32> const& sha256);

/// The SHA-256 fingerprint of a PEM certificate, as 64 lower-case hex digits -- the same form
/// @c ITlsContext::certificateFingerprint reports, so a client can compare one against the other.
/// @param certPem A certificate in PEM; only the first one is read.
/// @return The fingerprint, or a human-readable reason if @p certPem holds no certificate.
[[nodiscard]] CORE_NET_TLS_API std::expected<std::string, std::string> certificateFingerprint(
    std::string_view certPem);

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
[[nodiscard]] CORE_NET_TLS_API bool constantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept;

} // namespace core::net
