// SPDX-License-Identifier: Apache-2.0
#include <core/net/LingeringClose.hpp>

#include <core/net/NetError.hpp>
#include <core/net/SocketDeadline.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <span>
#include <tuple>

namespace core::net
{

namespace
{
    /// Enough to take a typical refused request's remainder in one read.
    constexpr auto DrainChunkBytes = std::size_t { 16 } * 1024;
} // namespace

async::Task<LingerOutcome> closeLingering(ISocket* socket, EventLoop* loop, LingerBounds bounds)
{
    // Closed or, as `ISocket::isClosed` also allows, seen at EOF: nothing to listen to.
    // `close()` is idempotent, and a transport that answers true at EOF still owns a handle.
    if (socket->isClosed())
    {
        socket->close();
        co_return LingerOutcome { .end = LingerEnd::AlreadyClosed, .reads = 0 };
    }

    // A half-close the transport could not deliver changes nothing below: the reads that follow
    // report the peer's state either way.
    std::ignore = co_await socket->shutdownWrite();

    // Past every read without the peer finishing, unless a read below says otherwise.
    auto outcome = LingerOutcome { .end = LingerEnd::ReadCap, .reads = 0 };
    auto target = SocketDeadlineTarget { .socket = socket, .expired = false };
    {
        // A loop socket: one deadline over the whole drain, which closes the socket and so
        // completes the parked read. Without a loop: each read carries its share instead.
        auto const deadline = armSocketDeadline(loop, bounds.total, &target);
        if (bounds.total > std::chrono::milliseconds::zero() && bounds.reads > 0)
        {
            // At least a millisecond each, so a read count past the total's milliseconds cannot
            // divide it down to zero -- which `setReceiveDeadline` reads as "no bound at all".
            auto const shares =
                std::min<std::size_t>(bounds.reads, static_cast<std::size_t>(bounds.total.count()));
            socket->setReceiveDeadline(bounds.total / static_cast<std::chrono::milliseconds::rep>(shares));
        }

        auto discard = std::array<std::byte, DrainChunkBytes> {};
        auto discarded = std::size_t { 0 };
        for (auto const read: std::views::iota(std::size_t { 0 }, bounds.reads))
        {
            auto const got = co_await socket->read(std::span<std::byte> { discard });
            outcome.reads = read + 1;
            if (got.has_value() && *got > 0)
            {
                discarded += *got;
                if (discarded < bounds.maxBytes)
                    continue;
                outcome.end = LingerEnd::ByteCap;
                break;
            }
            if (got.has_value())
                outcome.end = LingerEnd::PeerFinished;
            else if (target.expired || isDeadlineExpiry(got.error().code))
                outcome.end = LingerEnd::Expired;
            else
                outcome.end = LingerEnd::PeerFailed;
            break;
        }
    }
    socket->close();
    co_return outcome;
}

} // namespace core::net
