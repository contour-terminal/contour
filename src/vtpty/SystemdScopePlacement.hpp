// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PlacementBreaker.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/ScopeBus.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace vtpty
{

/// The name SystemdScopePlacement's worker thread carries, at most 15 characters as Linux allows.
inline constexpr auto WorkerThreadName = "scope-placement";

/// How SystemdScopePlacement places children.
struct SystemdScopeConfig
{
    /// Each scope is named `<unitPrefix>-<contour pid>-<child pid>-<n>.scope`.
    std::string unitPrefix = "contour-session";

    /// The slice the scopes go into: where desktops put applications, and what systemd-oomd watches.
    std::string slice = "app.slice";

    /// How long one request may take before its child is released unplaced.
    std::chrono::milliseconds deadline { 1000 };

    /// How long an unresponsive bus is left alone. @see PlacementBreaker.
    core::platform::SteadyDuration breakerCooldown = std::chrono::seconds { 30 };

    /// A memory ceiling for every scope. Tests only: a user-facing limit is not a feature yet.
    std::optional<MemoryLimit> memoryLimit {};

    /// The oom_score_adj each child sets for itself, if any. @see ProcessPlacement::childOomScoreAdjust.
    std::optional<int> childOomScoreAdjust {};
};

/// Places every child in a transient systemd scope of its own, with OOMPolicy=continue, so that an
/// out-of-memory kill inside a session stops neither Contour's unit nor the rest of that session.
///
/// One worker thread owns the bus -- sd-bus connections are not thread-safe -- and it starts with
/// the first child, so a placement nobody uses costs nothing. Every child is released whatever
/// happens: placed, refused, timed out, skipped by the breaker, or still queued at destruction.
class SystemdScopePlacement final: public ProcessPlacement
{
  public:
    /// @param config     How to place children.
    /// @param connectBus Connects the bus; called on the worker, at first use and after it broke.
    /// @param clock      The clock the breaker reads; must outlive this placement.
    SystemdScopePlacement(SystemdScopeConfig config,
                          ScopeBusFactory connectBus,
                          core::platform::IClock const& clock);

    /// Stops the worker -- after the request in flight, if any -- and releases every queued child.
    ~SystemdScopePlacement() override;

    SystemdScopePlacement(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement(SystemdScopePlacement&&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement&&) = delete;

    void placeThenRelease(ParkedChild child) noexcept override;

    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override
    {
        return _config.childOomScoreAdjust;
    }

  private:
    void startWorker();
    void run();
    void place(ParkedChild const& child);
    [[nodiscard]] std::expected<void, ScopeError> request(ParkedChild const& child, ScopeProtocol protocol);
    void report(ParkedChild const& child, ScopeError error);

    SystemdScopeConfig _config;
    ScopeBusFactory _connectBus;
    core::platform::IClock const& _clock;
    std::unique_ptr<ScopeBus> _bus;
    /// Current until systemd turns it down, then Legacy for as long as this connection lasts.
    ScopeProtocol _protocol = ScopeProtocol::Current;
    PlacementBreaker _breaker;
    std::uint64_t _sequence = 0;
    bool _hasReportedFault = false; ///< Faults are told once where a user sees them, then quietly.

    /// Whether the worker is to keep taking children off the queue.
    enum class WorkerState : std::uint8_t
    {
        Running,
        Stopping,
    };

    // A std::thread and a state rather than a std::jthread: libc++ before 20 offers std::jthread
    // only behind -fexperimental-library, which Xcode's toolchains do not enable.
    std::mutex _mutex;
    std::condition_variable _wakeup;
    std::deque<ParkedChild> _queue;
    WorkerState _workerState = WorkerState::Running;
    std::once_flag _workerStarted;
    std::thread _worker;
};

} // namespace vtpty
