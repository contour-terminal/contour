// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/StreamSocketOptions.hpp>

#include <core/net/IpSocketAddress.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
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

} // namespace

std::expected<void, NetError> armKeepAlive(platform::NativeHandle socket, KeepAliveSettings const& settings)
{
    auto const idle = wholeSeconds(settings.idle);
    auto const interval = wholeSeconds(settings.interval);
    auto const count = static_cast<int>(settings.count);

    // **The INTERVALS FIRST, and the flag last.** Reversed, a socket whose intervals could
    // not be applied would be left probing on the system default -- two hours on Linux --
    // which is indistinguishable from no keepalive at all for every deadline this protects,
    // while reading back as armed to anything that checks the flag.
    //
    // Through the option table's rows, which name the platform's spelling of each (macOS calls the
    // idle time `TCP_KEEPALIVE`). They are TCP-level and the same for both families, so the IPv4
    // rows serve an IPv6 socket too.
    auto const apply = [socket](SocketOption option, int value) {
        return writeSocketOption(socket, AddressFamily::Ipv4, option, static_cast<std::uint64_t>(value));
    };
    return apply(SocketOption::KeepAliveIdleTime, idle)
        .and_then([&] { return apply(SocketOption::KeepAliveInterval, interval); })
        .and_then([&] { return apply(SocketOption::KeepAliveCount, count); })
        .and_then([&] { return apply(SocketOption::KeepAliveEnabled, 1); });
}

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

    /// @return A keepalive option through its row, as @c armKeepAlive sets it; 0 if unreadable.
    [[nodiscard]] int keepAliveRow(int fd, SocketOption option) noexcept
    {
        return static_cast<int>(readSocketOption(fd, AddressFamily::Ipv4, option).value_or(0));
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
        // Best-effort: a dial does not fail over it. A failure builds a `NetError` (an allocated string)
        // inside this `noexcept` function, so `bad_alloc` here terminates -- accepted: an allocation
        // failure that small is the process's end on every other path too.
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
        .keepAlive = readOption(socket, SOL_SOCKET, SO_KEEPALIVE) != 0,
        .keepAliveIdleSeconds = keepAliveRow(socket, SocketOption::KeepAliveIdleTime),
        .keepAliveIntervalSeconds = keepAliveRow(socket, SocketOption::KeepAliveInterval),
        .keepAliveCount = keepAliveRow(socket, SocketOption::KeepAliveCount),
    };
}

} // namespace core::net::detail
