// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A SaveScheduler a test fires by hand, so a debounce is asserted without waiting for it.

#include <contour/config/SettingsEditSession.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <utility>

namespace contour::test
{

/// Records what is scheduled and runs it only when the test calls fire().
class ManualSaveScheduler final: public config::SaveScheduler
{
  public:
    void schedule(std::chrono::milliseconds delay, std::function<void()> fire) override
    {
        _delay = delay;
        _fire = std::move(fire);
        ++_scheduleCount;
    }

    void cancel() override
    {
        _delay.reset();
        _fire = nullptr;
    }

    /// @return The delay of the fire pending; nullopt when none is.
    [[nodiscard]] std::optional<std::chrono::milliseconds> pendingDelay() const { return _delay; }

    /// Runs the pending fire, as its timer would, and clears it. Does nothing when none is pending.
    void fire()
    {
        auto fire = std::exchange(_fire, nullptr);
        _delay.reset();
        if (fire)
            fire();
    }

    /// @return How many times schedule() was called.
    [[nodiscard]] int scheduleCount() const noexcept { return _scheduleCount; }

  private:
    std::optional<std::chrono::milliseconds> _delay;
    std::function<void()> _fire;
    int _scheduleCount = 0;
};

} // namespace contour::test
