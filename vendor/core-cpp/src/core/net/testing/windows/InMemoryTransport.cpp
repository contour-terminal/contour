// SPDX-License-Identifier: Apache-2.0

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/testing/InMemoryTransport.hpp>

#include <core/net/Sockets.hpp>
#include <core/net/windows/WindowsLoopback.hpp>
#include <core/platform/WinsockInit.hpp>

#include <array>
#include <utility>

namespace core::net::testing
{

std::expected<SocketPair, NetError> makeSocketPair(EventLoop& loop)
{
    platform::ensureWinsockInitialized();
    auto pair = std::array<SOCKET, 2> {};
    if (!makeLoopbackPair(pair)) // the shared production helper (net/windows/WindowsLoopback)
        return std::unexpected(makeNetError(NetErrorCode::SystemError, WSAGetLastError(), "loopback pair"));

    // The socket production would hand out for this loop, which is `adoptSocket`'s question, so
    // this file does not ask it a second time.
    auto first = adoptSocket(loop, reinterpret_cast<platform::NativeHandle>(pair[0]), {});
    if (!first)
    {
        ::closesocket(pair[1]); // adoptSocket closed pair[0]; this one was never handed over
        return std::unexpected(std::move(first.error()));
    }
    auto second = adoptSocket(loop, reinterpret_cast<platform::NativeHandle>(pair[1]), {});
    if (!second)
        return std::unexpected(std::move(second.error()));
    return SocketPair { .first = std::move(*first), .second = std::move(*second) };
}

} // namespace core::net::testing
