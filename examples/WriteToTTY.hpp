// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <cstddef>
#include <string_view>

#include <unistd.h>

/// Writes all of @p s to the terminal, resuming after a partial or interrupted write.
/// @param s The bytes to write; given up on when the terminal reports any other error, or accepts nothing.
inline void writeToTTY(std::string_view s) noexcept
{
    while (!s.empty())
    {
        auto const written = ::write(STDOUT_FILENO, s.data(), s.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        s.remove_prefix(static_cast<size_t>(written));
    }
}
