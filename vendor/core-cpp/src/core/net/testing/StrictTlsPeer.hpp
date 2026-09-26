// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `StrictTlsPeer` — a TLS endpoint driven by hand through OpenSSL, to hold a TLS socket against:
/// the test double of `core::net_tls`, as `ScriptedBackend` is the loop's.
///
/// **An oracle that is not the code under test.** A `TlsSocket` checked only against another
/// `TlsSocket` agrees with itself: a defect both ends share, in writing the alert or in reading it,
/// passes. This peer is OpenSSL alone, and it reads the way OpenSSL 3 reads from a
/// socket by default -- a transport EOF before `close_notify` is an error, not an end -- which is
/// what a strict client on the other side of a real connection does.
///
/// No OpenSSL type crosses this header either: the peer's state is behind a pointer to an
/// incomplete type, and its answers are strings and counts. `core-cpp.openssl-seam` holds this
/// header to that like any other; `StrictTlsPeer.cpp` is one of the units permitted to include
/// OpenSSL.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Tls.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace core::net::testing
{

/// How a peer's stream ended, as a strict reader sees it.
enum class StreamEnd : std::uint8_t
{
    CloseNotify, ///< A `close_notify` alert: the orderly end of a TLS stream.
    Truncated,   ///< A transport EOF with no `close_notify` before it: what a truncation looks like.
    Failed,      ///< Anything else went wrong.
};

/// One TLS endpoint over the far end of a socket pair, pumped by hand.
class StrictTlsPeer
{
  public:
    /// A client that verifies nothing (the cases check identity by asking, not by failing).
    /// @return The peer, or why OpenSSL could not build it.
    [[nodiscard]] static std::expected<std::unique_ptr<StrictTlsPeer>, std::string> client();

    /// A server presenting @p material.
    /// @param material The certificate chain and key to serve.
    /// @return The peer, or why OpenSSL refused the material.
    [[nodiscard]] static std::expected<std::unique_ptr<StrictTlsPeer>, std::string> server(
        CertKeyPem const& material);

    ~StrictTlsPeer();
    StrictTlsPeer(StrictTlsPeer const&) = delete;
    StrictTlsPeer& operator=(StrictTlsPeer const&) = delete;
    StrictTlsPeer(StrictTlsPeer&&) = delete;
    StrictTlsPeer& operator=(StrictTlsPeer&&) = delete;

    /// Runs the handshake to completion over @p wire.
    /// @param wire The plaintext socket this peer's ciphertext travels on; must outlive the task.
    /// @return Nothing, or OpenSSL's reason.
    [[nodiscard]] async::Task<std::expected<void, std::string>> handshake(ISocket* wire);

    /// Reads application data until the stream ends, and says HOW it ended.
    /// @param wire As for @c handshake.
    /// @param out Receives every plaintext byte; must outlive the task.
    /// @return The kind of end.
    [[nodiscard]] async::Task<StreamEnd> readToEnd(ISocket* wire, std::string* out);

    /// Reads until at least @p count plaintext bytes have arrived.
    /// @param wire As for @c handshake.
    /// @param count How many bytes to wait for.
    /// @return The bytes, or why the stream ended first.
    [[nodiscard]] async::Task<std::expected<std::string, std::string>> readAtLeast(ISocket* wire,
                                                                                   std::size_t count);

    /// Encrypts @p text as application data and puts it on the wire.
    /// @param wire As for @c handshake.
    /// @param text The plaintext.
    /// @return Whether every byte was sent.
    [[nodiscard]] async::Task<bool> write(ISocket* wire, std::string text);

    /// Sends `close_notify` -- the first half of an orderly close -- and puts it on the wire.
    /// @param wire As for @c handshake.
    /// @return Whether the alert was sent.
    [[nodiscard]] async::Task<bool> sendCloseNotify(ISocket* wire);

    /// @return Whether this peer's handshake has completed.
    [[nodiscard]] bool handshakeFinished() const noexcept;

    /// @return The common name of the certificate the other end presented, or empty.
    [[nodiscard]] std::string peerCommonName() const;

    /// @return The SHA-256 fingerprint of that certificate, as @c certificateFingerprint spells it.
    [[nodiscard]] std::string peerFingerprint() const;

    /// @return How many certificates the other end sent, leaf included.
    [[nodiscard]] int peerChainLength() const;

  private:
    struct State;
    explicit StrictTlsPeer(std::unique_ptr<State> state) noexcept;

    /// Moves everything OpenSSL queued outbound onto @p wire.
    [[nodiscard]] async::Task<bool> flush(ISocket* wire);

    /// Reads one chunk of ciphertext from @p wire into OpenSSL; at EOF, tells OpenSSL so.
    /// @return The bytes fed; 0 at EOF, and 0 on a wire error, which it records apart.
    [[nodiscard]] async::Task<std::size_t> feed(ISocket* wire);

    std::unique_ptr<State> _state;
};

/// @param certPem A PEM certificate.
/// @return Its subject's common name, or empty.
[[nodiscard]] std::string certificateCommonName(std::string_view certPem);

/// @param certPem A PEM certificate.
/// @param host A DNS name.
/// @return Whether a client checking @p host would accept the certificate for it.
[[nodiscard]] bool certificateMatchesHost(std::string_view certPem, std::string_view host);

/// @param certPem A PEM certificate.
/// @param address An IP address literal.
/// @return Whether a client checking @p address would accept the certificate for it.
[[nodiscard]] bool certificateMatchesIp(std::string_view certPem, std::string_view address);

/// Leaves an error on the calling thread's OpenSSL error queue, as another connection on the same
/// loop thread might.
void leaveStaleOpensslError();

} // namespace core::net::testing
