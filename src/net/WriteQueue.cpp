// SPDX-License-Identifier: Apache-2.0
#include <net/WriteQueue.hpp>

#include <span>
#include <utility>

#include <coro/Cancellation.hpp>

namespace net
{

bool WriteQueue::enqueue(std::string frame, uint64_t tag)
{
    auto& state = *_state;
    if (state.closed || state.failure.has_value())
        return false;
    // Only a frame landing on top of an existing backlog evidences a peer falling behind;
    // this frame's own size evidences nothing. @see the bound rule in the file comment.
    if (!state.queue.empty() && state.backlogBytes + frame.size() > state.maxQueuedBytes)
        return false; // over bound: the caller applies its disconnect policy

    state.backlogBytes += frame.size();
    state.queue.push_back(State::Pending { .frame = std::move(frame), .tag = tag });

    if (!state.draining)
    {
        state.draining = true;
        _loop.spawn(drain(_state));
    }
    return true;
}

std::size_t WriteQueue::dropTagged(uint64_t tag) noexcept
{
    if (tag == 0)
        return 0; // the untagged label: never a supersede target
    auto& state = *_state;
    return std::erase_if(state.queue, [&](State::Pending const& pending) {
        if (pending.tag != tag)
            return false;
        state.backlogBytes -= pending.frame.size();
        return true;
    });
}

void WriteQueue::close() noexcept
{
    _state->closed = true;
    _state->queue.clear();
    _state->backlogBytes = 0;
    // Also zeroed here, not only in the drain: the drain may never resume to do it (loop
    // shutdown), and a stale count would misreport a dead queue.
    _state->inFlightBytes = 0;
    // Abort any in-flight write so the drain coroutine stops promptly instead
    // of finishing a write to a peer the caller is already tearing down.
    _state->socket->close();
}

coro::Task<void> WriteQueue::flushThenClose()
{
    co_await pollUntil(&_loop, [state = _state] { return state->backlogBytes == 0 && !state->draining; });
    close();
}

coro::Task<void> WriteQueue::drain(std::shared_ptr<State> state)
{
    while (!state->queue.empty() && !state->failure.has_value() && !state->closed)
    {
        // Keep the frame alive across the (possibly parking) write, and move it from the
        // backlog into inFlightBytes rather than out of the accounting: the bound must stop
        // governing it, while queuedBytes() still reports it as owed to the peer.
        auto const pending = std::move(state->queue.front());
        state->queue.pop_front();
        state->backlogBytes -= pending.frame.size();
        state->inFlightBytes = pending.frame.size();

        try
        {
            auto const bytes =
                std::span<std::byte const> { reinterpret_cast<std::byte const*>(pending.frame.data()),
                                             pending.frame.size() };
            auto const written = co_await state->socket->write(bytes);
            state->inFlightBytes = 0;
            if (!written.has_value())
            {
                state->failure = written.error();
                state->queue.clear();
                state->backlogBytes = 0;
            }
        }
        catch (coro::OperationCancelled const&)
        {
            // Loop shutdown while parked on backpressure: stop draining; the
            // queue's owner is being torn down with the loop.
            state->inFlightBytes = 0;
            break;
        }
    }
    state->draining = false;
}

} // namespace net
