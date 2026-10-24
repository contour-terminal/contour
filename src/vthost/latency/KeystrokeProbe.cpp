// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/KeystrokeProbe.hpp>

#include <algorithm>
#include <utility>

namespace vthost::latency
{

KeystrokeProbe::KeystrokeProbe(core::platform::IClock const& clock, ProbePlan plan):
    _clock { clock }, _plan { plan }, _random { plan.seed }, _startedAt { clock.now() }
{
}

core::platform::SteadyDuration KeystrokeProbe::drawGap()
{
    // A plain modulo rather than std::uniform_int_distribution, whose draws differ between standard
    // libraries: the schedule of a seed is the same on every platform.
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    auto const low = duration_cast<milliseconds>(_plan.minGap).count();
    auto const high = std::max(low, duration_cast<milliseconds>(_plan.maxGap).count());
    auto const span = static_cast<uint32_t>(high - low) + 1;
    return milliseconds { low + static_cast<std::int64_t>(_random() % span) };
}

void KeystrokeProbe::pauseFrom(core::platform::SteadyTimePoint now)
{
    _dueAt = now + drawGap();
    _result.phase = ProbePhase::Pausing;
}

void KeystrokeProbe::frameSwapped(std::optional<int> drawnCursorColumn)
{
    if (!drawnCursorColumn)
        return;
    auto const now = _clock.now();
    auto const lock = std::scoped_lock { _mutex };
    switch (_result.phase)
    {
        case ProbePhase::AwaitingPrompt:
            if (*drawnCursorColumn != PromptColumns)
                return;
            _result.firstPrompt = now;
            pauseFrom(now);
            return;
        case ProbePhase::AwaitingEcho: {
            if (*drawnCursorColumn != expectedCursorColumn(_keyIndex))
                return;
            if (_keyIndex >= _plan.warmupKeys)
                _result.keystrokes.push_back(now - _keyTypedAt);
            ++_keyIndex;
            if (_keyIndex >= _plan.warmupKeys + _plan.measuredKeys)
                _result.phase = ProbePhase::Finished;
            else
                pauseFrom(now);
            return;
        }
        case ProbePhase::Pausing:
        case ProbePhase::Finished:
        case ProbePhase::PromptTimedOut:
        case ProbePhase::EchoTimedOut: return;
    }
}

std::optional<char> KeystrokeProbe::takeDueKey()
{
    auto const now = _clock.now();
    auto const lock = std::scoped_lock { _mutex };
    switch (_result.phase)
    {
        case ProbePhase::AwaitingPrompt:
            if (now - _startedAt > _plan.promptTimeout)
            {
                _result.phase = ProbePhase::PromptTimedOut;
                _result.awaitedColumn = PromptColumns;
            }
            return std::nullopt;
        case ProbePhase::Pausing:
            if (now < _dueAt)
                return std::nullopt;
            _keyTypedAt = now;
            _result.phase = ProbePhase::AwaitingEcho;
            return keyFor(_keyIndex);
        case ProbePhase::AwaitingEcho:
            if (now - _keyTypedAt > _plan.echoTimeout)
            {
                _result.phase = ProbePhase::EchoTimedOut;
                _result.awaitedColumn = expectedCursorColumn(_keyIndex);
            }
            return std::nullopt;
        case ProbePhase::Finished:
        case ProbePhase::PromptTimedOut:
        case ProbePhase::EchoTimedOut: return std::nullopt;
    }
    return std::nullopt;
}

ProbePhase KeystrokeProbe::phase() const
{
    auto const lock = std::scoped_lock { _mutex };
    return _result.phase;
}

ProbeResult KeystrokeProbe::result() const
{
    auto const lock = std::scoped_lock { _mutex };
    return _result;
}

} // namespace vthost::latency
