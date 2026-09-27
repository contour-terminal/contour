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

#include <memory>
#include <string>
#include <utility>

namespace core::net
{

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
