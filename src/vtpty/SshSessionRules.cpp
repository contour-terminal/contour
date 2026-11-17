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

} // namespace vtpty
