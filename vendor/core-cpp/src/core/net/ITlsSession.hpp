// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ITlsSession` -- what a TLS handshake lets a caller ask of its connection, `TlsFailure`, how one
/// that failed says why, and `TlsVersion`, the protocol floor a context is built with.
///
/// **In `core::net` beside `ITlsContext`, with no OpenSSL in sight**, so code that authenticates a
/// user over a TLS connection, or maps a TLS failure to a protocol error, compiles in a build that
/// has no TLS. The implementation is the socket `core::net_tls` returns; `ISocket::tlsSession()`
/// finds the session behind an `ISocket`, through any decorator that forwards it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace core::net
{

/// A TLS protocol version a context can be told to require at least.
enum class TlsVersion : std::uint8_t
{
    Tls12 = 0, ///< TLS 1.2 -- the floor every context has always had.
    Tls13,     ///< TLS 1.3.
};

/// What kind of failure ended a TLS connection.
enum class TlsFailureKind : std::uint8_t
{
    Protocol = 0,  ///< The TLS layer failed for any reason the two below do not name.
    Certificate,   ///< The peer's certificate was refused: an untrusted chain, an expired or not yet
                   ///< valid certificate, or one that does not name the host or address dialled.
    AlertReceived, ///< The peer ended the handshake or the connection with a fatal alert.
};

/// Why a TLS connection failed, in the shape `wasi:http`'s `TLS-*` error cases carry it.
struct TlsFailure
{
    TlsFailureKind kind = TlsFailureKind::Protocol; ///< What failed.
    std::optional<std::uint8_t> alert;              ///< The alert's description byte (RFC 8446
                                                    ///< section 6), for @c TlsFailureKind::AlertReceived.
    std::string message;                            ///< A human-readable reason; may be empty.
};

/// The facts about one TLS connection that are only known once its handshake has run.
///
/// Every query but @c failure answers for an ESTABLISHED session only, and an error (or empty)
/// otherwise. A session whose handshake finished but whose chain a verifier has not yet accepted,
/// or refused, answers as one that never finished: a caller cannot read a refused peer's identity,
/// or bind to its channel, by asking before it checks @c failure.
class ITlsSession
{
  public:
    virtual ~ITlsSession() = default;

    ITlsSession(ITlsSession const&) = delete;
    ITlsSession& operator=(ITlsSession const&) = delete;
    ITlsSession(ITlsSession&&) = delete;
    ITlsSession& operator=(ITlsSession&&) = delete;

    /// The RFC 9266 channel binding: `SSL_export_keying_material` with the label
    /// `"EXPORTER-Channel-Binding"`, 32 bytes and no context. Both ends of one session derive the
    /// same value, and no other session does, so a signature over it cannot be replayed on another
    /// connection.
    ///
    /// Only a TLS 1.3 session has one: RFC 9266 defines tls-exporter for TLS 1.3, and under TLS 1.2
    /// the exporter is not bound to the whole handshake (without extended master secret), so a 1.2
    /// session answers an error rather than a binding that binds less than it claims.
    /// @return The 32 bytes, or why they are not available (a session not established -- its handshake
    ///         unfinished, its chain still being verified, or refused -- a session below TLS 1.3, or
    ///         the export failed).
    [[nodiscard]] virtual std::expected<std::array<std::byte, 32>, std::string> channelBinding() const = 0;

    /// @return The SHA-256 of the peer's DER certificate, raw, so a caller may format it as it
    ///         likes (@c sshStyleFingerprint takes it as it is); no value until the session is
    ///         established (a refused peer's certificate is not reported), or if the peer presented
    ///         none.
    [[nodiscard]] virtual std::optional<std::array<std::byte, 32>> peerCertificateDigest() const = 0;

    /// @return The protocol version the handshake settled on; no value until the session is
    ///         established -- before the handshake has finished, while its chain is being verified,
    ///         and after it was refused.
    [[nodiscard]] virtual std::optional<TlsVersion> negotiatedVersion() const = 0;

    /// Why the TLS layer failed, once it has: the first failure of the handshake, or of a record the
    /// peer sent after it. A failure of the transport under TLS (a reset, an end before the handshake
    /// finished) is no TLS failure and answers nothing here; the operation's @c NetError says it.
    ///
    /// **Fails closed is what makes @c TlsFailureKind::Certificate mean something**: a socket that
    /// verifies its peer refuses every certificate the check rejects, so this kind is reported for
    /// exactly the connections that check ended.
    /// @return The failure, or no value while the connection works. The default never fails, for a
    ///         session that cannot.
    [[nodiscard]] virtual std::optional<TlsFailure> failure() const { return std::nullopt; }

    /// @return The application protocol ALPN (RFC 7301) settled on, e.g. `"http/1.1"`; empty until the
    ///         session is established (as @c negotiatedVersion), and when either end offered none.
    ///         The default is empty.
    [[nodiscard]] virtual std::string negotiatedProtocol() const { return {}; }

  protected:
    ITlsSession() = default;
};

} // namespace core::net
