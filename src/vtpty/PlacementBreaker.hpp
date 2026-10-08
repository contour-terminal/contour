// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>

#include <core/platform/Clock.hpp>

#include <expected>

namespace vtpty
{

/// Decides whether to ask systemd at all.
///
/// A wedged bus would cost every new session the whole deadline. Once it stops responding,
/// requests stop until a cooldown has passed; the next request is then a trial, and its outcome
/// decides again. Any answer from systemd -- a refusal included -- shows the bus is alive.
class PlacementBreaker
{
  public:
    /// @param cooldown How long to skip requests after the bus stopped responding.
    explicit PlacementBreaker(core::platform::SteadyDuration cooldown) noexcept: _cooldown { cooldown } {}

    /// @param now The current time.
    /// @return Whether to send a request now.
    [[nodiscard]] bool shouldAttempt(core::platform::SteadyTimePoint now) const noexcept
    {
        return now >= _retryAfter;
    }

    /// Records the outcome of a request.
    /// @param outcome What the request came back with.
    /// @param now     The current time.
    void record(std::expected<void, ScopeError> const& outcome, core::platform::SteadyTimePoint now) noexcept
    {
        auto const responsive = outcome || traitsOf(outcome.error()).health == BusHealth::Responsive;
        _retryAfter = responsive ? core::platform::SteadyTimePoint {} : now + _cooldown;
    }

  private:
    core::platform::SteadyDuration _cooldown;
    core::platform::SteadyTimePoint _retryAfter {};
};

} // namespace vtpty
