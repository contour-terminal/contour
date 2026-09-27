// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `testing::ManualExecutor` — an executor a test drains by hand.

#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>

#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace core::async::testing
{

/// An executor that runs nothing until a test says so, and states itself as the current executor
/// while it does.
///
/// `submit` only queues, from any thread; `runOne` and `drain` resume on the calling thread,
/// inside an @c ExecutorScope naming this executor -- the part a hand-written test double tends to
/// leave out, and without which a coroutine it resumes has no current executor and every
/// awaitable that reads one takes its fallback instead. So a case over this double exercises the
/// path an `EventLoop` turn or a `ThreadPoolExecutor` worker does.
///
/// Work still queued when it is destroyed is dropped, not resumed: what nothing else owns (a chain
/// rooted in a `DetachedTask`) is freed, and what a `Task` owns is left to it.
///
/// The namespace is `core::async::testing`, the directory's, as `core::net::testing` is net's:
/// inside `namespace core::async` an unqualified `testing::` names this one and not the `core::testing`
/// module, so code there that means the module spells it `core::testing::`.
class ManualExecutor final: public IExecutor
{
  public:
    ManualExecutor() = default;
    ManualExecutor(ManualExecutor const&) = delete;
    ManualExecutor(ManualExecutor&&) = delete;
    ManualExecutor& operator=(ManualExecutor const&) = delete;
    ManualExecutor& operator=(ManualExecutor&&) = delete;
    ~ManualExecutor() override = default;

    using IExecutor::submit;

    /// Queues @p handle, borrowed. Callable from any thread.
    /// @param handle The coroutine to resume.
    void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

    /// Queues @p work, holding its claim while it waits. Callable from any thread.
    /// @param work The coroutine to resume, and its claim on the chain root.
    void submit(ParkedWork work) override
    {
        auto entry = detail::Parked { std::move(work) };
        auto const guard = std::scoped_lock { _mutex };
        _queue.push_back(std::move(entry));
    }

    /// Resumes the oldest queued entry on the calling thread, with this executor current.
    /// @return Whether there was one. What it throws propagates, and the entry is gone.
    bool runOne()
    {
        auto entry = detail::Parked {};
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_queue.empty())
                return false;
            entry = std::move(_queue.front());
            _queue.pop_front();
        }
        auto const scope = ExecutorScope { *this };
        entry.resume();
        return true;
    }

    /// How many resumptions @c drain makes, by default, before it calls the work endless.
    static constexpr std::size_t DrainBound = std::size_t { 1 } << 20U;

    /// Resumes entries until none is left, including those the resumptions queue.
    ///
    /// Bounded: work that requeues itself for ever would otherwise hang the case that drains it,
    /// where a bound fails it and says why.
    /// @param bound How many resumptions to make at most.
    /// @return How many were resumed. What one throws propagates, and the rest stay queued.
    /// @throws std::length_error Where @p bound resumptions left work still queued.
    std::size_t drain(std::size_t bound = DrainBound)
    {
        auto ran = std::size_t { 0 };
        while (ran < bound && runOne())
            ++ran;
        if (ran == bound && pending() != 0)
            throw std::length_error { "ManualExecutor::drain: " + std::to_string(bound)
                                      + " resumptions and work is still queued -- work that requeues "
                                        "itself for ever?" };
        return ran;
    }

    /// @return How many entries are queued right now.
    [[nodiscard]] std::size_t pending() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _queue.size();
    }

  private:
    mutable std::mutex _mutex;
    std::deque<detail::Parked> _queue;
};

} // namespace core::async::testing
