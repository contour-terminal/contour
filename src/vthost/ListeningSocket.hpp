// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/un.h>

    #include <cerrno>

    #include <netinet/in.h>

namespace vthost
{

namespace detail
{
    /// Whether a descriptor is a listening socket, judged without SO_ACCEPTCONN: a bound stream socket with
    /// no peer. A socket bound and never connected but not listening either would pass too; nothing a daemon
    /// hands over is one.
    /// @param fd The descriptor to ask about; any number, open or not.
    /// @return true for a bound, unconnected stream socket.
    [[nodiscard]] inline bool listensByAddress(int fd) noexcept
    {
        auto type = 0;
        auto length = static_cast<socklen_t>(sizeof(type));
        if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM)
            return false;
        auto peer = sockaddr_storage {};
        auto peerLength = static_cast<socklen_t>(sizeof(peer));
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0 || errno != ENOTCONN)
            return false;
        auto own = sockaddr_storage {};
        auto ownLength = static_cast<socklen_t>(sizeof(own));
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&own), &ownLength) != 0)
            return false;
        switch (own.ss_family)
        {
            case AF_UNIX: return reinterpret_cast<sockaddr_un const&>(own).sun_path[0] != '\0';
            case AF_INET: return reinterpret_cast<sockaddr_in const&>(own).sin_port != 0;
            case AF_INET6: return reinterpret_cast<sockaddr_in6 const&>(own).sin6_port != 0;
            default: return false;
        }
    }
} // namespace detail

/// Whether a descriptor is a socket listening for connections.
///
/// SO_ACCEPTCONN answers this directly where the system supports it as a socket option. macOS does not, and
/// there the descriptor's address decides (detail::listensByAddress).
/// @param fd The descriptor to ask about; any number, open or not.
/// @return true for a listening socket, false for anything else, a closed number included.
[[nodiscard]] inline bool isListeningSocket(int fd) noexcept
{
    auto listening = 0;
    auto length = static_cast<socklen_t>(sizeof(listening));
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length) == 0)
        return listening != 0;
    if (errno != ENOPROTOOPT && errno != EINVAL)
        return false; // Not a socket, or not open at all.
    return detail::listensByAddress(fd);
}

} // namespace vthost
#endif
