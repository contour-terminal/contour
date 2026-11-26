// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>

#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

/// Whether an extension's host may be started.
enum class BreakerState : uint8_t
{
    Closed, ///< It may: its host died fewer than `faultsBeforeDisable` times within the window.
    Open,   ///< It may not, until the configuration is reloaded.
};

/// What an instance was admitted under: the breaker's epoch then. A host death it reports counts only while
/// no other report or reset came in between.
struct HostAdmission
{
    uint64_t epoch = 0; ///< The breaker's epoch when the instance was admitted.
};

/// Counts an extension's host deaths, once each however many panes saw it, and stops starting its host once
/// it died `faultsBeforeDisable` times within `faultWindow`: an extension whose host keeps dying then spawns
/// nothing more until the configuration is reloaded (reset()).
///
/// A guest's own faults (traps, deadlines) are no host death; each pane's Runner counts those itself.
///
/// **One death, once.** Every instance is admitted under the breaker's current epoch, and reports its host's
/// death with that admission. The first report of an epoch counts and advances the epoch; every other pane
/// admitted under the same epoch saw the same death, and its report is stale. An instance admitted between a
/// death and its first report undercounts: one such death in a race, never more.
///
/// Thread-safe: panes report from their lanes, the owner's thread resets.
class HostBreaker
{
  public:
    /// @param clock What the fault window is measured by; must outlive the breaker.
    /// @param faultsBeforeDisable How many deaths within @p faultWindow open the breaker.
    /// @param faultWindow The window deaths are counted within.
    HostBreaker(core::platform::IClock& clock,
                std::size_t faultsBeforeDisable,
                std::chrono::seconds faultWindow);

    /// Admits an instance to be created.
    /// @return What it reports its host's death with; `HostUnavailable` while the breaker is open.
    [[nodiscard]] std::expected<HostAdmission, ExtensionError> admit() noexcept;

    /// Counts a host death an instance saw, unless another report or a reset came since it was admitted.
    /// @param admission What the instance was admitted under.
    void recordHostFault(HostAdmission admission) noexcept;

    /// Closes the breaker and forgets every death: the configuration was reloaded. Reports of instances
    /// admitted before count no more.
    void reset() noexcept;

    /// @return Whether the host may be started.
    [[nodiscard]] BreakerState state() const noexcept;

    /// @return How many deaths were counted within the fault window, as of now.
    [[nodiscard]] std::size_t faultCount() const noexcept;

  private:
    core::platform::IClock& _clock;
    std::size_t _faultsBeforeDisable;
    std::chrono::seconds _faultWindow;
    mutable std::mutex _mutex;
    std::deque<core::platform::SteadyTimePoint> _faults; ///< Within the window, oldest first; guarded.
    uint64_t _epoch = 0;                                 ///< Advances with every counted death; guarded.
    BreakerState _state = BreakerState::Closed;          ///< Guarded by _mutex.
};

} // namespace vtextension
