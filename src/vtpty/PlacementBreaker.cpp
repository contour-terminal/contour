// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PlacementBreaker.hpp>

namespace vtpty
{

bool PlacementBreaker::shouldAttempt(Clock::time_point now) noexcept
{
    if (_state == BreakerState::Open && now - _openedAt >= _cooldown)
        _state = BreakerState::HalfOpen;
    return _state != BreakerState::Open;
}

void PlacementBreaker::record(std::expected<void, ScopeError> const& outcome, Clock::time_point now) noexcept
{
    if (outcome || traitsOf(outcome.error()).health == BusHealth::Responsive)
    {
        _state = BreakerState::Closed;
        return;
    }
    _state = BreakerState::Open;
    _openedAt = now;
}

} // namespace vtpty
