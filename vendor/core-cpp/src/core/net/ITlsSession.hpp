// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ITlsSession` -- what a finished TLS handshake lets a caller ask of its connection, and
/// `TlsVersion`, the protocol floor a context is built with.
///
/// **In `core::net` beside `ITlsContext`, with no OpenSSL in sight**, so code that authenticates a
/// user over a TLS connection compiles in a build that has no TLS. The implementation is the socket
/// `core::net_tls` returns; `tlsSessionOf` (`<core/net/Tls.hpp>`) finds the session behind an
/// `ISocket`.

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

/// The facts about one TLS connection that are only known once its handshake has finished.
///
/// Every query is valid after the handshake and answers an error (or empty) before it.
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
    /// @return The 32 bytes, or why they are not available (no handshake yet, a session below TLS 1.3,
    ///         or the export failed).
    [[nodiscard]] virtual std::expected<std::array<std::byte, 32>, std::string> channelBinding() const = 0;

    /// @return The SHA-256 of the peer's DER certificate, raw, so a caller may format it as it
    ///         likes (@c sshStyleFingerprint takes it as it is); no value if the handshake has not
    ///         finished or the peer presented no certificate.
    [[nodiscard]] virtual std::optional<std::array<std::byte, 32>> peerCertificateDigest() const = 0;

    /// @return The protocol version the handshake settled on; no value before the handshake has
    ///         finished, when nothing has been negotiated yet.
    [[nodiscard]] virtual std::optional<TlsVersion> negotiatedVersion() const = 0;

  protected:
    ITlsSession() = default;
};

} // namespace core::net
