// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/SocketErrors.hpp>

#include <cerrno>

namespace core::net::detail
{

NetErrorCode classifySocketError(int systemCode) noexcept
{
    // A switch rather than a table of pairs only because EAGAIN and EWOULDBLOCK may be one value,
    // which a table would have to state twice and a switch refuses to compile twice.
    switch (systemCode)
    {
        case ECONNRESET: return NetErrorCode::ConnReset;
        case ECONNREFUSED: return NetErrorCode::ConnRefused;
        // A route that does not exist and a host that does not answer are one category: both mean
        // this endpoint is unreachable from here, and neither is retryable at this layer. A host
        // reported down and a network that is down or absent belong with them; `accept` answers
        // all of these for one pending connection, and unclassified an accept loop would back off on
        // them, then give up on a listener that is fine.
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EHOSTDOWN:
        case ENETDOWN:
#ifdef ENONET
        case ENONET:
#endif
            return NetErrorCode::HostUnreach;
        case EADDRINUSE: return NetErrorCode::AddressInUse;
        case EADDRNOTAVAIL: return NetErrorCode::AddressNotAvail;
        // `EPERM` beside `EACCES`: a packet filter refusing a connect answers EPERM on Linux, and
        // a caller asks one question -- "was I refused by policy" -- whichever spelling it got.
        case EACCES:
        case EPERM: return NetErrorCode::PermissionDenied;
        // A family or protocol this host does not have, which a dial or an `openUdpSocket` meets
        // on a machine without IPv6.
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
        case EOPNOTSUPP: return NetErrorCode::Unsupported;
        case EBADF:
        case ENOTSOCK: return NetErrorCode::BadHandle;
        case EINTR: return NetErrorCode::Cancelled;
        case ETIMEDOUT: return NetErrorCode::Timeout;
        // `EMSGSIZE` is the whole difference between "this path cannot carry a message this big" and
        // "something went wrong": a datagram caller that cannot tell has no way to decide to split.
        case EMSGSIZE: return NetErrorCode::MessageTooLarge;
        // A receive deadline (`SO_RCVTIMEO`) expiring is EAGAIN on POSIX: `isDeadlineExpiry` is
        // what a caller asks.
        case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
        case EAGAIN:
#endif
            return NetErrorCode::WouldBlock;
        // Out of descriptors (the process's, the system's), buffer space or memory: transient where
        // `SystemError` may be permanent, which is what lets an accept loop back off on these and
        // give up on that (`AcceptPolicy.hpp`). `accept`, `socket` and a dial all answer them.
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
#ifdef ENOSR
        // Out of STREAMS resources, which accept(2) documents among its errors.
        case ENOSR:
#endif
            return NetErrorCode::ResourceExhausted;
        default: return NetErrorCode::SystemError;
    }
}

int lastSocketError() noexcept
{
    return errno;
}

} // namespace core::net::detail
