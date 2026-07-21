// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Shared POSIX fd helpers for the socket/listener implementations.

#include <sys/socket.h>

#include <utility>

#include <fcntl.h>

namespace net
{

/// Makes @p fd non-blocking and close-on-exec.
/// @return True on success.
[[nodiscard]] inline bool makeNonBlockingCloexec(int fd) noexcept
{
    auto const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;
    auto const fdFlags = ::fcntl(fd, F_GETFD, 0);
    return fdFlags >= 0 && ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) >= 0;
}

/// Creates a non-blocking, close-on-exec stream socket portably: Linux sets the
/// flags atomically on socket(); macOS/BSD (which lack SOCK_NONBLOCK/SOCK_CLOEXEC
/// as socket() type flags) create it plain and set the flags best-effort via fcntl.
/// @param family Address family (AF_UNIX / AF_INET / AF_INET6).
/// @param protocol Protocol (usually 0).
/// @return The fd, or -1 on failure (errno set).
[[nodiscard]] inline int makeStreamSocket(int family, int protocol) noexcept
{
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
    auto const fd = ::socket(family, SOCK_STREAM, protocol);
    if (fd < 0)
        return fd;
    std::ignore = makeNonBlockingCloexec(fd);
    return fd;
#endif
}

} // namespace net
