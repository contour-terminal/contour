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
        // One category for both, for the reason the POSIX table gives.
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH: return NetErrorCode::HostUnreach;
        case WSAEADDRINUSE: return NetErrorCode::AddressInUse;
        case WSAEADDRNOTAVAIL: return NetErrorCode::AddressNotAvail;
        case WSAEACCES: return NetErrorCode::PermissionDenied;
        case WSAEBADF:
        case WSAENOTSOCK: return NetErrorCode::BadHandle;
        case WSAEINTR: return NetErrorCode::Cancelled;
        // A receive deadline (`SO_RCVTIMEO`) expiring is WSAETIMEDOUT on Winsock: `isDeadlineExpiry`
        // is what a caller asks, so the two platforms need not agree on the code.
        case WSAETIMEDOUT: return NetErrorCode::Timeout;
        case WSAEMSGSIZE: return NetErrorCode::MessageTooLarge;
        case WSAEWOULDBLOCK: return NetErrorCode::WouldBlock;
        default: return NetErrorCode::SystemError;
    }
}

int lastSocketError() noexcept
{
    return ::WSAGetLastError();
}

} // namespace core::net::detail
