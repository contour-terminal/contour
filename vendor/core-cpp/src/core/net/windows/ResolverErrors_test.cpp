// SPDX-License-Identifier: Apache-2.0
//
// The Windows half of "a resolver failure is classified in ONE table". Winsock's `getaddrinfo`
// answers `WSA*` codes (its `EAI_*` macros are spellings of them), and Windows has no `gai_strerror`
// that tells a known code from an unknown one -- it formats any system message -- so completeness is
// asserted against the documented list here, the whole table and nothing beside it.

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>

using core::net::NetErrorCode;
using core::net::resolverErrorTable;
using core::net::detail::classifyResolverError;
using core::net::detail::resolverSystemCode;

namespace
{

/// One Winsock resolver code and the category it must land in.
struct Row
{
    int gaiCode;       ///< The `WSA*` value `getaddrinfo` returned.
    NetErrorCode code; ///< Its category.
};

// `getaddrinfo`'s documented return values, plus `WSANO_DATA` (a name with no address of the asked
// family, which it answers in practice) and `WSANOTINITIALISED`.
constexpr auto Rows = std::array {
    Row { .gaiCode = WSAHOST_NOT_FOUND, .code = NetErrorCode::NameNotFound },
    Row { .gaiCode = WSANO_DATA, .code = NetErrorCode::NameNotFound },
    Row { .gaiCode = WSATRY_AGAIN, .code = NetErrorCode::ResolverTemporaryFailure },
    Row { .gaiCode = WSANO_RECOVERY, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSAEINVAL, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSATYPE_NOT_FOUND, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSA_SECURE_HOST_NOT_FOUND, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSA_IPSEC_NAME_POLICY_ERROR, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSAEAFNOSUPPORT, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSAESOCKTNOSUPPORT, .code = NetErrorCode::AddressError },
    Row { .gaiCode = WSA_NOT_ENOUGH_MEMORY, .code = NetErrorCode::ResourceExhausted },
    Row { .gaiCode = WSANOTINITIALISED, .code = NetErrorCode::SystemError },
};

} // namespace

TEST_CASE("Every Winsock resolver code lands in its category, and the table holds nothing else",
          "[net][errors]")
{
    for (auto const& row: Rows)
    {
        INFO("WSA code " << row.gaiCode);
        CHECK(classifyResolverError(row.gaiCode) == row.code);
    }

    auto expected = std::map<int, NetErrorCode> {};
    for (auto const& row: Rows)
        expected.emplace(row.gaiCode, row.code);
    auto actual = std::map<int, NetErrorCode> {};
    for (auto const& row: resolverErrorTable())
        actual.emplace(row.gaiCode, row.code);
    CHECK(actual == expected);
}

TEST_CASE("A Winsock resolver code is its own system code", "[net][errors]")
{
    // On Windows the code `getaddrinfo` returns IS the `WSAGetLastError` value, so it is reported as
    // the error's system code; POSIX has nothing like it outside `EAI_SYSTEM`.
    CHECK(resolverSystemCode(WSAHOST_NOT_FOUND) == WSAHOST_NOT_FOUND);
    CHECK(resolverSystemCode(WSATRY_AGAIN) == WSATRY_AGAIN);
}
