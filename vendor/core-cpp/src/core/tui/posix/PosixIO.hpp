// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <cstddef>

#include <unistd.h>

namespace core::tui
{

/// @brief EINTR-safe write: retries on signal interruption, handles short writes.
/// @param fd File descriptor to write to.
/// @param buf Pointer to the data to write.
/// @param count Number of bytes to write.
/// @return Total bytes written, or -1 on non-EINTR error.
inline auto safeWrite(int fd, void const* buf, size_t count) -> ssize_t
{
    auto const* ptr = static_cast<char const*>(buf);
    auto remaining = count;
    while (remaining > 0)
    {
        auto const written = ::write(fd, ptr, remaining);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
    return static_cast<ssize_t>(count);
}

/// @brief EINTR-safe read: retries on signal interruption.
/// @param fd File descriptor to read from.
/// @param buf Buffer to read into.
/// @param count Maximum number of bytes to read.
/// @return Bytes read, 0 on EOF, or -1 on non-EINTR error.
inline auto safeRead(int fd, void* buf, size_t count) -> ssize_t
{
    while (true)
    {
        auto const n = ::read(fd, buf, count);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

} // namespace core::tui
