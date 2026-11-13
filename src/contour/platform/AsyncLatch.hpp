// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A value given once, from any thread, and awaited by any number of coroutines without holding up their
/// loops.

#include <core/Assert.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/Task.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace contour::platform
{

/// One value, set at most once from any thread, awaited by any number of coroutines.
///
/// A waiter parks its coroutine, never its thread: it is resumed on the executor it parked on, and a stop of
/// that coroutine ends its wait (with core::async::OperationCancelled), as for every loop awaitable.
/// @tparam T The value; copied to every waiter.
template <typename T>
    requires std::is_nothrow_move_constructible_v<T> && std::is_copy_constructible_v<T>
class AsyncLatch
{
  public:
    AsyncLatch() = default;
    AsyncLatch(AsyncLatch const&) = delete;
    AsyncLatch& operator=(AsyncLatch const&) = delete;
    AsyncLatch(AsyncLatch&&) = delete;
    AsyncLatch& operator=(AsyncLatch&&) = delete;
    ~AsyncLatch() = default;

    /// Sets the value, unless it is set already, and wakes every waiter. Never resumes one inline: each is
    /// handed to its executor, so the caller may hold locks of its own.
    /// @param value The value.
    /// @return Whether this call set it.
    bool set(T value)
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_value)
            return false;
        _value.emplace(std::move(value));
        for (auto* const waiter: _waiters)
            std::ignore = waiter->push(*_value);
        return true;
    }

    /// @return The value, if it is set.
    [[nodiscard]] std::optional<T> value() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _value;
    }

    /// Completes once the value is set: at once if it is.
    /// The awaiting coroutine must run on an executor (an event loop, a strand) when the value is not set
    /// yet, as there is nothing else to park it on.
    /// @return The value.
    /// @throws core::async::OperationCancelled if the awaiting coroutine is stopped while it waits.
    [[nodiscard]] core::async::Task<T> wait()
    {
        auto* const executor = core::async::currentExecutor();
        auto arrived = std::optional<core::async::AsyncQueue<T>> {};
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_value)
                co_return *_value;
            Require(executor != nullptr);
            arrived.emplace(*executor, core::async::AsyncQueueOptions {});
            _waiters.push_back(&*arrived);
        }
        // Off the list on the way out, whether it was set or stopped; before the queue is destroyed.
        auto const parked = Parked { *this, *arrived };
        auto value = co_await arrived->pop();
        // The queue is closed by nobody: an item is the only way a pop ends without a stop.
        Guarantee(value.has_value());
        co_return *std::move(value);
    }

  private:
    friend struct AsyncLatchTestAccess;

    /// Takes a waiter off the list as its coroutine leaves wait().
    class Parked
    {
      public:
        Parked(AsyncLatch& latch, core::async::AsyncQueue<T>& queue): _latch { latch }, _queue { queue } {}
        ~Parked()
        {
            auto const lock = std::scoped_lock { _latch._mutex };
            std::erase(_latch._waiters, &_queue);
        }
        Parked(Parked const&) = delete;
        Parked& operator=(Parked const&) = delete;
        Parked(Parked&&) = delete;
        Parked& operator=(Parked&&) = delete;

      private:
        AsyncLatch& _latch;
        core::async::AsyncQueue<T>& _queue;
    };

    mutable std::mutex _mutex;
    std::optional<T> _value;
    std::vector<core::async::AsyncQueue<T>*> _waiters; ///< The parked wait() calls' queues.
};

/// What a test may see of an AsyncLatch.
struct AsyncLatchTestAccess
{
    /// @return How many coroutines are parked in @p latch's wait().
    template <typename T>
    [[nodiscard]] static std::size_t waiting(AsyncLatch<T> const& latch)
    {
        auto const lock = std::scoped_lock { latch._mutex };
        return latch._waiters.size();
    }
};

} // namespace contour::platform
