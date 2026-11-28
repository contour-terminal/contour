// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of "a resolver failure is classified in ONE table": every `getaddrinfo` code this
// platform can answer has a row of `resolverErrorTable()`, and the rows the WASI `ip-name-lookup`
// mapping depends on land where it says. The Windows half is `windows/ResolverErrors_test.cpp`.
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <map>
#include <ranges>
#include <string>
#include <string_view>

#include <netdb.h>

using core::net::NetErrorCode;
using core::net::resolverErrorTable;
using core::net::detail::classifyResolverError;
using core::net::detail::resolverSystemCode;

namespace
{

/// One `getaddrinfo` code and the category it must land in.
struct Row
{
    int gaiCode;       ///< The `EAI_*` value.
    NetErrorCode code; ///< Its category.
};

// Every row of the table, restated: a row whose category changes fails here, not only a row that
// goes missing. The codes only some libcs define are guarded by their own macro, as in the table;
// FreeBSD keeps the values of the two it retired, which its `gai_strerror` still describes.
constexpr auto Rows = std::array {
    Row { .gaiCode = EAI_NONAME, .code = NetErrorCode::NameNotFound },
#ifdef EAI_NODATA
    Row { .gaiCode = EAI_NODATA, .code = NetErrorCode::NameNotFound },
#elifdef __FreeBSD__
    Row { .gaiCode = 7, .code = NetErrorCode::NameNotFound },
#endif
#ifdef EAI_ADDRFAMILY
    Row { .gaiCode = EAI_ADDRFAMILY, .code = NetErrorCode::NameNotFound },
#elifdef __FreeBSD__
    Row { .gaiCode = 1, .code = NetErrorCode::NameNotFound },
#endif
    Row { .gaiCode = EAI_AGAIN, .code = NetErrorCode::ResolverTemporaryFailure },
    Row { .gaiCode = EAI_FAIL, .code = NetErrorCode::AddressError },
    Row { .gaiCode = EAI_MEMORY, .code = NetErrorCode::ResourceExhausted },
    Row { .gaiCode = EAI_SYSTEM, .code = NetErrorCode::SystemError },
    Row { .gaiCode = EAI_FAMILY, .code = NetErrorCode::AddressError },
    Row { .gaiCode = EAI_SOCKTYPE, .code = NetErrorCode::AddressError },
    Row { .gaiCode = EAI_BADFLAGS, .code = NetErrorCode::AddressError },
    Row { .gaiCode = EAI_SERVICE, .code = NetErrorCode::AddressError },
    Row { .gaiCode = EAI_OVERFLOW, .code = NetErrorCode::AddressError },
#ifdef EAI_BADHINTS
    Row { .gaiCode = EAI_BADHINTS, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_PROTOCOL
    Row { .gaiCode = EAI_PROTOCOL, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_INPROGRESS
    Row { .gaiCode = EAI_INPROGRESS, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_CANCELED
    Row { .gaiCode = EAI_CANCELED, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_NOTCANCELED
    Row { .gaiCode = EAI_NOTCANCELED, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_ALLDONE
    Row { .gaiCode = EAI_ALLDONE, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_INTR
    Row { .gaiCode = EAI_INTR, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_IDN_ENCODE
    Row { .gaiCode = EAI_IDN_ENCODE, .code = NetErrorCode::AddressError },
#endif
};

} // namespace

TEST_CASE("Every POSIX resolver code lands in its category, and the table holds nothing else",
          "[net][errors]")
{
    for (auto const& row: Rows)
    {
        INFO("EAI code " << row.gaiCode << ": " << ::gai_strerror(row.gaiCode));
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

TEST_CASE("Every code this platform's getaddrinfo can answer has a row", "[net][errors]")
{
    // Asked of the platform rather than of a list written here: `gai_strerror` describes every code
    // its libc defines, and the same text for every value it does not. A libc that grows a code fails
    // this case until the code's row is a decision, instead of reporting it as the catch-all.
    //
    // glibc numbers its codes from -1 down, with `getaddrinfo_a`'s around -100; macOS and the BSDs
    // number theirs from 1 up. The window covers both with room to spare.
    auto const unknown = std::string { ::gai_strerror(-424242) };
    auto missing = std::string {};
    for (auto const gaiCode: std::views::iota(-512, 513))
    {
        if (gaiCode == 0 || std::string_view { ::gai_strerror(gaiCode) } == unknown)
            continue;
        auto const listed = std::ranges::any_of(
            resolverErrorTable(), [gaiCode](auto const& row) { return row.gaiCode == gaiCode; });
        if (!listed)
            missing += std::to_string(gaiCode) + " (" + ::gai_strerror(gaiCode) + "); ";
    }
    INFO("getaddrinfo codes without a row: " << missing);
    CHECK(missing.empty());
}

TEST_CASE("Only EAI_SYSTEM carries an errno", "[net][errors]")
{
    // `EAI_SYSTEM` says "look in errno", and is the one code whose cause is not the code itself. Every
    // other code is no errno, and reporting the errno left over from some earlier call would name a
    // cause that has nothing to do with the lookup.
    errno = ECONNREFUSED;
    CHECK(resolverSystemCode(EAI_SYSTEM) == ECONNREFUSED);
    CHECK(resolverSystemCode(EAI_NONAME) == 0);
    CHECK(resolverSystemCode(EAI_AGAIN) == 0);
}
