// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The POSIX accept loop shared by UnixListener and PosixListener: one
/// definition of the accept / EAGAIN-park / EINTR-retry / error-map machinery,
/// so a fix to the accept or cancellation logic cannot drift between them.

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>

#include <memory>

namespace core::net
{

class EventLoop;

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
/// @param listener The owning listener's lifetime token. `close()` wakes a parked accept through
///        the loop, a turn later, and the owner may destroy the listener in between: once this has
///        expired, @p fd and @p closed dangle, and the accept answers Cancelled without them.
[[nodiscard]] async::Task<AcceptResult> acceptOne(EventLoop* loop,
                                                  int const* fd,
                                                  bool const* closed,
                                                  std::weak_ptr<void const> listener);

} // namespace core::net
