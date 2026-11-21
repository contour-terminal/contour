// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>

#include <chrono>
#include <cstdint>

namespace orchestrator::guest
{

/// The guest's clock as core::tui and the dashboard read it: the monotonic time the driver last read from
/// IGuestHost::now(), in the engine's milliseconds. It reads no clock of its own.
class GuestClock final: public core::IClock
{
  public:
    /// Sets the time the clock reads.
    /// @param monoMs The host's monotonic milliseconds.
    void set(uint64_t monoMs) noexcept { _monoMs = monoMs; }

    /// @return The time last set.
    [[nodiscard]] core::SteadyTimePoint now() const noexcept override
    {
        return core::SteadyTimePoint { std::chrono::milliseconds { _monoMs } };
    }

  private:
    uint64_t _monoMs = 0; ///< What now() reads.
};

} // namespace orchestrator::guest
