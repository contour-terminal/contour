// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h (which project headers pull in), so this
// block leads the translation unit, mirroring every Win32 net TU.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/net/windows/WindowsLoopback.hpp>

#include <core/net/windows/InvalidSocket.hpp>

namespace core::net
{

bool makeLoopbackPair(std::array<SOCKET, 2>& out) noexcept
{
    auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == detail::InvalidSocket)
        return false;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ask the OS for an ephemeral port
    auto cleanupListener = [&] {
        closesocket(listener);
    };

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR
        || ::listen(listener, 1) == SOCKET_ERROR)
    {
        cleanupListener();
        return false;
    }

    int addrLen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR)
    {
        cleanupListener();
        return false;
    }

    auto client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == detail::InvalidSocket)
    {
        cleanupListener();
        return false;
    }
    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(client);
        cleanupListener();
        return false;
    }

    auto server = ::accept(listener, nullptr, nullptr);
    cleanupListener();
    if (server == detail::InvalidSocket)
    {
        closesocket(client);
        return false;
    }

    out = { server, client };
    return true;
}

} // namespace core::net
