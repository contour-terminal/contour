// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of "a platform socket error is classified in ONE table": every row of
// `detail::classifySocketError`, asserted, because every transport and the dial read their errors
// through it -- a row that goes missing turns a code a caller branches on into `SystemError` for all
// of them at once, and nothing else in the suite provokes most of these errors.
#include <core/net/NetError.hpp>
#include <core/net/detail/SocketErrors.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>

using core::net::NetErrorCode;
using core::net::detail::classifySocketError;

namespace
{

/// One `errno` and the category it must land in.
struct Row
{
    int systemCode;    ///< The `errno` value.
    NetErrorCode code; ///< Its category.
};

// `EPIPE` is `SystemError` ON PURPOSE: it is a write after this end's own half-close, not a reset,
// and `SocketClosedStates_test.cpp` pins the in-memory fake to that answer against a real pair.
constexpr auto Rows = std::array {
    Row { .systemCode = ECONNRESET, .code = NetErrorCode::ConnReset },
    Row { .systemCode = ECONNREFUSED, .code = NetErrorCode::ConnRefused },
    Row { .systemCode = EHOSTUNREACH, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = ENETUNREACH, .code = NetErrorCode::HostUnreach },
    Row { .systemCode = EADDRINUSE, .code = NetErrorCode::AddressInUse },
    Row { .systemCode = EADDRNOTAVAIL, .code = NetErrorCode::AddressNotAvail },
    Row { .systemCode = EACCES, .code = NetErrorCode::PermissionDenied },
    Row { .systemCode = EPERM, .code = NetErrorCode::PermissionDenied },
    Row { .systemCode = EAFNOSUPPORT, .code = NetErrorCode::Unsupported },
    Row { .systemCode = EPROTONOSUPPORT, .code = NetErrorCode::Unsupported },
    Row { .systemCode = EBADF, .code = NetErrorCode::BadHandle },
    Row { .systemCode = ENOTSOCK, .code = NetErrorCode::BadHandle },
    Row { .systemCode = EINTR, .code = NetErrorCode::Cancelled },
    Row { .systemCode = ETIMEDOUT, .code = NetErrorCode::Timeout },
    Row { .systemCode = EMSGSIZE, .code = NetErrorCode::MessageTooLarge },
    Row { .systemCode = EWOULDBLOCK, .code = NetErrorCode::WouldBlock },
    Row { .systemCode = EAGAIN, .code = NetErrorCode::WouldBlock },
    Row { .systemCode = EPIPE, .code = NetErrorCode::SystemError },
    Row { .systemCode = ENOMEM, .code = NetErrorCode::SystemError },
};

} // namespace

TEST_CASE("Every POSIX socket error lands in the category the one table gives it", "[net][errors]")
{
    for (auto const& row: Rows)
    {
        INFO("errno " << row.systemCode);
        CHECK(classifySocketError(row.systemCode) == row.code);
    }
}
