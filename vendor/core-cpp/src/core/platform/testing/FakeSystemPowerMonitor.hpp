// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/SystemPower.hpp>

#include <cstddef>
#include <utility>

namespace core::platform::testing
{

/// An ISystemPowerMonitor for unit tests: events happen when the test says so.
class FakeSystemPowerMonitor final: public ISystemPowerMonitor
{
  public:
    [[nodiscard]] PowerSubscription subscribe(Handler handler) override
    {
        return _registry.subscribe(std::move(handler));
    }

    /// Delivers an event to every subscriber, on the calling thread. A handler that throws terminates, as
    /// it does under every native monitor; unlike those, a handler may destroy the fake.
    /// Not named `emit`: that is a macro in every Qt translation unit.
    /// @param event What happened.
    void deliver(SystemPowerEvent event) noexcept { _registry.dispatch(event); }

    /// The number of live subscriptions.
    [[nodiscard]] std::size_t subscriberCount() const { return _registry.size(); }

  private:
    detail::PowerHandlerRegistry _registry;
};

} // namespace core::platform::testing
