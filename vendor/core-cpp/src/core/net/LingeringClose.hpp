// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `closeLingering` — close a connection whose peer may still be sending, without the close
/// destroying what was just written to it.
///
/// Imported from fastcached's `Net/LingeringClose.{hpp,cpp}` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` (core-cpp#35).

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace core::net
{

/// How long, and how much, a closing server listens to a peer it has stopped answering.
///
/// Time is bounded two ways because two sockets wait two ways: a loop socket's reads SUSPEND, so
/// the whole drain is one deadline when a loop is given; a blocking socket's reads BLOCK, so each
/// one carries `total / reads` and the read count caps the sum, which makes the worst case `total`
/// on both. The byte cap is the third bound and the one a fast peer meets: never more than serving
/// the request would have read. **The bounds are the design**: an unbounded drain turns a refusal
/// into a denial of service against the server that refused.
struct LingerBounds
{
    /// The whole drain, from the half-close to the close. Non-positive arms nothing, for a
    /// surface whose own sweep already bounds a socket it has refused.
    std::chrono::milliseconds total;

    /// Bytes discarded at most. Past them the close is a reset after all -- the price of a
    /// bound, and never a promise to read an unbounded upload in order to refuse it.
    std::size_t maxBytes;

    /// Reads at most, which is what bounds a blocking socket's total.
    std::size_t reads;
};

/// How a lingering close ended.
enum class LingerEnd : std::uint8_t
{
    AlreadyClosed, ///< Nothing to listen to: the socket was closed before the linger began.
    PeerFinished,  ///< The peer closed or half-closed: EOF, the ordinary ending.
    PeerFailed,    ///< A read failed on its own: a reset, or the socket gone under the read.
    Expired,       ///< The bound ran out before the peer finished.
    ReadCap,       ///< The reads ran out first, so a peer still sending meets a reset.
    ByteCap,       ///< The bytes ran out first, so a peer still sending meets a reset.
};

/// How a lingering close ended, and after how many reads.
struct LingerOutcome
{
    LingerEnd end;     ///< How it ended.
    std::size_t reads; ///< The reads it made, the last one included.
};

/// Half-closes, listens to the peer until it closes too or a bound runs out, then closes.
///
/// **A close with the peer's bytes still unread is an RST, not a FIN**, and an RST destroys what
/// the peer had not read yet. Measured on loopback (fastcached#1553): Windows drops every byte
/// already buffered for the reader and reports the reset at once; Linux and macOS hand those bytes
/// over first and then report it. So a server that answers a request it did not finish reading --
/// a `413` over a body past its cap, a `400` over a head it could not parse -- and then closes,
/// delivers that answer followed by an error at best, and a reset alone on Windows.
///
/// So the close lingers, the way web servers have for decades: half-close first, so the answer is
/// followed by a FIN and a well-behaved peer closes on reading it; discard what the peer is still
/// sending; close once it has closed, or once the bounds say stop. A peer that closed first costs
/// ONE read, which reports EOF or the reset at once -- so an ordinary goodbye is never turned into
/// a wait. A peer that keeps sending past the bounds meets the reset anyway -- bounded best
/// effort.
///
/// A socket that is already closed is only closed again, which is a no-op.
/// @param socket The socket to close; must outlive the task.
/// @param loop Where a loop socket's whole-drain deadline is armed, or nullptr, which bounds each
///        read by its share of the total through @c ISocket::setReceiveDeadline instead -- a
///        blocking socket's only bound, and one a loop socket honours too.
/// @param bounds How long and how much to listen.
/// @return A task that completes once the socket is closed, with how the linger ended.
[[nodiscard]] async::Task<LingerOutcome> closeLingering(ISocket* socket,
                                                        EventLoop* loop,
                                                        LingerBounds bounds);

} // namespace core::net
