// SPDX-License-Identifier: Apache-2.0
#include <core/tui/HoverState.hpp>

#include <core/tui/TimerUtils.hpp>

namespace core::tui
{

HoverState::HoverState(std::chrono::milliseconds delay, IClock const& clock): _delay(delay), _clock(&clock)
{
}

void HoverState::onMouseMove(int x, int y, Component* target)
{
    bool const positionChanged = !_hover || _hover->x != x || _hover->y != y;

    if (positionChanged)
    {
        // If hover was confirmed and position changed, trigger leave
        if (_confirmed && _onHoverLeave)
        {
            _onHoverLeave();
        }

        // Reset hover state
        _hover = HoverInfo { .x = x, .y = y, .target = target };
        _hoverStart = _clock->now();
        _confirmed = false;
    }
    else
    {
        // Same position, just update target
        if (_hover)
            _hover->target = target;
    }
}

void HoverState::onMouseLeave()
{
    if (_confirmed && _onHoverLeave)
    {
        _onHoverLeave();
    }
    _hover.reset();
    _confirmed = false;
}

void HoverState::tick(SteadyTimePoint now)
{
    if (!_hover || _confirmed)
        return;

    auto const elapsed = now - _hoverStart;
    if (elapsed >= _delay)
    {
        _confirmed = true;
        if (_onHoverConfirmed)
        {
            _onHoverConfirmed(*_hover);
        }
    }
}

void HoverState::setOnHoverConfirmed(HoverCallback callback)
{
    _onHoverConfirmed = std::move(callback);
}

void HoverState::setOnHoverLeave(LeaveCallback callback)
{
    _onHoverLeave = std::move(callback);
}

int HoverState::timeoutMs() const
{
    if (!_hover || _confirmed)
        return -1;
    return remainingMs(*_clock, _hoverStart, _delay);
}

std::optional<HoverInfo> HoverState::currentHover() const
{
    return _hover;
}

std::optional<std::pair<int, int>> HoverState::mousePosition() const
{
    if (_hover)
        return std::make_pair(_hover->x, _hover->y);
    return std::nullopt;
}

void HoverState::setDelay(std::chrono::milliseconds delay)
{
    _delay = delay;
}

void HoverState::reset()
{
    if (_confirmed && _onHoverLeave)
    {
        _onHoverLeave();
    }
    _hover.reset();
    _confirmed = false;
}

void HoverState::forget(Component const& component)
{
    if (_hover && _hover->target == &component)
        reset();
}

} // namespace core::tui
