// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The shared Windows loopback-socketpair helper. Windows lacks socketpair(2),
/// so both the SystemPipe and the in-memory test transport need a connected TCP
/// pair over the loopback interface. Single-sourced here so an error-path or
/// leak fix in the bind/listen/connect/accept dance lands in one place.

// winsock2.h defines SOCKET; keep it ahead of any windows.h a later include pulls in.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
#endif
// clang-format on

#ifdef _WIN32

    #include <array>

namespace net
{

/// Creates a connected loopback TCP socket pair.
/// @param out The two connected sockets {accepted-server, client} on success.
/// @return True on success (both sockets valid); false otherwise, with nothing
///         left open (every partial path closes what it opened).
[[nodiscard]] bool makeLoopbackPair(std::array<SOCKET, 2>& out) noexcept;

} // namespace net

#endif // _WIN32
