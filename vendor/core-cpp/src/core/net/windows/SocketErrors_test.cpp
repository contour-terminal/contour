// SPDX-License-Identifier: Apache-2.0
//
// The Windows half of "a platform socket error is classified in ONE table": every row of
// `detail::classifySocketError`, and the two things `detail::fromWinsockError` adds on top of it for
// a completion or a dial. Every Windows transport and the dial read their errors through these, so a
// row that goes missing turns a code a caller branches on into `SystemError` for all of them at once.

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/net/NetError.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/windows/WinsockError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using core::net::NetErrorCode;
using core::net::detail::classifySocketError;
using core::net::detail::fromWinsockError;

namespace
{

/// One Winsock or Win32 error and the category it must land in.
struct Row
{
    int systemCode;    ///< The error value.
    NetErrorCode code; ///< Its category.
};

constexpr auto Rows = std::array {
    Row { .systemCode = WSAECONNRESET, .code = NetErrorCode::ConnReset },
    Row { .systemCode = WSAECONNREFUSED, .code = NetErrorCode::ConnRefused },
    Row { .systemCode = WSAEHOSTUNREACH, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = WSAENETUNREACH, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = WSAEADDRINUSE, .code = NetErrorCode::AddressInUse },
    Row { .systemCode = WSAEADDRNOTAVAIL, .code = NetErrorCode::AddressNotAvail },
    Row { .systemCode = WSAEACCES, .code = NetErrorCode::PermissionDenied },
    Row { .systemCode = WSAEBADF, .code = NetErrorCode::BadHandle },
    Row { .systemCode = WSAENOTSOCK, .code = NetErrorCode::BadHandle },
    Row { .systemCode = WSAEINTR, .code = NetErrorCode::Cancelled },
    Row { .systemCode = WSAETIMEDOUT, .code = NetErrorCode::Timeout },
    Row { .systemCode = WSAEMSGSIZE, .code = NetErrorCode::MessageTooLarge },
    Row { .systemCode = WSAEWOULDBLOCK, .code = NetErrorCode::WouldBlock },
    // Not resets, on purpose: an abort this end's stack made and a keepalive-detected loss are
    // `SystemError`, as `EPIPE` is on POSIX.
    Row { .systemCode = WSAECONNABORTED, .code = NetErrorCode::SystemError },
    Row { .systemCode = WSAENETRESET, .code = NetErrorCode::SystemError },
};

} // namespace

TEST_CASE("Every Winsock error lands in the category the one table gives it", "[net][errors]")
{
    for (auto const& row: Rows)
    {
        INFO("error " << row.systemCode);
        CHECK(classifySocketError(row.systemCode) == row.code);
        // `fromWinsockError` answers every WSAE* code through the table, not beside it.
        CHECK(fromWinsockError(row.systemCode, "test").code == row.code);
    }
}

TEST_CASE("fromWinsockError adds what a completion or a dial meets", "[net][errors]")
{
    CHECK(fromWinsockError(ERROR_OPERATION_ABORTED, "test").code == NetErrorCode::Cancelled);
    CHECK(fromWinsockError(WSAEAFNOSUPPORT, "test").code == NetErrorCode::Unsupported);
    CHECK(fromWinsockError(WSAEPROTONOSUPPORT, "test").code == NetErrorCode::Unsupported);
    CHECK(fromWinsockError(WSAECONNREFUSED, "test").systemCode == WSAECONNREFUSED);
}
