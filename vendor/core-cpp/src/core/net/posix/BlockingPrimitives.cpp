// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/BlockingPrimitives.hpp>

#include <core/net/detail/SocketErrors.hpp>
#include <core/net/posix/FdUtils.hpp> // MSG_NOSIGNAL fallback

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace core::net::detail
{

std::expected<std::size_t, NetError> receiveSome(platform::NativeHandle socket, std::span<std::byte> buffer)
{
    while (true)
    {
        auto const got = ::recv(socket, buffer.data(), buffer.size(), 0);
        if (got >= 0)
            return static_cast<std::size_t>(got);
        if (errno != EINTR)
            return std::unexpected(socketError(errno, "recv"));
    }
}

std::expected<std::size_t, NetError> waitReadable(platform::NativeHandle socket)
{
    auto probe = std::byte {};
    while (true)
    {
        // A blocking peek of one byte is the wait: it returns once something is readable, consumes
        // nothing, and tells EOF (0) from data (1) the way `ISocket::waitReadable`'s count must.
        auto const got = ::recv(socket, &probe, 1, MSG_PEEK);
        if (got == 0)
            return std::size_t { 0 };
        if (got > 0)
        {
            // The count is "how many are pending" where the kernel will say, and at least one.
            auto pending = 0;
            if (::ioctl(socket, FIONREAD, &pending) == 0 && pending > 0)
                return static_cast<std::size_t>(pending);
            return std::size_t { 1 };
        }
        if (errno != EINTR)
            return std::unexpected(socketError(errno, "recv(MSG_PEEK)"));
    }
}

std::expected<std::size_t, NetError> sendSome(platform::NativeHandle socket,
                                              std::span<std::byte const> buffer)
{
    while (true)
    {
        auto const sent = ::send(socket, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        if (sent >= 0)
            return static_cast<std::size_t>(sent);
        if (errno != EINTR)
            return std::unexpected(socketError(errno, "send"));
    }
}

std::expected<void, NetError> shutdownSend(platform::NativeHandle socket)
{
    if (::shutdown(socket, SHUT_WR) == 0 || errno == ENOTCONN)
        return {};
    return std::unexpected(socketError(errno, "shutdown"));
}

void closeSocket(platform::NativeHandle socket) noexcept
{
    if (socket != platform::InvalidHandle)
        std::ignore = ::close(socket);
}

void setIoTimeout(platform::NativeHandle socket,
                  IoDirection direction,
                  std::chrono::milliseconds timeout) noexcept
{
    // Zero is the option's own "no timeout", so a non-positive request is written as zero rather
    // than skipped: skipping would leave a bound the caller just asked to remove.
    auto const bounded = std::max(timeout, std::chrono::milliseconds { 0 });
    auto value = timeval {};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(bounded.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((bounded.count() % 1000) * 1000);
    auto const option = direction == IoDirection::Receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    std::ignore = ::setsockopt(socket, SOL_SOCKET, option, &value, sizeof(value));
}

void armNoSigPipe([[maybe_unused]] platform::NativeHandle socket) noexcept
{
    // SO_NOSIGPIPE where it exists (macOS, the BSDs), set once so every later send is covered.
    // Elsewhere the suppression is MSG_NOSIGNAL on each `sendSome`, and there is nothing to arm.
#ifdef SO_NOSIGPIPE
    int const on = 1;
    std::ignore = ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

std::expected<void, NetError> waitDialled(DialHandles const& handles, std::chrono::milliseconds timeout)
{
    auto entry = pollfd {};
    entry.fd = handles.socket;
    entry.events = POLLOUT;
    // A refused connect reports POLLOUT with POLLERR, so readiness is only "resolved"; SO_ERROR is
    // what decides. A negative timeout is poll(2)'s "wait indefinitely".
    auto const millis = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;
    while (true)
    {
        auto const ready = ::poll(&entry, 1, millis);
        if (ready > 0)
            return {};
        if (ready == 0)
            return std::unexpected(
                makeNetError(NetErrorCode::Timeout,
                             0,
                             "connect timed out after " + std::to_string(timeout.count()) + "ms"));
        if (errno != EINTR)
            return std::unexpected(socketError(errno, "poll"));
    }
}

std::expected<platform::NativeHandle, NetError> releaseBlocking(DialHandles& handles)
{
    auto const fd = std::exchange(handles.socket, platform::InvalidHandle);
    handles = DialHandles {};
    auto const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
    {
        auto const error = socketError(errno, "fcntl(~O_NONBLOCK)");
        closeSocket(fd);
        return std::unexpected(error);
    }
    return fd;
}

} // namespace core::net::detail
