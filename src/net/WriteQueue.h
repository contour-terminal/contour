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
/// apply its disconnect policy rather than buffer without limit.

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/IoResult.h>

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
    /// @return True if accepted; false if the queue is over its byte bound or has
    ///         failed/closed — the caller should disconnect the client.
    [[nodiscard]] bool enqueue(std::string frame);

    /// Stops the queue: drops all queued frames and refuses further enqueues.
    /// An in-flight frame write finishes on its own (the drain then stops).
    void close() noexcept;

    /// @return The write error that stopped the queue, if any. Once set, every
    ///         subsequent enqueue fails; the caller should drop the connection.
    [[nodiscard]] std::optional<NetError> const& failure() const noexcept { return _state->failure; }

    /// @return The number of queued-but-unwritten bytes.
    [[nodiscard]] std::size_t queuedBytes() const noexcept { return _state->queuedBytes; }

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

        ISocket* socket;                 ///< The transport written to (not owned).
        std::size_t maxQueuedBytes;      ///< Bound on queued-but-unwritten bytes.
        std::deque<std::string> queue;   ///< Frames awaiting the drain, FIFO.
        std::size_t queuedBytes = 0;     ///< Sum of queued frame sizes.
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
