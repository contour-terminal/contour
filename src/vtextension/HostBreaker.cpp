// SPDX-License-Identifier: Apache-2.0
#include <vtextension/HostBreaker.hpp>

#include <algorithm>

namespace vtextension
{

HostBreaker::HostBreaker(core::platform::IClock& clock,
                         std::size_t faultsBeforeDisable,
                         std::chrono::seconds faultWindow):
    _clock { clock }, _faultsBeforeDisable { faultsBeforeDisable }, _faultWindow { faultWindow }
{
}

std::expected<HostAdmission, ExtensionError> HostBreaker::admit() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    if (_state == BreakerState::Open)
        return std::unexpected(ExtensionError::HostUnavailable);
    return HostAdmission { .epoch = _epoch };
}

void HostBreaker::recordHostFault(HostAdmission admission) noexcept
{
    auto const now = _clock.now();
    auto const lock = std::scoped_lock { _mutex };
    // Another pane reported this death already, or the configuration was reloaded since.
    if (admission.epoch != _epoch)
        return;
    ++_epoch;
    // Pruned as Runner prunes a pane's faults: a death counts while it is no older than the window.
    _faults.push_back(now);
    while (_faults.front() + _faultWindow < now)
        _faults.pop_front();
    if (_faults.size() >= _faultsBeforeDisable)
        _state = BreakerState::Open;
}

void HostBreaker::reset() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    _faults.clear();
    _state = BreakerState::Closed;
    ++_epoch;
}

BreakerState HostBreaker::state() const noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    return _state;
}

std::size_t HostBreaker::faultCount() const noexcept
{
    auto const now = _clock.now();
    auto const lock = std::scoped_lock { _mutex };
    return static_cast<std::size_t>(std::ranges::count_if(
        _faults, [&](core::platform::SteadyTimePoint fault) { return fault + _faultWindow >= now; }));
}

} // namespace vtextension
