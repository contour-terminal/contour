// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Wakeup.hpp>

#include <cerrno>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

namespace core::platform
{

Wakeup::Wakeup()
{
    int fds[2] {};
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // macOS and BSDs may not have pipe2, use pipe + fcntl.
    if (::pipe(fds) != 0)
        throw std::runtime_error("Failed to create self-pipe for Wakeup");
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#else
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0)
        throw std::runtime_error("Failed to create self-pipe for Wakeup");
#endif
    _handle = fds[0];  // read end
    _writeFd = fds[1]; // write end
}

Wakeup::~Wakeup()
{
    if (_handle != InvalidHandle)
        ::close(_handle);
    if (_writeFd != InvalidHandle)
        ::close(_writeFd);
}

void Wakeup::signal() const
{
    auto const byte = char { 1 };
    while (::write(_writeFd, &byte, 1) == -1)
    {
        if (errno == EINTR)
            continue;
        break; // EAGAIN means pipe buffer is full — already signaled.
    }
}

void Wakeup::reset() const
{
    auto buf = char {};
    // Drain all bytes from the read end.
    while (::read(_handle, &buf, 1) > 0)
        ;
}

auto Wakeup::nativeHandle() const noexcept -> NativeHandle
{
    return _handle;
}

} // namespace core::platform
