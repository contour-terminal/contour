// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the thread that owns a pane, as a queue the test runs by hand.
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

#include <vtextension/SessionExtensions.hpp>

namespace vtextension::testing
{

/// The owner's thread, as a queue the test runs, or drops, the way a pane that went away drops it. A build
/// thread posts to it while the test polls it, so it is guarded.
class QueuedOwner final: public vtextension::IOwnerThread
{
  public:
    void post(std::function<void()> job) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _jobs.push_back(std::move(job));
    }

    /// Runs what was handed back, as the owner's event loop would: on the calling thread, outside the lock.
    void run()
    {
        while (auto job = take())
            (*job)();
    }

    /// Drops what was handed back without running it, as a pane that went away does.
    void drop()
    {
        auto jobs = std::deque<std::function<void()>> {};
        {
            auto const lock = std::scoped_lock { _mutex };
            jobs.swap(_jobs);
        }
        // Destroyed outside the lock: what a job holds is released as a dropped event's would be.
    }

    [[nodiscard]] std::size_t pending() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _jobs.size();
    }

  private:
    [[nodiscard]] std::optional<std::function<void()>> take()
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_jobs.empty())
            return std::nullopt;
        auto job = std::move(_jobs.front());
        _jobs.pop_front();
        return job;
    }

    mutable std::mutex _mutex;
    std::deque<std::function<void()>> _jobs; ///< Guarded by _mutex.
};

} // namespace vtextension::testing
