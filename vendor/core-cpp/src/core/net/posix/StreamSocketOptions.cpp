// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/StreamSocketOptions.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <optional>
#include <tuple>

#include <fcntl.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

namespace core::net::detail
{

namespace
{
    /// Whole seconds, and never zero.
    ///
    /// The keepalive options take seconds, and a zero is not "immediately" — it is rejected, or
    /// read as "keep the default", depending on the option and the platform. Rounding a
    /// sub-second request down to nothing would leave the two-hour system default in place while
    /// reporting success, which is exactly the silently-unarmed state @c KeepAliveSettings says
    /// is worth nothing.
    /// @param value The requested interval.
    /// @return At least one second.
    [[nodiscard]] int wholeSeconds(std::chrono::milliseconds value) noexcept
    {
        auto const whole = std::chrono::ceil<std::chrono::seconds>(value);
        return whole.count() > 0 ? static_cast<int>(whole.count()) : 1;
    }

    /// Arms TCP keepalive with @p settings. Best-effort by contract; see the header.
    /// @param fd The connected socket.
    /// @param settings The intervals to apply.
    /// @return True when the flag AND the intervals were all applied.
    [[nodiscard]] bool armKeepAlive(int fd, KeepAliveSettings const& settings) noexcept
    {
        // macOS spells the idle time `TCP_KEEPALIVE`; it is `TCP_KEEPIDLE` everywhere else.
#ifdef __APPLE__
        constexpr int IdleOption = TCP_KEEPALIVE;
#else
        constexpr int IdleOption = TCP_KEEPIDLE;
#endif
        auto const idle = wholeSeconds(settings.idle);
        auto const interval = wholeSeconds(settings.interval);
        auto const count = static_cast<int>(settings.count);

        // **The INTERVALS FIRST, and the flag last.** Reversed, a socket whose intervals could
        // not be applied would be left probing on the system default — two hours on Linux —
        // which is indistinguishable from no keepalive at all for every deadline this protects,
        // while reading back as armed to anything that checks the flag.
        if (::setsockopt(fd, IPPROTO_TCP, IdleOption, &idle, sizeof(idle)) != 0)
            return false;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0)
            return false;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0)
            return false;

        int const on = 1;
        return ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0;
    }

} // namespace

namespace
{
    /// Asks for @p size bytes of the buffer @p option names, if a size was asked for at all.
    /// @param fd The socket.
    /// @param option `SO_SNDBUF` or `SO_RCVBUF`.
    /// @param size The requested size; unset leaves the kernel's value alone.
    void requestBuffer(int fd, int option, std::optional<std::size_t> size) noexcept
    {
        if (!size.has_value())
            return;
        auto const bytes = static_cast<int>(std::min<std::size_t>(*size, INT_MAX));
        std::ignore = ::setsockopt(fd, SOL_SOCKET, option, &bytes, sizeof(bytes));
    }

    /// @param fd The socket.
    /// @param level The option's level.
    /// @param option The option.
    /// @return What it reads back as, or 0 where the kernel will not say.
    [[nodiscard]] int readOption(int fd, int level, int option) noexcept
    {
        auto value = 0;
        auto length = static_cast<socklen_t>(sizeof(value));
        if (::getsockopt(fd, level, option, &value, &length) != 0)
            return 0;
        return value;
    }
} // namespace

void applyStreamSocketOptions(platform::NativeHandle socket, KeepAlive keepAlive) noexcept
{
    // Close-on-exec. Already set where the descriptor was made -- `accept4` and `makeStreamSocket`
    // ask for it atomically where they can -- and set again here because not every platform
    // offers that, and because this is the one place that can promise it for every socket.
    if (auto const flags = ::fcntl(socket, F_GETFD, 0); flags >= 0)
        std::ignore = ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC);

    // So a small write is not held back waiting for the peer's ACK of an earlier segment. An
    // AF_UNIX socket refuses it, which is fine.
    int const one = 1;
    std::ignore = ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (keepAlive == KeepAlive::Yes)
        std::ignore = armKeepAlive(socket, KeepAliveSettings {});
}

void applySocketBufferSizes(platform::NativeHandle socket, SocketBufferSizes const& sizes) noexcept
{
    requestBuffer(socket, SO_SNDBUF, sizes.send);
    requestBuffer(socket, SO_RCVBUF, sizes.receive);
}

StreamSocketReport reportStreamSocketOptions(platform::NativeHandle socket) noexcept
{
    auto const clampedSize = [](int value) {
        return static_cast<std::size_t>(std::max(value, 0));
    };
    return StreamSocketReport {
        .noDelay = readOption(socket, IPPROTO_TCP, TCP_NODELAY) != 0,
        .sendBuffer = clampedSize(readOption(socket, SOL_SOCKET, SO_SNDBUF)),
        .receiveBuffer = clampedSize(readOption(socket, SOL_SOCKET, SO_RCVBUF)),
    };
}

} // namespace core::net::detail
