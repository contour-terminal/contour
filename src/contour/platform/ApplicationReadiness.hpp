// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether the application object exists yet, for the work that must not run before it does.

#include <core/async/AsyncQueue.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/Task.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <vector>

namespace contour::platform
{

/// Says when the Qt application object has come to exist, to the coroutines that wait for it. Any thread.
///
/// Qt runs no event loop on a thread of its own before the application object exists ("QEventLoop: Cannot
/// be used without QCoreApplication"), yet `contour client` starts its attach -- and with it ssh, whose
/// prompts are served on such a thread -- while the GUI is still booting, before that object is made. What
/// would start such a thread waits here, on its own event loop, and the launch says when it may go on, or
/// that it never will.
///
/// Qt-free: what it settles on is said by whoever makes the application object.
class ApplicationReadiness
{
  public:
    /// Where the application object stands.
    enum class State : std::uint8_t
    {
        Pending = 0, ///< Not made yet: a waiter waits.
        Ready,       ///< Made: Qt may run event loops on threads of their own.
        Abandoned,   ///< It will not be made: nothing waits for it any more.
    };

    /// @param initial Where it stands: Ready where the application object exists already.
    explicit ApplicationReadiness(State initial): _state { initial } {}

    ApplicationReadiness(ApplicationReadiness const&) = delete;
    ApplicationReadiness& operator=(ApplicationReadiness const&) = delete;
    ApplicationReadiness(ApplicationReadiness&&) = delete;
    ApplicationReadiness& operator=(ApplicationReadiness&&) = delete;
    ~ApplicationReadiness() = default;

    /// Says the application object exists now, and wakes every waiter. Pending only; later calls are ignored.
    void markReady() { settle(State::Ready); }

    /// Says the application object will not be made after all, and wakes every waiter. Pending only, so that
    /// it may be said on every way out and count only for those taken before the object was made.
    void abandon() { settle(State::Abandoned); }

    /// @return Where it stands now.
    [[nodiscard]] State state() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _state;
    }

    /// @return How many coroutines are parked in whenSettled().
    [[nodiscard]] std::size_t waiting() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _waiters.size();
    }

    /// Completes once the application object exists or will not be made, without holding up the loop it is
    /// awaited on: the coroutine parks, and is resumed on the executor it parked on.
    /// @return Ready or Abandoned; Pending if it is not settled and the awaiting coroutine runs on no
    ///         executor, and so cannot park.
    /// @throws core::async::OperationCancelled if the awaiting coroutine is stopped while it waits.
    [[nodiscard]] core::async::Task<State> whenSettled()
    {
        auto* const executor = core::async::currentExecutor();
        if (executor == nullptr)
            co_return state();
        auto settled = core::async::AsyncQueue<State> { *executor, core::async::AsyncQueueOptions {} };
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_state != State::Pending)
                co_return _state;
            _waiters.push_back(&settled);
        }
        // Unparked by settle(), or by a stop: either way it is no longer a waiter once this is gone.
        auto const parked = Parked { *this, settled };
        co_return (co_await settled.pop()).value_or(State::Abandoned);
    }

  private:
    /// Takes a waiter off the list as its coroutine leaves whenSettled(), settled or stopped.
    class Parked
    {
      public:
        Parked(ApplicationReadiness& readiness, core::async::AsyncQueue<State>& queue):
            _readiness { readiness }, _queue { queue }
        {
        }
        ~Parked()
        {
            auto const lock = std::scoped_lock { _readiness._mutex };
            std::erase(_readiness._waiters, &_queue);
        }
        Parked(Parked const&) = delete;
        Parked& operator=(Parked const&) = delete;
        Parked(Parked&&) = delete;
        Parked& operator=(Parked&&) = delete;

      private:
        ApplicationReadiness& _readiness;
        core::async::AsyncQueue<State>& _queue;
    };

    void settle(State state)
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_state != State::Pending)
            return;
        _state = state;
        // A push resumes nobody inline: it hands the waiter to its executor, so this lock is safe to hold.
        for (auto* const waiter: _waiters)
            std::ignore = waiter->push(state);
    }

    mutable std::mutex _mutex;
    State _state;
    std::vector<core::async::AsyncQueue<State>*> _waiters; ///< The parked whenSettled() calls' queues.
};

} // namespace contour::platform
