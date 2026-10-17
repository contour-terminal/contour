// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <expected>
#include <optional>

namespace vtpty
{

/// Decides whether to ask systemd at all.
///
/// A wedged bus would cost every new session the whole deadline. Once it stops responding,
/// requests stop until a cooldown has passed; the next request is then a trial, and its outcome
/// decides again. Any answer from systemd -- a refusal included -- shows the bus is alive; an answer
/// heard only after the deadline counts as the silence it was. While answers keep going astray, the
/// children deferred meanwhile are therefore placed late at one trial per cooldown.
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

    /// @param now The current time.
    /// @return The failure that holds requests off until the cooldown has passed, or nothing when a
    ///         request may be sent now.
    [[nodiscard]] std::optional<ScopeError> heldOffBy(core::platform::SteadyTimePoint now) const noexcept
    {
        if (shouldAttempt(now))
            return std::nullopt;
        return _silence;
    }

    /// @param now The current time.
    /// @return How long requests are still held off: zero once one may be sent.
    [[nodiscard]] core::platform::SteadyDuration cooldownLeft(
        core::platform::SteadyTimePoint now) const noexcept
    {
        return std::max(_retryAfter - now, core::platform::SteadyDuration::zero());
    }

    /// Records the outcome of a request.
    /// @param outcome What the request came back with.
    /// @param now     The current time.
    void record(std::expected<void, ScopeError> const& outcome, core::platform::SteadyTimePoint now) noexcept
    {
        auto const responsive = outcome || traitsOf(outcome.error()).health == BusHealth::Responsive;
        _retryAfter = responsive ? core::platform::SteadyTimePoint {} : now + _cooldown;
        if (!responsive)
            _silence = outcome.error();
    }

  private:
    core::platform::SteadyDuration _cooldown;
    core::platform::SteadyTimePoint _retryAfter {};
    ScopeError _silence = ScopeError::TimedOut; ///< What the bus last fell silent with; read while held off.
};

} // namespace vtpty
