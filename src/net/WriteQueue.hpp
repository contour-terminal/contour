// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `WriteQueue` — the single writer per connection.
///
/// Two coroutines calling `socket->write()` concurrently interleave their bytes
/// and corrupt the stream (a write parks mid-frame on backpressure and another
/// write slips in). Endo never hit this because its server was strict
/// request/response; a daemon pushing notifications WHILE answering requests
/// will. All writes therefore go through this queue: producers `enqueue()`
/// whole frames (non-suspending, callable from event callbacks), and ONE drain
/// coroutine writes them out in FIFO order, each frame fully before the next.
///
/// The queue is bounded by bytes: a slow or stuck client that lets frames pile
/// up past the bound makes `enqueue()` fail, which is the caller's signal to
/// apply its disconnect policy rather than buffer without limit. The bound
/// governs the BACKLOG, not any single frame — a frame is always accepted when
/// nothing is waiting behind the one being written, however large it is. The
/// bound exists to detect a peer that stopped draining, and a lone oversized
/// frame is not evidence of that; refusing it would instead make a legitimately
/// large message (a full-grid snapshot of a deep scrollback) impossible to send, a cliff
/// neither the producer nor the peer can do anything about.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <coro/Task.hpp>
#include <net/EventLoop.hpp>
#include <net/ISocket.hpp>
#include <net/IoResult.hpp>

namespace net
{

/// FIFO byte-frame queue with a single drain coroutine per connection.
///
/// Single-threaded: all calls happen on the loop thread (marshal via
/// EventLoop::post from elsewhere). Frames are written atomically — the drain
/// finishes one frame (looping over partial writes inside ISocket::write)
/// before starting the next.
///
/// Lifetimes: the queue's state is shared with the drain coroutine, so the
/// WriteQueue object itself may be destroyed while a drain is parked (the drain
/// finishes against the shared state). The SOCKET must outlive any in-flight
/// drain: owners close() the queue and the socket, then let the loop settle
/// (the drain resumes, observes the closed socket, and stops) before destroying
/// the socket object.
class WriteQueue
{
  public:
    /// @param loop The loop the drain coroutine runs on (not owned).
    /// @param socket The transport written to (not owned; see the lifetime note).
    /// @param maxQueuedBytes Enqueue fails once the queued-but-unwritten total
    ///        would exceed this bound.
    WriteQueue(EventLoop& loop, ISocket* socket, std::size_t maxQueuedBytes):
        _loop(loop), _state(std::make_shared<State>(socket, maxQueuedBytes))
    {
    }

    /// Queues @p frame for writing and starts the drain if none is running.
    /// @param frame The bytes to send as one atomic unit (moved in).
    /// @param tag An opaque caller-chosen label for @ref dropTagged. Zero (the
    ///        default) marks a frame no supersede may ever discard.
    /// @return True if accepted; false if the backlog is over its byte bound or the
    ///         queue has failed/closed — the caller should disconnect the client.
    ///         A frame enqueued onto an empty backlog is always accepted.
    [[nodiscard]] bool enqueue(std::string frame, uint64_t tag = 0);

    /// Discards every unwritten frame carrying @p tag.
    ///
    /// For a producer whose next frame fully re-describes what the tagged ones said — a
    /// snapshot superseding the deltas that preceded it — those frames are dead weight:
    /// sending them costs bandwidth the peer must also decode, and can push the backlog
    /// over the bound and cost it the connection. Dropping them is only sound because the
    /// replacement is self-contained; a queue cannot know that, so the caller says so by
    /// tagging.
    ///
    /// Tag 0 is never matched, so untagged frames (handshakes, layout) always survive.
    /// The frame currently being written is already on the wire and is never dropped.
    /// @param tag The label whose frames to discard.
    /// @return How many frames were discarded. Callers whose own bookkeeping records what the
    ///         peer has been told need this: only a DROPPED frame makes that record wrong, and
    ///         rebuilding it when nothing was dropped is pure cost.
    std::size_t dropTagged(uint64_t tag) noexcept;

    /// Stops the queue: drops all queued frames and refuses further enqueues.
    /// An in-flight frame write finishes on its own (the drain then stops).
    void close() noexcept;

    /// Waits until every queued frame is written (or the queue failed), then
    /// closes the queue — the graceful teardown epilogue every connection owner
    /// needs. The owner still closes and out-lives the SOCKET afterwards, per
    /// the lifetime note above.
    [[nodiscard]] coro::Task<void> flushThenClose();

    /// @return The write error that stopped the queue, if any. Once set, every
    ///         subsequent enqueue fails; the caller should drop the connection.
    [[nodiscard]] std::optional<NetError> const& failure() const noexcept { return _state->failure; }

    /// @return The number of bytes still owed to the peer: the backlog plus the frame
    ///         currently being written. The in-flight frame counts because it is not yet
    ///         the peer's problem — leaving it out understates the true debt by a whole
    ///         frame, which is exactly the frame that is usually largest.
    [[nodiscard]] std::size_t queuedBytes() const noexcept
    {
        return _state->backlogBytes + _state->inFlightBytes;
    }

    /// @return The bytes waiting behind the in-flight frame — what the bound governs.
    [[nodiscard]] std::size_t backlogBytes() const noexcept { return _state->backlogBytes; }

    /// Explains why `enqueue` last refused, for the caller's log line.
    ///
    /// The two reasons are very different problems for whoever reads the log — a peer that
    /// stopped draining versus a transport that died under us — and every caller needs the same
    /// distinction, so the queue that owns both facts words it once.
    /// @return A human-readable reason, without a trailing period.
    [[nodiscard]] std::string describeRefusal() const
    {
        if (auto const& failure = _state->failure)
            return "transport write failed: " + failure->toString();
        return std::format("send queue overflow ({} of {} bytes backlogged, {} in flight)",
                           _state->backlogBytes,
                           _state->maxQueuedBytes,
                           _state->inFlightBytes);
    }

    /// @return True while the drain coroutine is running (tests/diagnostics).
    [[nodiscard]] bool draining() const noexcept { return _state->draining; }

  private:
    /// The queue state, shared between the WriteQueue handle and the drain
    /// coroutine so neither dangles if the other finishes first.
    struct State
    {
        State(ISocket* socketArg, std::size_t maxQueuedBytesArg) noexcept:
            socket(socketArg), maxQueuedBytes(maxQueuedBytesArg)
        {
        }

        /// One queued frame and the label that lets a later frame supersede it.
        struct Pending
        {
            std::string frame; ///< The bytes to write, as one atomic unit.
            uint64_t tag;      ///< @see WriteQueue::dropTagged. Zero can never be dropped.
        };

        ISocket* socket;                 ///< The transport written to (not owned).
        std::size_t maxQueuedBytes;      ///< Bound on the backlog (excludes the in-flight frame).
        std::deque<Pending> queue;       ///< Frames awaiting the drain, FIFO.
        std::size_t backlogBytes = 0;    ///< Sum of backlogged frame sizes.
        std::size_t inFlightBytes = 0;   ///< Size of the frame the drain is writing, if any.
        bool draining = false;           ///< A drain coroutine is live.
        bool closed = false;             ///< close() called; enqueues refused.
        std::optional<NetError> failure; ///< First write error; poisons the queue.
    };

    /// The single writer: pops and writes frames until the queue is empty, then
    /// finishes. enqueue() spawns a fresh drain for the next burst — the
    /// `draining` flag guarantees at most one drain exists at any moment.
    static coro::Task<void> drain(std::shared_ptr<State> state);

    EventLoop& _loop;              ///< Runs the drain coroutine.
    std::shared_ptr<State> _state; ///< Shared with the drain coroutine.
};

} // namespace net
