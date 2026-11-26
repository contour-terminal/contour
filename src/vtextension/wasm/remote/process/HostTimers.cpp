// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/HostTimers.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vtextension::wasm::remote::process
{

void HostTimers::arm(TimerKey key, core::SteadyTimePoint now, core::SteadyDuration budget)
{
    _timers.insert_or_assign(key, Entry { .remaining = budget, .runningSince = now });
}

void HostTimers::pause(TimerKey key, core::SteadyTimePoint now) noexcept
{
    auto const timer = _timers.find(key);
    if (timer == _timers.end() || !timer->second.runningSince)
        return;
    auto& entry = timer->second;
    entry.remaining = std::max(core::SteadyDuration {}, entry.remaining - (now - *entry.runningSince));
    entry.runningSince = std::nullopt;
}

void HostTimers::resume(TimerKey key, core::SteadyTimePoint now) noexcept
{
    auto const timer = _timers.find(key);
    if (timer == _timers.end() || timer->second.runningSince)
        return;
    timer->second.runningSince = now;
}

void HostTimers::disarm(TimerKey key) noexcept
{
    _timers.erase(key);
}

void HostTimers::disarmHelper(uint64_t helper) noexcept
{
    std::erase_if(_timers, [helper](auto const& timer) { return timer.first.helper == helper; });
}

std::vector<TimerKey> HostTimers::expire(core::SteadyTimePoint now)
{
    auto expired = std::vector<TimerKey> {};
    std::erase_if(_timers, [&](auto const& timer) {
        auto const deadline = deadlineOf(timer.second);
        if (!deadline || *deadline > now)
            return false;
        expired.push_back(timer.first);
        return true;
    });
    return expired;
}

std::optional<core::SteadyTimePoint> HostTimers::nextDeadline() const noexcept
{
    auto earliest = std::optional<core::SteadyTimePoint> {};
    for (auto const& [key, entry]: _timers)
        if (auto const deadline = deadlineOf(entry); deadline && (!earliest || *deadline < *earliest))
            earliest = deadline;
    return earliest;
}

std::size_t HostTimers::armed() const noexcept
{
    return _timers.size();
}

std::optional<core::SteadyTimePoint> HostTimers::deadlineOf(Entry const& entry) noexcept
{
    if (!entry.runningSince)
        return std::nullopt;
    return *entry.runningSince + entry.remaining;
}

} // namespace vtextension::wasm::remote::process
