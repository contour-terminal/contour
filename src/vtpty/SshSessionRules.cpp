// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSessionRules.hpp>

#include <algorithm>

namespace vtpty
{

std::optional<std::chrono::milliseconds> remainingSshBudget(std::chrono::steady_clock::time_point deadline,
                                                            std::chrono::steady_clock::time_point now)
{
    if (now >= deadline)
        return std::nullopt;
    auto const left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return std::max(left, std::chrono::milliseconds { 1 });
}

std::optional<std::chrono::milliseconds> sshCallTimeout(
    std::optional<std::chrono::steady_clock::time_point> deadline, std::chrono::steady_clock::time_point now)
{
    if (!deadline)
        return std::chrono::milliseconds { 0 };
    return remainingSshBudget(*deadline, now);
}

std::chrono::steady_clock::time_point saturatingDeadline(std::chrono::steady_clock::time_point start,
                                                         std::chrono::milliseconds limit) noexcept
{
    auto const room = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - start);
    if (limit >= room)
        return std::chrono::steady_clock::time_point::max();
    return start + limit;
}

std::optional<std::chrono::steady_clock::time_point> sshAgentDeadline(
    std::optional<std::chrono::milliseconds> connectLimit,
    std::chrono::steady_clock::time_point start) noexcept
{
    auto const allowance = sshAgentAllowance(connectLimit);
    if (!allowance)
        return std::nullopt;
    return saturatingDeadline(start, *allowance);
}

SshConnectBudget::SshConnectBudget(std::optional<std::chrono::milliseconds> limit,
                                   Clock::time_point now) noexcept:
    _deadline { limit ? std::optional { saturatingDeadline(now, *limit) } : std::nullopt }
{
}

void SshConnectBudget::pause(Clock::time_point now) noexcept
{
    if (!_pausedAt)
        _pausedAt = now;
}

void SshConnectBudget::resume(Clock::time_point now) noexcept
{
    if (!_pausedAt)
        return;
    if (_deadline)
    {
        auto const paused = now - *_pausedAt;
        *_deadline =
            paused >= Clock::time_point::max() - *_deadline ? Clock::time_point::max() : *_deadline + paused;
    }
    _pausedAt.reset();
}

} // namespace vtpty
