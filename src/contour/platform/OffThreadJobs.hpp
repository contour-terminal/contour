// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Jobs run off the GUI thread whose results come back to it: a probe, a listing, a daemon start.

#include <contour/platform/QtInvoke.hpp>

#include <QtCore/QObject>

#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace contour::platform
{

/// Runs jobs off the thread that made it (the GUI thread), each on a thread of its own, and hands each
/// result back to that thread.
///
/// A job runs on its worker and touches nothing of its owner; what it returns is posted to a receiver of
/// this object's, where @c deliver runs. Workers that ended are joined as the next job starts, which is
/// immediate; the rest as this object is destroyed, before the receiver -- so a delivery still queued then
/// is dropped with it, and none reaches an owner going away. Declare it after everything a delivery
/// touches. A job that takes a std::stop_token is handed its worker's, which this object's destruction
/// requests before it joins: a job that waits on the network or on the user ends early rather than holding
/// the destruction up. Thread of its making only, apart from the jobs themselves.
class OffThreadJobs
{
  public:
    OffThreadJobs() = default;
    OffThreadJobs(OffThreadJobs const&) = delete;
    OffThreadJobs(OffThreadJobs&&) = delete;
    OffThreadJobs& operator=(OffThreadJobs const&) = delete;
    OffThreadJobs& operator=(OffThreadJobs&&) = delete;
    /// Asks every worker still running to stop, then joins it: a job under way is waited for, and one that
    /// honours its stop token ends at once.
    ~OffThreadJobs()
    {
        for (auto& worker: _workers)
            worker.thread.request_stop();
    }

    /// Runs @p job on a thread of its own, and @p deliver with its result back on this object's thread.
    /// @param job What runs off the thread; it must touch nothing a delivery does. Called with the worker's
    ///        std::stop_token if it takes one.
    /// @param deliver Told the job's result, on this object's thread, unless this object is gone by then.
    template <typename Job, typename Deliver>
    void run(Job job, Deliver deliver)
    {
        std::erase_if(_workers, [](Worker const& worker) { return worker.ended->load(); });
        auto ended = std::make_shared<std::atomic<bool>>(false);
        auto thread =
            std::jthread { [receiver = &_receiver, ended, job = std::move(job), deliver = std::move(deliver)](
                               std::stop_token const& stop) mutable {
                auto result = [&] {
                    if constexpr (std::is_invocable_v<Job&, std::stop_token const&>)
                        return job(stop);
                    else
                        return job();
                }();
                postToObject(receiver, [deliver = std::move(deliver), result = std::move(result)]() mutable {
                    deliver(std::move(result));
                });
                ended->store(true);
            } };
        _workers.push_back(Worker { .ended = std::move(ended), .thread = std::move(thread) });
    }

    /// @return How many workers are kept: running, or ended and not joined yet.
    [[nodiscard]] std::size_t workerCount() const noexcept { return _workers.size(); }

  private:
    /// A job under way, or ended and not joined yet.
    struct Worker
    {
        std::shared_ptr<std::atomic<bool>> ended; ///< Set by the worker as its last act.
        std::jthread thread;
    };

    QObject _receiver;            ///< Where the results are posted. Declared first: destroyed last.
    std::vector<Worker> _workers; ///< Joined as they are destroyed, before the receiver.
};

} // namespace contour::platform
