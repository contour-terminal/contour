// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

/// The most worker threads extensions get, however many cores the machine has: extensions draw a
/// status line, and must never compete with the terminal for the machine.
inline constexpr std::size_t MaxExtensionWorkers = 4;

/// How many workers to run on a machine.
/// @param hardwareConcurrency What std::thread::hardware_concurrency() reports; 0 if unknown.
/// @return One per core, at most MaxExtensionWorkers, at least one.
[[nodiscard]] constexpr std::size_t extensionWorkerCount(unsigned hardwareConcurrency) noexcept
{
    return std::clamp<std::size_t>(hardwareConcurrency, 1, MaxExtensionWorkers);
}

/// The production IExtensionScheduler: a fixed number of worker threads, each one a lane.
///
/// Nothing runs until the first runner is assigned a lane; then all workers start. Each holds a thread
/// guard of the backend for its whole life, so that it may call into instances, and renews it before a job
/// whenever the runtime has been brought up anew since (@see wasm::IWasmBackend::runtimeGeneration). Lanes
/// are handed out round-robin. Timers run in real time, by a steady clock (which clock() returns).
///
/// Destruction stops the workers: each runs the jobs already posted to it (a runner's last job destroys
/// its instance on its own lane), discards its timers unrun, and is joined. Whatever those jobs reach
/// must therefore outlive the pool.
class ExtensionWorkerPool final: public IExtensionScheduler
{
  public:
    /// @param backend What the workers prepare their threads with; must outlive the pool.
    /// @param workerCount How many workers (and lanes) to run once started; at least one.
    ///        @see extensionWorkerCount
    ExtensionWorkerPool(wasm::IWasmBackend& backend, std::size_t workerCount);
    ~ExtensionWorkerPool() override;

    ExtensionWorkerPool(ExtensionWorkerPool const&) = delete;
    ExtensionWorkerPool(ExtensionWorkerPool&&) = delete;
    ExtensionWorkerPool& operator=(ExtensionWorkerPool const&) = delete;
    ExtensionWorkerPool& operator=(ExtensionWorkerPool&&) = delete;

    [[nodiscard]] LaneId assignLane() override;
    void post(LaneId lane, std::function<void()> job) override;
    [[nodiscard]] TimerId schedule(LaneId lane,
                                   std::chrono::milliseconds after,
                                   std::function<void()> job) override;
    void cancel(TimerId timer) noexcept override;
    [[nodiscard]] core::platform::IClock& clock() noexcept override { return _clock; }

    /// Stops the workers, as destruction does: each runs the jobs already posted to it, discards its timers
    /// unrun, and is joined. Whatever those jobs reach stays alive meanwhile. A job posted afterwards is
    /// never run, and is destroyed with the pool. Idempotent; not from a worker.
    void stop() noexcept;

    /// @return How many worker threads run: none before the first lane is assigned.
    [[nodiscard]] std::size_t runningWorkers() const noexcept
    {
        return _running.load(std::memory_order_acquire);
    }

  private:
    /// One timer waiting on a worker.
    struct Timer
    {
        core::platform::SteadyTimePoint due;
        TimerId id {};
        std::function<void()> job;
    };

    /// Whether a worker keeps waiting for work.
    enum class WorkerState : uint8_t
    {
        Running,
        Stopping,
    };

    /// One worker thread and what it waits on.
    struct Worker
    {
        std::mutex mutex;
        std::condition_variable wakeup;
        std::deque<std::function<void()>> jobs; ///< Guarded by mutex.
        std::vector<Timer> timers;              ///< A min-heap on due, guarded by mutex.
        WorkerState state = WorkerState::Running;
        std::thread thread;
    };

    /// Orders the timer heap so that its front is the earliest due.
    [[nodiscard]] static bool dueLater(Timer const& a, Timer const& b) noexcept { return a.due > b.due; }

    /// Starts every worker, once.
    void start();

    /// A worker's body.
    void work(Worker& worker);

    /// @return The worker behind a lane.
    [[nodiscard]] Worker& workerOf(LaneId lane) const;

    wasm::IWasmBackend& _backend;
    core::platform::SteadyClock _clock;
    std::vector<std::unique_ptr<Worker>> _workers; ///< Fixed at construction; started lazily.
    std::once_flag _started;
    std::atomic<std::size_t> _running { 0 };
    std::atomic<uint32_t> _nextLane { 0 };
    std::atomic<uint64_t> _nextTimer { 1 };
};

} // namespace vtextension
