// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <cstddef>
#include <string_view>

#include <poll.h>
#include <unistd.h>

/// Writes all of @p s to the terminal, resuming after a partial or interrupted write, and waiting for room
/// when the terminal is full: standard output can be non-blocking, a flag an earlier program can leave set on
/// the shared terminal, and a write larger than the terminal's buffer then fills it and fails with EAGAIN.
/// @param s The bytes to write; given up on when the terminal reports any other error, or accepts nothing.
inline void writeToTTY(std::string_view s) noexcept
{
    while (!s.empty())
    {
        auto const written = ::write(STDOUT_FILENO, s.data(), s.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            auto output = pollfd { .fd = STDOUT_FILENO, .events = POLLOUT, .revents = 0 };
            if (::poll(&output, 1, -1) < 0 && errno != EINTR)
                return;
            continue;
        }
        if (written <= 0)
            return;
        s.remove_prefix(static_cast<size_t>(written));
    }
}
