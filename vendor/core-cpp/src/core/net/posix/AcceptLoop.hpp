// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The POSIX accept loop shared by UnixListener and PosixListener: one
/// definition of the accept / EAGAIN-park / retry / error-map machinery,
/// so a fix to the accept or cancellation logic cannot drift between them.

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>

#include <sys/socket.h>

#include <cstdint>
#include <memory>

namespace core::net
{

class EventLoop;

/// One `accept(2)` as the accept loop makes it: the new descriptor, or the error it failed with.
struct AcceptAttempt
{
    int fd;    ///< The accepted descriptor, already non-blocking; negative when the call failed.
    int error; ///< The `errno` a failed call left; 0 when it succeeded.
};

/// The one system call the accept loop makes, injected so that a test can make it fail in each
/// way accept(2) can. No loopback connection can be asked for a packet filter's `EPERM` or a
/// pending `EPROTO`, and without this seam the loop's handling of them -- retry, report, park --
/// was asserted nowhere: only the pure decision (`detail::acceptFailureOf`) was.
class IAcceptCall
{
  public:
    virtual ~IAcceptCall() = default;

    /// Accepts one pending connection.
    /// @param listenFd The listening descriptor.
    /// @param peer Receives the peer's address.
    /// @param peerLen On entry the size of @p peer, on return the length of the address.
    /// @return The accepted descriptor, or the error.
    [[nodiscard]] virtual AcceptAttempt accept(int listenFd,
                                               sockaddr_storage& peer,
                                               socklen_t& peerLen) noexcept = 0;
};

/// The kernel's accept: `accept4` with `SOCK_NONBLOCK | SOCK_CLOEXEC` on Linux, `accept` and then
/// `O_NONBLOCK` elsewhere.
/// @return The process-wide instance; it holds no state.
[[nodiscard]] IAcceptCall& systemAcceptCall() noexcept;

namespace detail
{

    /// What the accept loop does with one failed `accept(2)`.
    enum class AcceptStep : std::uint8_t
    {
        Park,   ///< Nothing is pending: wait until the listener is readable, then accept again.
        Retry,  ///< Accept again at once: the call was interrupted, or the connection it dequeued had
                ///< already failed, and the listener is as good as it was.
        Report, ///< Hand the error to the caller, classified by the one table (`SocketErrors.hpp`).
    };

    /// What the accept loop does with one failed `accept(2)`, and what it reports.
    struct AcceptFailure
    {
        AcceptStep step;   ///< Park, retry, or report.
        NetErrorCode code; ///< What is reported, for `Report`; `Ok` otherwise.
    };

    /// Decides what a failed `accept(2)` means for the accept loop.
    ///
    /// Only the RETRY set is accept's own: `EINTR`, and the errors Linux's accept(2) says are
    /// "already-pending network errors on the new socket" to be treated "like EAGAIN by retrying"
    /// and that no category names -- `ECONNABORTED`, `EPROTO`, `ENOPROTOOPT`. Every other error is
    /// REPORTED through `classifySocketError`, so a per-connection answer that HAS a category
    /// reaches the caller as it: a packet filter's `EPERM` as `PermissionDenied`, `EHOSTUNREACH`,
    /// `ENETUNREACH`, `EHOSTDOWN`, `ENETDOWN` and `ENONET` as `HostUnreach` -- where it used to be
    /// `SystemError`, which an accept loop backs off on and then gives up on. Exhaustion (`EMFILE`,
    /// `ENFILE`, `ENOBUFS`, `ENOMEM`) is `ResourceExhausted`, the one category it backs off on
    /// without end. One error is accept's own to classify: `EINVAL`, a socket that is not
    /// listening, is `BadHandle` like `EBADF` and `ENOTSOCK`, not the argument error it is from
    /// other calls.
    ///
    /// This decides what ONE accept does inside itself; what a loop over accepts does with what it
    /// reports is `AcceptErrorPolicy`'s (`core/net/AcceptPolicy.hpp`), for every transport.
    /// @param err The `errno` a failed `accept(2)` left.
    /// @return What the accept loop does about it, and for `Report` the category it reports.
    [[nodiscard]] AcceptFailure acceptFailureOf(int err) noexcept;

} // namespace detail

/// One shared accept turn-loop: accepts a connection (recording the peer via
/// formatPeer -- empty for AF_UNIX), parking on the listener fd until it is
/// readable on EAGAIN, retrying what `detail::acceptFailureOf` says to, and mapping
/// the rest through the one error table. A closed or cancelled listener yields
/// NetErrorCode::Cancelled. A listening descriptor the loop refuses to watch
/// (@c FdRegistrationFailed) yields the refusal's own code -- exhaustion for want of
/// kernel memory or descriptors, which a loop backs off on -- and
/// NetErrorCode::BadHandle only where the refusal says the descriptor is bad or says
/// nothing.
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
/// @param acceptCall The system call, the kernel's unless a test scripts it. A pointer, like the
///        others, and outliving the accept task.
[[nodiscard]] async::Task<AcceptResult> acceptOne(EventLoop* loop,
                                                  int const* fd,
                                                  bool const* closed,
                                                  std::weak_ptr<void const> listener,
                                                  IAcceptCall* acceptCall = &systemAcceptCall());

} // namespace core::net
