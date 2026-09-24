// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// clang-format on

#include <core/net/detail/StreamSocketOptions.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <optional>
#include <tuple>

namespace core::net::detail
{

namespace
{
    /// Arms TCP keepalive with @p settings. Best-effort by contract; see the header.
    ///
    /// One ioctl sets the flag and both intervals together, so there is no partially-armed state
    /// to unwind. The probe COUNT is absent on purpose: Windows fixes it at 10 and offers no way
    /// to set it — @c KeepAliveSettings states what that does to the detection time rather than
    /// pretending the parameter was applied.
    /// @param socket The connected socket.
    /// @param settings The intervals to apply.
    /// @return True when the ioctl succeeded.
    [[nodiscard]] bool armKeepAlive(SOCKET socket, KeepAliveSettings const& settings) noexcept
    {
        std::ignore = settings.count;

        auto request = tcp_keepalive {};
        request.onoff = 1;
        // Milliseconds here, unlike every other platform.
        request.keepalivetime = static_cast<ULONG>(settings.idle.count());
        request.keepaliveinterval = static_cast<ULONG>(settings.interval.count());

        DWORD returned = 0;
        return ::WSAIoctl(socket,
                          SIO_KEEPALIVE_VALS,
                          &request,
                          sizeof(request),
                          nullptr,
                          0,
                          &returned,
                          nullptr,
                          nullptr)
               == 0;
    }

} // namespace

namespace
{
    /// Asks for @p size bytes of the buffer @p option names, if a size was asked for at all.
    /// @param socket The socket.
    /// @param option `SO_SNDBUF` or `SO_RCVBUF`.
    /// @param size The requested size; unset leaves the kernel's value alone.
    void requestBuffer(SOCKET socket, int option, std::optional<std::size_t> size) noexcept
    {
        if (!size.has_value())
            return;
        auto const bytes = static_cast<int>(std::min<std::size_t>(*size, INT_MAX));
        std::ignore = ::setsockopt(socket,
                                   SOL_SOCKET,
                                   option,
                                   reinterpret_cast<char const*>(&bytes),
                                   static_cast<int>(sizeof(bytes)));
    }

    /// @param socket The socket.
    /// @param level The option's level.
    /// @param option The option.
    /// @return What it reads back as, or 0 where Winsock will not say.
    [[nodiscard]] int readOption(SOCKET socket, int level, int option) noexcept
    {
        auto value = 0;
        auto length = static_cast<int>(sizeof(value));
        if (::getsockopt(socket, level, option, reinterpret_cast<char*>(&value), &length) != 0)
            return 0;
        return value;
    }
} // namespace

void applyStreamSocketOptions(platform::NativeHandle socket, KeepAlive keepAlive) noexcept
{
    auto const winSocket = reinterpret_cast<SOCKET>(socket);

    // Windows' close-on-exec: not inherited by a child process. A socket made with
    // `WSA_FLAG_NO_HANDLE_INHERIT` already is not, but one made by a plain `::socket` -- the
    // readiness dial's, or an adopted one -- is, and a process that also spawns children would hand
    // every child an open connection.
    std::ignore = ::SetHandleInformation(reinterpret_cast<HANDLE>(winSocket), HANDLE_FLAG_INHERIT, 0);

    int const one = 1;
    std::ignore =
        ::setsockopt(winSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));

    if (keepAlive == KeepAlive::Yes)
        std::ignore = armKeepAlive(winSocket, KeepAliveSettings {});
}

void applySocketBufferSizes(platform::NativeHandle socket, SocketBufferSizes const& sizes) noexcept
{
    auto const winSocket = reinterpret_cast<SOCKET>(socket);
    requestBuffer(winSocket, SO_SNDBUF, sizes.send);
    requestBuffer(winSocket, SO_RCVBUF, sizes.receive);
}

StreamSocketReport reportStreamSocketOptions(platform::NativeHandle socket) noexcept
{
    auto const winSocket = reinterpret_cast<SOCKET>(socket);
    auto const clampedSize = [](int value) {
        return static_cast<std::size_t>(std::max(value, 0));
    };
    return StreamSocketReport {
        .noDelay = readOption(winSocket, IPPROTO_TCP, TCP_NODELAY) != 0,
        .sendBuffer = clampedSize(readOption(winSocket, SOL_SOCKET, SO_SNDBUF)),
        .receiveBuffer = clampedSize(readOption(winSocket, SOL_SOCKET, SO_RCVBUF)),
    };
}

} // namespace core::net::detail
