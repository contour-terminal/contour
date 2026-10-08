// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PlacementBreaker.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/ScopeBus.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace vtpty
{

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
    PlacementBreaker::Clock::duration breakerCooldown = std::chrono::seconds { 30 };

    /// A memory ceiling for every scope. Tests only: a user-facing limit is not a feature yet.
    std::optional<MemoryLimit> memoryLimit {};

    /// The clock the breaker reads.
    std::function<PlacementBreaker::Clock::time_point()> now = &PlacementBreaker::Clock::now;
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
    SystemdScopePlacement(SystemdScopeConfig config, ScopeBusFactory connectBus);
    ~SystemdScopePlacement() override;

    SystemdScopePlacement(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement(SystemdScopePlacement&&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement&&) = delete;

    void placeThenRelease(ParkedChild child) override;

  private:
    void run(std::stop_token const& stop);
    void place(ParkedChild const& child);
    [[nodiscard]] std::expected<void, ScopeError> request(ParkedChild const& child,
                                                          ProcessReference reference);
    void report(ParkedChild const& child, ScopeError error);

    SystemdScopeConfig _config;
    ScopeBusFactory _connectBus;
    std::unique_ptr<ScopeBus> _bus;
    PlacementBreaker _breaker;
    std::uint64_t _sequence = 0;
    bool _hasReportedFault = false; ///< Faults are told once where a user sees them, then quietly.

    std::mutex _mutex;
    std::condition_variable_any _wakeup;
    std::deque<ParkedChild> _queue;
    std::once_flag _workerStarted;
    std::jthread _worker; ///< Last: stopped and joined before the queue it reads is destroyed.
};

} // namespace vtpty
