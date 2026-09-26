// SPDX-License-Identifier: Apache-2.0

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/testing/RawSockets.hpp>

#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpSocket.hpp>
#include <core/platform/WinsockInit.hpp>

namespace core::net::testing
{

namespace
{
    /// @param handle A socket handle.
    /// @return It as Winsock's type.
    [[nodiscard]] SOCKET socketOf(platform::NativeHandle handle) noexcept
    {
        return reinterpret_cast<SOCKET>(handle);
    }

    /// @param socket A Winsock socket.
    /// @return It as the portable handle type.
    [[nodiscard]] platform::NativeHandle handleOf(SOCKET socket) noexcept
    {
        return reinterpret_cast<platform::NativeHandle>(socket);
    }
} // namespace

void closeRawSocket(platform::NativeHandle handle) noexcept
{
    if (handle != platform::InvalidHandle)
        ::closesocket(socketOf(handle));
}

platform::NativeHandle openRawTcpSocket() noexcept
{
    platform::ensureWinsockInitialized();
    return handleOf(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
}

RawConnection rawLoopbackConnection() noexcept
{
    auto const listener = RawSocket { openRawTcpSocket() };
    if (listener.get() == platform::InvalidHandle)
        return {};
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto* const generic = reinterpret_cast<sockaddr*>(&address);
    auto length = static_cast<int>(sizeof(address));
    if (::bind(socketOf(listener.get()), generic, length) != 0 || ::listen(socketOf(listener.get()), 1) != 0
        || ::getsockname(socketOf(listener.get()), generic, &length) != 0)
        return {};

    auto client = RawSocket { openRawTcpSocket() };
    if (client.get() == platform::InvalidHandle
        || ::connect(socketOf(client.get()), generic, static_cast<int>(sizeof(address))) != 0)
        return {};
    auto accepted = RawSocket { handleOf(::accept(socketOf(listener.get()), nullptr, nullptr)) };
    if (accepted.get() == platform::InvalidHandle)
        return {};
    return RawConnection { .client = client.release(), .accepted = accepted.release() };
}

bool rawSendAll(platform::NativeHandle handle, std::string_view payload) noexcept
{
    auto const sent = ::send(socketOf(handle), payload.data(), static_cast<int>(payload.size()), 0);
    return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
}

std::ptrdiff_t rawReceive(platform::NativeHandle handle, std::span<char> into) noexcept
{
    return ::recv(socketOf(handle), into.data(), static_cast<int>(into.size()), 0);
}

platform::NativeHandle nativeHandleOf(ISocket const& socket) noexcept
{
    if (auto const* iocp = dynamic_cast<IocpSocket const*>(&socket))
        return handleOf(iocp->native());
    return platform::InvalidHandle;
}

platform::NativeHandle nativeHandleOf(IListener const& listener) noexcept
{
    if (auto const* iocp = dynamic_cast<IocpListener const*>(&listener))
        return handleOf(iocp->native());
    return platform::InvalidHandle;
}

} // namespace core::net::testing
