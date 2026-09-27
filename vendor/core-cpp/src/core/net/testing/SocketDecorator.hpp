// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SocketDecorator` — an @c ISocket that forwards every operation to another one, so a test double
/// overrides the one verb it stages and inherits correct forwarding for the rest.
///
/// Origin: fastcached `src/tests/SocketDecorator.hpp` (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace core::net::testing
{

/// An @c ISocket that forwards every operation to another one.
///
/// A test that stages a socket CONDITION -- a read that parks, a write that stalls -- wants to change
/// one method and leave the rest alone. Written by hand each time, that is a dozen forwarding bodies
/// per double, of which all but one are noise, and the copies then diverge in the part nobody
/// looked at.
///
/// **What hand-written copies get wrong is the part they did not write.** @c ISocket has virtuals
/// with default implementations -- @c readWithFd, @c handshakeIfNeeded, @c waitReadable,
/// @c cancelRead, @c shutdownWrite, @c setReceiveDeadline, @c peerAddress -- and a decorator that
/// overrides none of them silently answers from the BASE rather than from the socket it decorates:
/// a decorated peer has no address, a half-close reaches nothing, a deadline bounds nothing. None of
/// that is visible at a call site, because every one of those defaults succeeds. Forwarding them is a
/// property of this type, so a double cannot omit it -- and when @c ISocket grows a virtual, this is
/// the one place that must grow with it.
class SocketDecorator: public ISocket
{
  public:
    /// @param inner The socket every operation is forwarded to; must outlive this.
    explicit SocketDecorator(ISocket& inner) noexcept: _inner { inner } {}

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override { return _inner.read(buffer); }

    [[nodiscard]] ResultAwaitable<ReadWithFd> readWithFd(std::span<std::byte> buffer) override
    {
        return _inner.readWithFd(buffer);
    }

    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _inner.write(buffer);
    }

    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        return _inner.writeVectored(segments, std::move(keepAlive));
    }

    [[nodiscard]] ResultAwaitable<void> handshakeIfNeeded() override { return _inner.handshakeIfNeeded(); }

    [[nodiscard]] IoAwaitable waitReadable() override { return _inner.waitReadable(); }

    void cancelRead() noexcept override { _inner.cancelRead(); }

    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override { return _inner.shutdownWrite(); }

    /// Forwarded UNCHANGED, zero and negative included: a non-positive duration removes the bound
    /// (@c ISocket::setReceiveDeadline), and a decorator that filtered it -- treating zero as "leave
    /// it alone", the meaning it once had -- would leave a caller no way to lift a deadline through it.
    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        _inner.setReceiveDeadline(deadline);
    }

    [[nodiscard]] std::string peerAddress() const override { return _inner.peerAddress(); }

    void close() noexcept override { _inner.close(); }

    [[nodiscard]] bool isClosed() const noexcept override { return _inner.isClosed(); }

  protected:
    /// @return The decorated socket, for a subclass that wants to reach it directly.
    [[nodiscard]] ISocket& inner() const noexcept { return _inner; }

  private:
    ISocket& _inner;
};

} // namespace core::net::testing
