// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One cross-platform peer-address formatter. inet_ntop is portable (POSIX and
/// Windows/ws2tcpip alike), so the same body serves the POSIX accept loop and
/// the Windows listener — a change (adding the port, IPv6 scope handling) lands
/// once instead of in each platform's listener.

// winsock2.h MUST precede any windows.h a later include pulls in.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif
// clang-format on

#include <array>
#include <string>

namespace net
{

/// Formats a connected peer's address as a printable host string.
/// @param addr The peer's address storage.
/// @return The printable host ("127.0.0.1", "::1"), or "" when the peer is not
///         an IPv4/IPv6 endpoint (e.g. an AF_UNIX connection) or cannot format.
[[nodiscard]] inline std::string formatPeer(sockaddr_storage const& addr) noexcept
{
    auto buf = std::array<char, INET6_ADDRSTRLEN> {};
    if (addr.ss_family == AF_INET)
    {
        auto const* v4 = reinterpret_cast<sockaddr_in const*>(&addr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, buf.data(), buf.size()) != nullptr)
            return buf.data();
    }
    else if (addr.ss_family == AF_INET6)
    {
        auto const* v6 = reinterpret_cast<sockaddr_in6 const*>(&addr);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, buf.data(), buf.size()) != nullptr)
            return buf.data();
    }
    return {};
}

} // namespace net
