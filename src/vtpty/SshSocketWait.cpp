// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSocketWait.hpp>

#ifndef _WIN32

    #include <vtpty/SshSessionRules.hpp>

    #include <algorithm>
    #include <cerrno>
    #include <limits>

    #include <poll.h>

namespace vtpty
{

std::expected<void, int> waitForDescriptor(int fd,
                                           short events,
                                           std::chrono::steady_clock::time_point deadline)
{
    auto pfd = pollfd { .fd = fd, .events = events, .revents = 0 };
    while (true)
    {
        auto const left = remainingSshBudget(deadline, std::chrono::steady_clock::now());
        if (!left)
            return std::unexpected { ETIMEDOUT };
        auto const timeoutMs = static_cast<int>(
            std::min<std::chrono::milliseconds::rep>(left->count(), std::numeric_limits<int>::max()));
        // A timeout or a signal goes round again: the deadline above decides when it is over.
        auto const rc = ::poll(&pfd, 1, timeoutMs);
        if (rc > 0)
            return {};
        if (rc < 0 && errno != EINTR)
            return std::unexpected { errno };
    }
}

} // namespace vtpty

#endif
