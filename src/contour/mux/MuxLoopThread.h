// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `MuxLoopThread` — a net::EventLoop running on its own thread, beside Qt's.
///
/// The GUI's mux controllers (native attach, tmux mirroring) do all their
/// socket and protocol work on this reactor; the ONLY thread-safe entry into
/// it is post(). Data flowing the other way needs no Qt marshaling either:
/// it lands in a thread-safe vtpty::ChannelPty::feed, and the session's own
/// parser thread does the rest — the identical threading a local session has.

#include <atomic>
#include <functional>
#include <thread>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>

namespace contour
{

/// Owns the reactor and the thread that pumps it.
class MuxLoopThread
{
  public:
    MuxLoopThread() = default;

    /// Joins; the owner must have made the root task finish first (post a
    /// shutdown into the loop), or this blocks forever by design.
    ~MuxLoopThread() { join(); }

    MuxLoopThread(MuxLoopThread const&) = delete;
    MuxLoopThread& operator=(MuxLoopThread const&) = delete;
    MuxLoopThread(MuxLoopThread&&) = delete;
    MuxLoopThread& operator=(MuxLoopThread&&) = delete;

    /// Starts the thread, running @p rootTask to completion on the loop.
    /// A cancellation unwind (requestStop) ends the task silently.
    void start(std::function<coro::Task<void>(net::EventLoop*)> rootTask)
    {
        _thread = std::thread { [this, rootTask = std::move(rootTask)]() mutable {
            try
            {
                _loop.blockOn(rootTask(&_loop));
            }
            catch (coro::OperationCancelled const&)
            {
                // requestStop() unwound the root task — the intended shutdown.
                _cancelled.store(true, std::memory_order_release);
            }
        } };
    }

    /// @return Whether the root task ended by cancellation (requestStop)
    ///         rather than running to completion.
    [[nodiscard]] bool wasCancelled() const noexcept { return _cancelled.load(std::memory_order_acquire); }

    /// Marshals @p fn onto the loop thread. Thread-safe.
    void post(std::function<void()> fn) { _loop.post(std::move(fn)); }

    /// Cancels every flow on the loop (marshaled onto it), so a task parked
    /// in connect/read unwinds and the thread becomes joinable.
    void requestStop()
    {
        _loop.post([this] { _loop.requestStop(); });
    }

    /// Waits for the root task (and thus the thread) to finish. Idempotent.
    void join()
    {
        if (_thread.joinable())
            _thread.join();
    }

  private:
    net::PollEventSource _source;
    net::EventLoop _loop { _source };
    std::thread _thread;
    std::atomic<bool> _cancelled { false };
};

} // namespace contour
