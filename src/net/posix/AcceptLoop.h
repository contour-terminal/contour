// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The POSIX accept loop shared by UnixListener and PosixListener: one
/// definition of the accept / EAGAIN-park / EINTR-retry / error-map machinery,
/// so a fix to the accept or cancellation logic cannot drift between them.

#ifndef _WIN32

    #include <sys/socket.h>

    #include <string>

    #include <coro/Task.hpp>
    #include <net/IListener.h>

namespace net
{

class EventLoop;

/// Formats a connected peer's address as a printable host string.
/// @param addr The peer's address storage.
/// @return The printable host ("127.0.0.1", "::1"), or "" when the peer is not
///         an IPv4/IPv6 endpoint (e.g. an AF_UNIX connection) or cannot format.
[[nodiscard]] std::string formatPeer(sockaddr_storage const& addr) noexcept;

/// One shared accept turn-loop: accepts a connection (recording the peer via
/// formatPeer -- empty for AF_UNIX), parking on the listener fd until it is
/// readable on EAGAIN, retrying EINTR/ECONNABORTED, and mapping the rest to a
/// NetError. A closed or cancelled listener yields NetErrorCode::Cancelled.
/// Pointers, not references: a coroutine must not take reference parameters
/// (they would dangle across a suspension). The owning listener outlives the
/// accept task, so its live @c _fd / @c _closed are read through the pointers.
/// @param loop The reactor the accepted socket and the readable-wait bind to.
/// @param fd The listening fd (already non-blocking); read live so the owner's
///        close() (which drops it below 0) is observed between turns.
/// @param closed The owning listener's closed flag, read live.
[[nodiscard]] coro::Task<AcceptResult> acceptOne(EventLoop* loop, int const* fd, bool const* closed);

} // namespace net

#endif // !_WIN32
