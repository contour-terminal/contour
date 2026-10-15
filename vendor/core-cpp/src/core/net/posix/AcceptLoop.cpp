// SPDX-License-Identifier: Apache-2.0
#include <core/net/posix/AcceptLoop.hpp>

#include <core/async/Cancellation.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/detail/PeerAddress.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/net/detail/WouldBlock.hpp>
#include <core/net/posix/PosixSocket.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace core::net
{

namespace
{

    /// The kernel's accept, for `systemAcceptCall`.
    class SystemAcceptCall final: public IAcceptCall
    {
      public:
        [[nodiscard]] AcceptAttempt accept(int listenFd,
                                           sockaddr_storage& peer,
                                           socklen_t& peerLen) noexcept override
        {
#ifdef __linux__
            auto const conn = ::accept4(
                listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
            auto const conn = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
#endif
            if (conn < 0)
                return AcceptAttempt { .fd = -1, .error = errno };
#ifndef __linux__
            // Portable fallback: non-blocking explicitly; close-on-exec is the accept loop's
            // stream-socket helper's.
            if (auto const flags = ::fcntl(conn, F_GETFL, 0); flags >= 0)
                ::fcntl(conn, F_SETFL, flags | O_NONBLOCK);
#endif
            return AcceptAttempt { .fd = conn, .error = 0 };
        }
    };

} // namespace

IAcceptCall& systemAcceptCall() noexcept
{
    // Stateless, so one instance serves every listener on every thread.
    static auto instance = SystemAcceptCall {};
    return instance;
}

namespace detail
{

    namespace
    {

        /// The failed accepts the loop takes again at once, and nothing else: `EINTR`, and the
        /// pending-connection errors Linux's accept(2) says to treat like EAGAIN that no category
        /// of `NetErrorCode` names. Each one dequeued the connection it reports on, so retrying
        /// consumes the backlog rather than spinning on it. The man page's list goes on --
        /// `EHOSTUNREACH`, `ENETUNREACH`, `EHOSTDOWN`, `ENETDOWN`, `ENONET`, and `EPERM` for a packet
        /// filter -- but those HAVE a category, so they are reported as it, and a caller that tells
        /// a per-connection failure from exhaustion can. `EOPNOTSUPP` is on that list too and is
        /// reported, as `Unsupported`, deliberately: it is also what `accept` answers on a socket
        /// that is not a stream, on every call, and retrying that would never stop.
        constexpr auto RetriedAcceptErrors = std::array { EINTR, ECONNABORTED, EPROTO, ENOPROTOOPT };

        /// One `errno` that means something narrower from `accept` than from every other socket
        /// call, and the category it is reported as.
        struct AcceptOwnClassification
        {
            int systemCode;    ///< The `errno` value.
            NetErrorCode code; ///< What the accept loop reports it as.
        };

        /// The errors `accept` classifies itself, ahead of the shared table. `EINVAL` from `accept`
        /// says the socket is not listening (or the flags are bad): a listener that is not one, as
        /// permanent as `EBADF` and `ENOTSOCK`, which the shared table already calls `BadHandle`.
        /// The shared table cannot say so, because `EINVAL` from another call is an argument error
        /// that says nothing about the handle; left `SystemError`, a caller backs off and tries a
        /// dead listener again forever.
        ///
        /// `ETIMEDOUT` from `accept` is a network error pending on ONE new connection -- the peer
        /// went silent before the handshake finished -- where from a receive with a deadline armed
        /// it is the deadline: the shared table's `Timeout`, which an accept loop reads as its own
        /// poll ticking and says nothing about. As `HostUnreach` it is what it is: one connection's
        /// failure, warned about and counted toward the loop's yield.
        constexpr auto AcceptOwnClassifications = std::array {
            AcceptOwnClassification { .systemCode = EINVAL, .code = NetErrorCode::BadHandle },
            AcceptOwnClassification { .systemCode = ETIMEDOUT, .code = NetErrorCode::HostUnreach },
        };

    } // namespace

    AcceptFailure acceptFailureOf(int err) noexcept
    {
        if (isWouldBlock(err))
            return AcceptFailure { .step = AcceptStep::Park, .code = NetErrorCode::Ok };
        if (std::ranges::find(RetriedAcceptErrors, err) != RetriedAcceptErrors.end())
            return AcceptFailure { .step = AcceptStep::Retry, .code = NetErrorCode::Ok };
        if (auto const own =
                std::ranges::find(AcceptOwnClassifications, err, &AcceptOwnClassification::systemCode);
            own != AcceptOwnClassifications.end())
            return AcceptFailure { .step = AcceptStep::Report, .code = own->code };
        return AcceptFailure { .step = AcceptStep::Report, .code = classifySocketError(err) };
    }

} // namespace detail

async::Task<AcceptResult> acceptOne(EventLoop* loop,
                                    int const* fd,
                                    bool const* closed,
                                    std::weak_ptr<void const> listener,
                                    IAcceptCall* acceptCall)
{
    while (true)
    {
        if (*closed || *fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept on closed listener"));

        auto peer = sockaddr_storage {};
        auto peerLen = socklen_t { sizeof(peer) };
        auto const attempt = acceptCall->accept(*fd, peer, peerLen);
        if (attempt.fd >= 0)
        {
            auto const conn = attempt.fd;
            // What a dialled socket is given too -- TCP_NODELAY above all, which only the dial
            // used to set, so a server's replies waited on Nagle while its client's did not. The
            // buffer sizes are not asked for here: the socket inherited them from the listener,
            // which asked before it listened.
            detail::applyStreamSocketOptions(conn, KeepAlive::No);
            co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, conn, formatPeer(peer)));
        }

        auto const err = attempt.error;
        auto const failure = detail::acceptFailureOf(err);
        if (failure.step == detail::AcceptStep::Retry)
            continue;
        if (failure.step == detail::AcceptStep::Report)
            co_return std::unexpected(makeNetError(failure.code, err, "accept"));
        // AcceptStep::Park: wait until the listener fd is readable (a connection is pending). A
        // cancelled wait (listener closed / stop requested) throws OperationCancelled, which the
        // accept loop turns into Cancelled.
        try
        {
            co_await loop->waitReadable(*fd);
        }
        catch (async::OperationCancelled const&)
        {
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept cancelled"));
        }
        catch (FdRegistrationFailed const& refused)
        {
            // The loop could not watch the listening descriptor THIS time. Reported as what the
            // backend said, which its one socket-error table already classified: a refusal for want
            // of kernel memory or descriptors (`ENOMEM`, `EMFILE` from the duplicate registration's
            // `dup`) is exhaustion an accept loop backs off on, and one nothing classifies is
            // backed off on too -- the registration is not resident, so the next accept asks
            // again. Only a refusal that says the descriptor itself is bad, or says nothing, is a
            // dead listener. Thrown on, it would end the loop as an exception nobody reports.
            //
            // And a refusal is never a CLOSE: one whose errno classifies as `Cancelled` (`EINTR`)
            // would read as the listener's own close and stop the loop quietly with the port open,
            // so it is a failure nothing classifies instead, which is backed off on.
            auto code = refused.reason.code;
            if (code == NetErrorCode::Ok)
                code = NetErrorCode::BadHandle;
            else if (code == NetErrorCode::Cancelled)
                code = NetErrorCode::SystemError;
            co_return std::unexpected(makeNetError(code,
                                                   refused.reason.systemCode,
                                                   "accept: the loop could not watch the listener ("
                                                       + refused.reason.toString() + ")"));
        }
        // Asked before `*closed` and `*fd` are: the listener's `close()` woke this park, and
        // its owner may have destroyed it before the loop got here.
        if (listener.expired())
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "the listener was destroyed"));
    }
}

} // namespace core::net
