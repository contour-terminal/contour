// SPDX-License-Identifier: Apache-2.0
#include <net/WriteQueue.h>

#include <span>
#include <utility>

#include <coro/Cancellation.hpp>

namespace net
{

bool WriteQueue::enqueue(std::string frame)
{
    auto& state = *_state;
    if (state.closed || state.failure.has_value())
        return false;
    if (state.queuedBytes + frame.size() > state.maxQueuedBytes)
        return false; // over bound: the caller applies its disconnect policy

    state.queuedBytes += frame.size();
    state.queue.push_back(std::move(frame));

    if (!state.draining)
    {
        state.draining = true;
        _loop.spawn(drain(_state));
    }
    return true;
}

void WriteQueue::close() noexcept
{
    _state->closed = true;
    _state->queue.clear();
    _state->queuedBytes = 0;
}

coro::Task<void> WriteQueue::flushThenClose()
{
    co_await pollUntil(&_loop, [state = _state] { return state->queuedBytes == 0 && !state->draining; });
    close();
}

coro::Task<void> WriteQueue::drain(std::shared_ptr<State> state)
{
    while (!state->queue.empty() && !state->failure.has_value() && !state->closed)
    {
        // Keep the frame alive across the (possibly parking) write; adjust the
        // byte accounting up front so queuedBytes reflects backlog, not in-flight.
        auto const frame = std::move(state->queue.front());
        state->queue.pop_front();
        state->queuedBytes -= frame.size();

        try
        {
            auto const bytes =
                std::span<std::byte const> { reinterpret_cast<std::byte const*>(frame.data()), frame.size() };
            auto const written = co_await state->socket->write(bytes);
            if (!written.has_value())
            {
                state->failure = written.error();
                state->queue.clear();
                state->queuedBytes = 0;
            }
        }
        catch (coro::OperationCancelled const&)
        {
            // Loop shutdown while parked on backpressure: stop draining; the
            // queue's owner is being torn down with the loop.
            break;
        }
    }
    state->draining = false;
}

} // namespace net
