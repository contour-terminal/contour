// SPDX-License-Identifier: Apache-2.0
#include <core/net/ISocket.hpp>

#include <core/async/Task.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <utility>

namespace core::net
{

namespace
{
    /// Reads through @p socket's own `read` and reports that no descriptor came with it.
    ///
    /// A coroutine, and therefore a frame, on a path that is neither hot nor common: it is what a
    /// transport with no fd-passing gets when a caller asks for one anyway. The transports here
    /// that CAN pass descriptors implement `readWithFd` natively over their own read slot, so none
    /// of them reaches this.
    /// @param socket The transport to read from; must outlive the operation.
    /// @param buffer The destination.
    /// @return Bytes read, with @c ReadWithFd::fd at -1.
    async::Task<std::expected<ReadWithFd, NetError>> readWithNoFd(ISocket* socket,
                                                                  std::span<std::byte> buffer)
    {
        auto const read = co_await socket->read(buffer);
        if (!read)
            co_return std::unexpected(read.error());
        co_return ReadWithFd { .bytesRead = *read, .fd = -1 };
    }

    /// Writes each segment in turn through @p socket's own `write`.
    ///
    /// **The honest default where no scattered syscall is reachable** — a decorator, an in-memory
    /// transport, a record layer that frames what it is given. It keeps the contract that matters
    /// (every byte of every segment, in order, before it resolves) and gives up only the syscall
    /// count, which is what a transport without `sendmsg` was always going to give up.
    /// @param socket The transport to write to; must outlive the operation.
    /// @param segments The buffers, in send order.
    /// @param keepAlive Pins their backing storage across every suspension below.
    /// @return The total written, or the first failure.
    async::Task<IoResult> writeSegmentsInTurn(ISocket* socket,
                                              std::span<std::span<std::byte const> const> segments,
                                              std::shared_ptr<void const> keepAlive)
    {
        auto total = std::size_t { 0 };
        for (auto const segment: segments)
        {
            if (segment.empty())
                continue;
            auto const written = co_await socket->write(segment);
            if (!written)
                co_return std::unexpected(written.error());
            total += *written;
        }
        // Named after the loop so the pin is visibly required to outlive every suspension above,
        // rather than looking like a parameter nobody reads.
        std::ignore = keepAlive.use_count();
        co_return total;
    }
} // namespace

ResultAwaitable<ReadWithFd> ISocket::readWithFd(std::span<std::byte> buffer)
{
    return ResultAwaitable<ReadWithFd> { readWithNoFd(this, buffer) };
}

IoAwaitable ISocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                   std::shared_ptr<void const> keepAlive)
{
    return IoAwaitable { writeSegmentsInTurn(this, segments, std::move(keepAlive)) };
}

ResultAwaitable<void> ISocket::handshakeIfNeeded()
{
    return ResultAwaitable<void> { std::expected<void, NetError> {} };
}

ResultAwaitable<void> ISocket::shutdownWrite()
{
    // Same shape as the handshake above, and for the same reason: nothing to do here, but the verb
    // has to be awaitable for the transports where it IS work.
    return ResultAwaitable<void> { std::expected<void, NetError> {} };
}

IoAwaitable ISocket::waitReadable()
{
    // The fail-safe direction: a transport that cannot tell must not claim EOF. A false `>0` costs
    // one `read` that discovers the truth; a false `0` tells a caller its peer is gone.
    return IoAwaitable { IoResult { std::size_t { 1 } } };
}

void ISocket::setReceiveDeadline(std::chrono::milliseconds /*deadline*/) noexcept
{
}

} // namespace core::net
