// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ITlsContext` — the seam a TLS implementation plugs in behind, and `wrapTls`, the one place an
/// accepted or dialed socket becomes an encrypted one.
///
/// **In `core::net`, not `core::net_tls`, and that is the point of the file.** Nothing here names
/// OpenSSL or needs it, so a consumer's accept path can be written once -- `wrapTls(socket,
/// context)` with a null context in a plaintext configuration -- and compile identically in a build
/// that has no TLS at all. The implementation (`<core/net/Tls.hpp>`, `core::net_tls`) is what needs
/// `CORE_CPP_WITH_TLS`; the seam does not.
///
/// Merged from contour's `net::ITlsContext` and fastcached's `FastCache::WrapTls`
/// (`FastCache/Net/TlsWrap.hpp` at `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), which reached the
/// same shape through a preprocessor switch this seam replaces.

#include <core/async/IExecutor.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>

#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace core::net
{

/// Who one client connection is for: what @c ITlsContext::wrapClient binds the connection to.
struct TlsPeer
{
    /// The host the connection was dialled for, as the caller spelled it: a DNS name, or an IP
    /// literal -- a dotted quad, or IPv6 text with or without the brackets a URL authority puts
    /// around it, and with or without a zone (`fe80::1%eth0`). Brackets around anything but IPv6,
    /// and a zone on anything but IPv6, are refused.
    ///
    /// A DNS name is sent as SNI and the certificate must name it in a DNS subjectAltName; an IP
    /// literal is sent as nothing (RFC 6066 forbids an address in SNI) and the certificate must carry
    /// it, without its zone, as an IP subjectAltName. A verifying context (@c makeTlsClientContext over
    /// a @c TrustSource) refuses an empty one: the chain alone proves nothing about who the peer is.
    std::string serverName;
};

/// A configured TLS role: a SERVER context holds the certificate and private key; a CLIENT context
/// holds the peer-verification policy. `wrap()` layers TLS onto an already-connected transport.
///
/// A socket it returns negotiates lazily on its first read or write, or eagerly through
/// @c ISocket::handshakeIfNeeded, which is what an accept loop awaits before it reads a request.
class ITlsContext
{
  public:
    virtual ~ITlsContext() = default;

    ITlsContext(ITlsContext const&) = delete;
    ITlsContext& operator=(ITlsContext const&) = delete;
    ITlsContext(ITlsContext&&) = delete;
    ITlsContext& operator=(ITlsContext&&) = delete;

    /// Wraps @p inner in a TLS layer of this context's role.
    ///
    /// **@p executor is the loop the socket's own waits are resumed on.** A TLS socket parks an
    /// operation on itself -- a read waiting while a write drives the handshake, a write waiting for
    /// a flush in progress -- and a waiter is never resumed inline by whatever releases it
    /// (`.agent/rules/async-and-net.md`), so the socket needs somewhere to hand it. Pass the loop
    /// @p inner belongs to.
    /// @param inner The connected transport to encrypt (owned by the result).
    /// @param executor Where the socket resumes its waiters; must outlive the returned socket and
    ///        every operation on it.
    /// @return The TLS socket, or null when it could not be allocated.
    [[nodiscard]] virtual std::unique_ptr<ISocket> wrap(std::unique_ptr<ISocket> inner,
                                                        async::IExecutor& executor) = 0;

    /// Wraps @p inner as a CLIENT connection to @p peer: SNI and the certificate check bound to the
    /// one name this connection was dialled for.
    ///
    /// **A context never discards a requested name.** The default -- for a context with no
    /// per-connection check, a server context or one written before this member existed -- refuses
    /// a non-empty @c TlsPeer::serverName with @c NetErrorCode::Unsupported, because wrapping without
    /// it would hand back a connection that is not verified for the host asked for. With an empty
    /// name it is @c wrap. A context that does bind the name refuses one the wire cannot carry (an
    /// embedded NUL, brackets around something that is no IPv6 literal), and an empty one, with
    /// @c NetErrorCode::AddressError.
    /// @param inner The connected transport to encrypt (owned by the result).
    /// @param executor As for @c wrap.
    /// @param peer Who the connection is for.
    /// @return The TLS socket, negotiating lazily as @c wrap's does; @c NetErrorCode::Unsupported for
    ///         a name this context cannot verify; @c NetErrorCode::ResourceExhausted when the socket
    ///         could not be allocated.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ISocket>, NetError> wrapClient(
        std::unique_ptr<ISocket> inner, async::IExecutor& executor, TlsPeer const& peer)
    {
        if (!peer.serverName.empty())
            return std::unexpected(makeNetError(
                NetErrorCode::Unsupported, 0, "this TLS context cannot verify a per-connection server name"));
        auto socket = wrap(std::move(inner), executor);
        if (socket == nullptr)
            return std::unexpected(
                makeNetError(NetErrorCode::ResourceExhausted, 0, "the TLS socket could not be allocated"));
        return socket;
    }

    /// The SHA-256 fingerprint of the certificate this context presents, as 64 lower-case hex
    /// digits.
    ///
    /// **The only thing that authenticates a self-signed certificate**, since nothing signs it: an
    /// operator compares what the server reported against what the client saw, and a client that
    /// trusts on first use pins it.
    /// @return The fingerprint, or empty for a context that presents no certificate (a client).
    [[nodiscard]] virtual std::string certificateFingerprint() const { return {}; }

  protected:
    ITlsContext() = default;
};

/// Wraps @p socket in TLS when a context is configured, and hands it back unchanged when not.
///
/// The single place a connection becomes encrypted, so every accept and dial path shares it rather
/// than each re-deriving the "is TLS on?" question.
/// @param socket The connected transport (owned).
/// @param context The TLS context, or null for plaintext.
/// @param executor The loop @p socket belongs to, as @c ITlsContext::wrap takes it.
/// @return @p socket, wrapped when @p context is non-null; null only when wrapping could not
///         allocate, as @c ITlsContext::wrap documents.
[[nodiscard]] inline std::unique_ptr<ISocket> wrapTls(std::unique_ptr<ISocket> socket,
                                                      ITlsContext* context,
                                                      async::IExecutor& executor)
{
    if (context == nullptr)
        return socket;
    return context->wrap(std::move(socket), executor);
}

} // namespace core::net
