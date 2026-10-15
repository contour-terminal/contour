// SPDX-License-Identifier: Apache-2.0

// clang-format off
#include <winsock2.h>
// clang-format on

#include <core/net/detail/SocketErrors.hpp>

namespace core::net::detail
{

NetErrorCode classifySocketError(int systemCode) noexcept
{
    switch (systemCode)
    {
        case WSAECONNRESET: return NetErrorCode::ConnReset;
        case WSAECONNREFUSED: return NetErrorCode::ConnRefused;
        // One category for all three, for the reason the POSIX table gives. NOT `WSAENETDOWN`,
        // although the POSIX table has `ENETDOWN`: Winsock answers it when the network SUBSYSTEM
        // has failed, for every call on the machine, where Linux's accept(2) answers ENETDOWN for
        // one pending connection. `AcceptEx` failures are classified by this table too, and as
        // `HostUnreach` a caller would read a machine-wide failure as one peer's and accept again
        // at once, forever; left `SystemError`, it reads as the condition it is: one an accept loop
        // backs off on without end, and reports degraded if it persists
        // (`AcceptErrorPolicy::UnclassifiedBeforeDegraded`).
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case WSAEHOSTDOWN: return NetErrorCode::HostUnreach;
        case WSAEADDRINUSE: return NetErrorCode::AddressInUse;
        case WSAEADDRNOTAVAIL: return NetErrorCode::AddressNotAvail;
        case WSAEACCES: return NetErrorCode::PermissionDenied;
        case WSAEOPNOTSUPP: return NetErrorCode::Unsupported;
        case WSAEBADF:
        case WSAENOTSOCK: return NetErrorCode::BadHandle;
        case WSAEINTR: return NetErrorCode::Cancelled;
        // A receive deadline (`SO_RCVTIMEO`) expiring is WSAETIMEDOUT on Winsock: `isDeadlineExpiry`
        // is what a caller asks, so the two platforms need not agree on the code.
        case WSAETIMEDOUT: return NetErrorCode::Timeout;
        case WSAEMSGSIZE: return NetErrorCode::MessageTooLarge;
        case WSAEWOULDBLOCK: return NetErrorCode::WouldBlock;
        // Out of sockets, and out of buffer space: what a `WSASocketW` for the next `AcceptEx`
        // answers under load. Transient where `SystemError` may be permanent, which is what lets an
        // accept loop back off on these and give up on that (`AcceptPolicy.hpp`).
        case WSAEMFILE:
        case WSAENOBUFS:
        // `WSA_NOT_ENOUGH_MEMORY`, which is Win32's `ERROR_NOT_ENOUGH_MEMORY` by value, from a
        // completion or a `WSA*` call that could not allocate.
        case WSA_NOT_ENOUGH_MEMORY: return NetErrorCode::ResourceExhausted;
        default: return NetErrorCode::SystemError;
    }
}

int lastSocketError() noexcept
{
    return ::WSAGetLastError();
}

} // namespace core::net::detail
