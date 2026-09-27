// SPDX-License-Identifier: Apache-2.0
#include <core/net/IoResult.hpp>
#include <core/net/NetError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>

using core::net::IoResult;
using core::net::isDeadlineExpiry;
using core::net::makeNetError;
using core::net::NetError;
using core::net::NetErrorCode;

// The zero enumerator is the absent case, and a code is one byte: what cpp-guidelines.md asks of an
// enum class, checked where the vocabulary is defined.
static_assert(std::is_same_v<std::underlying_type_t<NetErrorCode>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(NetErrorCode::Ok) == 0);
static_assert(NetErrorCode {} == NetErrorCode::Ok);

// Last states the count, so it must come after every code it counts.
static_assert(NetErrorCode::SystemError < NetErrorCode::Last);

// toString is usable in a constant expression, so a table of descriptions can be built at compile time.
static_assert(core::net::toString(NetErrorCode::Eof) == "end of stream");

// isDeadlineExpiry too: a caller may branch on it in a constant expression.
static_assert(isDeadlineExpiry(NetErrorCode::WouldBlock));

// The result of a byte transfer is a count or an error, and nothing else.
static_assert(std::is_same_v<IoResult, std::expected<std::size_t, NetError>>);

namespace
{

/// How many codes the enumeration has, taken from the enumeration itself. Every case below walks
/// `[0, CodeCount)`, so a code added tomorrow is covered without this file being edited — which is
/// the point: a hand-written list of today's codes only ever tests today's codes.
constexpr auto CodeCount = static_cast<int>(NetErrorCode::Last);

/// @return Every code in the enumeration, in declaration order.
[[nodiscard]] constexpr auto allCodes() noexcept
{
    return std::views::iota(0, CodeCount)
           | std::views::transform([](int index) { return static_cast<NetErrorCode>(index); });
}

/// Whether @p text is a description in the house style: lower-case words separated by spaces, no
/// punctuation, no leading or trailing space. A new code whose description shouts or ends in a full
/// stop changes the shape of every log line that carries it.
/// @param text The description to inspect.
/// @return Whether it is in that style.
[[nodiscard]] bool isHumanDescription(std::string_view text) noexcept
{
    if (text.empty() || text.front() == ' ' || text.back() == ' ')
        return false;
    return std::ranges::all_of(text, [](char ch) { return (ch >= 'a' && ch <= 'z') || ch == ' '; });
}

/// The vocabulary contour's `net::NetErrorCode` carried before the merge, written in the merged
/// spelling: contour's `Other` is `SystemError`.
constexpr auto ContourLineage = std::array {
    NetErrorCode::Ok,           NetErrorCode::Eof,         NetErrorCode::Cancelled,
    NetErrorCode::Timeout,      NetErrorCode::WouldBlock,  NetErrorCode::BadHandle,
    NetErrorCode::ConnReset,    NetErrorCode::ConnRefused, NetErrorCode::AddressInUse,
    NetErrorCode::AddressError, NetErrorCode::Unsupported, NetErrorCode::MessageTooLarge,
    NetErrorCode::SystemError,
};

/// The vocabulary fastcached's `FastCache::NetErrorCode` carried at `0708dd54`, written in the
/// merged spelling: fastcached's `BadFileHandle` is `BadHandle`.
constexpr auto FastcachedLineage = std::array {
    NetErrorCode::Ok,
    NetErrorCode::Eof,
    NetErrorCode::Cancelled,
    NetErrorCode::Timeout,
    NetErrorCode::WouldBlock,
    NetErrorCode::BadHandle,
    NetErrorCode::AddressInUse,
    NetErrorCode::ConnRefused,
    NetErrorCode::ConnReset,
    NetErrorCode::HostUnreach,
    NetErrorCode::AddressNotAvail,
    NetErrorCode::PermissionDenied,
    NetErrorCode::SystemError,
};

} // namespace

TEST_CASE("Every NetErrorCode has a description of its own in the house style", "[net][types]")
{
    auto seen = std::set<std::string_view> {};
    for (auto const code: allCodes())
    {
        auto const text = core::net::toString(code);
        CAPTURE(static_cast<int>(code), text);
        CHECK(text != "unknown error");
        CHECK(isHumanDescription(text));
        CHECK(seen.insert(text).second);
    }
    CHECK(seen.size() == static_cast<std::size_t>(CodeCount));
}

TEST_CASE("Last counts the codes; it is not one of them", "[net][types]")
{
    CHECK(core::net::toString(NetErrorCode::Last) == "unknown error");
    CHECK_FALSE(isDeadlineExpiry(NetErrorCode::Last));
}

TEST_CASE("No code hides above Last", "[net][types]")
{
    // Every case here walks `[0, Last)`, so a code appended AFTER `Last` is invisible to all of
    // them -- and it breaks nothing that would otherwise catch it: it has a `toString` case, so
    // -Wswitch is satisfied, and `static_assert(SystemError < Last)` still holds because
    // `SystemError` did not move. Measured: with such a code in the enumeration the other twelve
    // cases pass green.
    //
    // The enumeration is dense from zero, so "nothing above the count is described" says the same
    // thing as "`Last` is the count" -- and unlike the count, it is still true to check once a code
    // has been appended past it. `.agent/rules/design-principles.md` names the general version: a
    // check anchored on an enumerator by name fires only when nothing is wrong.
    auto hiddenAt = -1;
    auto hidden = std::string_view {};
    for (auto const value: std::views::iota(CodeCount, 256))
    {
        auto const text = core::net::toString(static_cast<NetErrorCode>(value));
        if (text != "unknown error")
        {
            hiddenAt = value;
            hidden = text;
            break;
        }
    }
    CAPTURE(hiddenAt, hidden);
    CHECK(hiddenAt == -1);
}

TEST_CASE("A value outside the enumeration is described as unknown", "[net][types]")
{
    CHECK(core::net::toString(static_cast<NetErrorCode>(0xFF)) == "unknown error");
}

TEST_CASE("Both spellings of an expired deadline answer yes and no other code does", "[net][types]")
{
    CHECK(isDeadlineExpiry(NetErrorCode::Timeout));
    CHECK(isDeadlineExpiry(NetErrorCode::WouldBlock));

    for (auto const code: allCodes())
    {
        if (code == NetErrorCode::Timeout || code == NetErrorCode::WouldBlock)
            continue;
        CAPTURE(core::net::toString(code));
        CHECK_FALSE(isDeadlineExpiry(code));
    }
}

TEST_CASE("Dropping WouldBlock stops an accept loop a quarter-second in (fastcached#824)", "[net][types]")
{
    // A listener whose poll carries a deadline reports that deadline expiring as WouldBlock on
    // POSIX and as Timeout on Winsock. An accept loop asks "did my poll tick" and goes round again;
    // narrow this predicate to Timeout and every POSIX tick reads as a fatal accept error instead,
    // so the loop logs once and returns. Both operands are load-bearing, and neither may be
    // dropped: https://github.com/LASTRADA-Software/fastcached/issues/824
    CHECK(isDeadlineExpiry(NetErrorCode::WouldBlock)); // the POSIX spelling
    CHECK(isDeadlineExpiry(NetErrorCode::Timeout));    // the Winsock spelling
}

TEST_CASE("makeNetError carries every argument and defaults the two it may omit", "[net][types]")
{
    auto const full = makeNetError(NetErrorCode::AddressInUse, 98, "bind");
    CHECK(full.code == NetErrorCode::AddressInUse);
    CHECK(full.systemCode == 98);
    CHECK(full.context == "bind");

    auto const codeOnly = makeNetError(NetErrorCode::HostUnreach);
    CHECK(codeOnly.code == NetErrorCode::HostUnreach);
    CHECK(codeOnly.systemCode == 0);
    CHECK(codeOnly.context.empty());

    auto const noContext = makeNetError(NetErrorCode::PermissionDenied, 13);
    CHECK(noContext.code == NetErrorCode::PermissionDenied);
    CHECK(noContext.systemCode == 13);
    CHECK(noContext.context.empty());
}

TEST_CASE("A default NetError is an unclassified OS error and never a success", "[net][types]")
{
    auto const unclassified = NetError {};
    CHECK(unclassified.code == NetErrorCode::SystemError);
    CHECK(unclassified.code != NetErrorCode::Ok);
    CHECK(unclassified.systemCode == 0);
    CHECK(unclassified.context.empty());
}

TEST_CASE("NetError describes its category then its context then the OS code", "[net][types]")
{
    CHECK(makeNetError(NetErrorCode::ConnReset, 104, "recv").toString()
          == "connection reset (recv) [errno 104]");
    CHECK(makeNetError(NetErrorCode::Eof).toString() == "end of stream");
    CHECK(makeNetError(NetErrorCode::Timeout, 0, "connect").toString() == "timed out (connect)");
    CHECK(makeNetError(NetErrorCode::SystemError, 13).toString() == "system error [errno 13]");
    CHECK(makeNetError(NetErrorCode::AddressNotAvail, 99, "bind").toString()
          == "address not available (bind) [errno 99]");
}

TEST_CASE("NetError renders words rather than an enumerator's position", "[net][types]")
{
    // The merged rendering is contour's. fastcached's was `NetError(code=9 system=104 context=recv)`,
    // whose `9` is a position in an enumeration this very task renumbered, so an old log line and a
    // new one that read alike mean different codes.
    auto const text = makeNetError(NetErrorCode::ConnReset, 104, "recv").toString();
    CHECK(text.starts_with("connection reset"));
    CHECK_FALSE(text.contains("code="));
    CHECK_FALSE(text.contains("NetError("));
}

TEST_CASE("The merge gave each lineage exactly what the other one had", "[net][types]")
{
    // That every code either lineage had survives is settled by the compiler, not here: the two
    // arrays above are written in the merged spelling, so they name codes that must exist for this
    // file to build, and the first case already proves each has a description of its own. Asserting
    // it again at run time would be a case that cannot fail.
    //
    // What is not settled anywhere else is the merge's actual content: which codes each lineage did
    // not have before and does now. Get that wrong -- drop one, or add a code to only one side --
    // and a caller of that lineage has nowhere to put a failure it used to distinguish, which is
    // the thing this whole task exists to prevent.
    auto const contour = std::set<NetErrorCode>(ContourLineage.begin(), ContourLineage.end());
    auto const fastcached = std::set<NetErrorCode>(FastcachedLineage.begin(), FastcachedLineage.end());

    // Described rather than compared as codes, so a failure reads as words rather than as numbers.
    auto const gainedBy = [](std::set<NetErrorCode> const& lineage, std::set<NetErrorCode> const& other) {
        auto descriptions = std::set<std::string_view> {};
        for (auto const code: other)
            if (!lineage.contains(code))
                descriptions.insert(core::net::toString(code));
        return descriptions;
    };

    CHECK(gainedBy(contour, fastcached)
          == std::set<std::string_view> { "address not available", "host unreachable", "permission denied" });
    CHECK(gainedBy(fastcached, contour)
          == std::set<std::string_view> { "address error", "message too large", "unsupported" });
}

TEST_CASE("The two renamed codes keep the meaning their call sites relied on", "[net][types]")
{
    // contour's `Other`: the OS error nothing classified further, and what a NetError defaults to,
    // which is what every `makeNetError(Other, errno, ...)` call site in src/core/net/ meant.
    CHECK(NetError {}.code == NetErrorCode::SystemError);
    CHECK(makeNetError(NetErrorCode::SystemError, 13, "bind").toString() == "system error (bind) [errno 13]");

    // fastcached's `BadFileHandle`: a closed or invalid descriptor, which is a different answer
    // from "some OS error" and must stay one.
    CHECK(core::net::toString(NetErrorCode::BadHandle) == "bad handle");
    CHECK(NetErrorCode::BadHandle != NetErrorCode::SystemError);

    auto const closed = IoResult { std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "read")) };
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().code == NetErrorCode::BadHandle);
    CHECK(closed.error().context == "read");
}

TEST_CASE("IoResult carries the transferred count or the error", "[net][types]")
{
    auto const transferred = IoResult { std::size_t { 42 } };
    REQUIRE(transferred.has_value());
    CHECK(*transferred == 42);

    auto const failed = IoResult { std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "read")) };
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == NetErrorCode::BadHandle);
    CHECK(failed.error().context == "read");
}
