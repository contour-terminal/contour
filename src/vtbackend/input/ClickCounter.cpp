// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/ClickCounter.hpp>

#include <algorithm>
#include <iterator>

namespace vtbackend
{

ClickCounter::ClickCounter(SteadyClockRef clock, std::chrono::milliseconds interval) noexcept:
    _clock { clock }, _interval { interval }
{
}

std::optional<size_t> ClickCounter::slotOf(MouseButton button) noexcept
{
    auto const counted = std::ranges::find(CountedButtons, button);
    if (counted == CountedButtons.end())
        return std::nullopt;
    return static_cast<size_t>(std::distance(CountedButtons.begin(), counted));
}

uint8_t ClickCounter::press(CellLocation position, MouseButton button) noexcept
{
    auto const slot = slotOf(button);
    if (!slot)
        return 1;

    auto& sequence = _sequences[*slot];
    auto const now = _clock.now();
    auto const gap = now - sequence.lastPress;

    // A gap below zero is a clock that stepped back; trusting it would continue a sequence forever.
    auto const continues = sequence.count != 0 && position == sequence.position
                           && gap >= core::platform::SteadyDuration::zero() && gap <= _interval;

    sequence.count = continues ? static_cast<uint8_t>((sequence.count % MaxClickCount) + 1) : uint8_t { 1 };
    sequence.position = position;
    sequence.lastPress = now;
    return sequence.count;
}

void ClickCounter::reset() noexcept
{
    _sequences = {};
}

} // namespace vtbackend
