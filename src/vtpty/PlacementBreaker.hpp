// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>

#include <chrono>
#include <cstdint>
#include <expected>

namespace vtpty
{

/// Where a PlacementBreaker stands.
enum class BreakerState : std::uint8_t
{
    Closed,   ///< Requests are sent.
    Open,     ///< The bus did not respond; requests are skipped until the cooldown has passed.
    HalfOpen, ///< The cooldown passed; the next request is a trial that decides.
};

/// Decides whether to ask systemd at all.
///
/// A wedged bus would cost every new session the whole deadline. Once it stops responding, requests
/// stop until a cooldown has passed, and then one trial request decides whether to resume. Any
/// answer from systemd -- a refusal included -- shows the bus is alive, and closes the breaker.
class PlacementBreaker
{
  public:
    using Clock = std::chrono::steady_clock;

    /// @param cooldown How long to skip requests after the bus stopped responding.
    explicit PlacementBreaker(Clock::duration cooldown) noexcept: _cooldown { cooldown } {}

    /// @param now The current time.
    /// @return Whether to send a request now. An Open breaker whose cooldown has passed becomes
    ///         HalfOpen, and answers yes.
    [[nodiscard]] bool shouldAttempt(Clock::time_point now) noexcept;

    /// Records the outcome of a request.
    /// @param outcome What the request came back with.
    /// @param now     The current time.
    void record(std::expected<void, ScopeError> const& outcome, Clock::time_point now) noexcept;

    /// @return Where the breaker stands.
    [[nodiscard]] BreakerState state() const noexcept { return _state; }

  private:
    Clock::duration _cooldown;
    BreakerState _state = BreakerState::Closed;
    Clock::time_point _openedAt {};
};

} // namespace vtpty
