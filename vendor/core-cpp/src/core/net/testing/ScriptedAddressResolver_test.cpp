// SPDX-License-Identifier: Apache-2.0
//
// `testing::ScriptedAddressResolver`, and the default `IAddressResolver::resolveClassified` it is the
// first resolver to exercise: a resolver that only says WHY in words is a permanent failure, with
// those words kept.
#include <core/async/Task.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/testing/ScriptedAddressResolver.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::makeNetError;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::ResolveResult;
using core::net::testing::ScriptedAddressResolver;

namespace
{

/// An endpoint no literal would produce, so a case can tell a scripted answer from a parsed one.
[[nodiscard]] ResolvedEndpoint scriptedEndpoint()
{
    return ResolvedEndpoint { .length = 16, .family = 2, .protocol = 6 };
}

/// Resolves @p host through @p resolver and stores what came back.
Task<void> resolveInto(core::net::IAsyncAddressResolver* resolver, std::string host, ResolveResult* out)
{
    *out = co_await resolver->resolve(std::move(host), 80, nullptr);
}

} // namespace

TEST_CASE("the default classification of a worded failure is a permanent one that keeps the words",
          "[net][resolver]")
{
    // A resolver written before the classified seam existed overrides only `resolve`. Its failures
    // must still reach a caller of `resolveClassified` -- as `AddressError`, the one code that claims
    // nothing about whether asking again could help, and with its message intact.
    auto script = ScriptedAddressResolver {};
    script.failWith("no such host");

    auto const resolved = script.resolveClassified("missing.test", 80);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == NetErrorCode::AddressError);
    CHECK(resolved.error().context == "no such host");
    CHECK(resolved.error().systemCode == 0);

    auto const worded = script.resolve("missing.test", 80);
    REQUIRE_FALSE(worded.has_value());
    CHECK(worded.error() == "no such host");
}

TEST_CASE("a scripted code reaches a classified caller as it was scripted", "[net][resolver]")
{
    auto script = ScriptedAddressResolver {};
    script.failWithCode(makeNetError(NetErrorCode::ResolverTemporaryFailure, 0, "server failure"));

    auto const resolved = script.resolveClassified("flaky.test", 80);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == NetErrorCode::ResolverTemporaryFailure);
    CHECK(resolved.error().context == "server failure");

    // The worded seam renders the same failure in words, so a caller of either sees one answer.
    auto const worded = script.resolve("flaky.test", 80);
    REQUIRE_FALSE(worded.has_value());
    CHECK(worded.error() == "resolver temporarily unavailable (server failure)");
}

TEST_CASE("a scripted name answers its endpoints and an unscripted one is not found", "[net][resolver]")
{
    auto script = ScriptedAddressResolver {};
    script.answer("db.test", { scriptedEndpoint(), scriptedEndpoint() });

    auto const answered = script.resolveClassified("db.test", 5432);
    REQUIRE(answered.has_value());
    CHECK(answered->size() == 2);
    CHECK(answered->front().length == 16);

    // Deterministic without a script for it: the answer a real resolver gives an unknown name.
    auto const unknown = script.resolveClassified("other.test", 5432);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == NetErrorCode::NameNotFound);
    CHECK(unknown.error().context.contains("other.test"));

    CHECK(script.calls() == 2);
}

TEST_CASE("a literal is parsed, never answered from the script", "[net][resolver]")
{
    // What a real resolver does with a literal, so a test of a dial to one exercises what production
    // would: the address and the port come from the text, whatever the script says.
    auto script = ScriptedAddressResolver {};
    script.answer("127.0.0.1", { scriptedEndpoint() });
    script.failWithCode(makeNetError(NetErrorCode::NameNotFound, 0, "scripted"));

    auto const v4 = script.resolveClassified("127.0.0.1", 9);
    REQUIRE(v4.has_value());
    REQUIRE(v4->size() == 1);
    CHECK(core::net::formatPeerAddress(v4->front()) == "127.0.0.1");
    CHECK(core::net::detail::portOfSockaddr(v4->front().storage.data(), v4->front().length) == 9);

    auto const v6 = script.resolveClassified("::1", 443);
    REQUIRE(v6.has_value());
    REQUIRE(v6->size() == 1);
    CHECK(core::net::formatPeerAddress(v6->front()) == "::1");
    CHECK(core::net::detail::portOfSockaddr(v6->front().storage.data(), v6->front().length) == 443);

    CHECK(script.calls() == 2); // asked, and answered without the script

    // The shorthand getaddrinfo expands is a name here, as `endpointOfLiteral` documents.
    auto const shorthand = script.resolveClassified("127.1", 9);
    REQUIRE_FALSE(shorthand.has_value());
    CHECK(shorthand.error().code == NetErrorCode::NameNotFound);
}

TEST_CASE("the inline resolver reports what the classified seam said", "[net][resolver]")
{
    auto script = ScriptedAddressResolver {};
    script.failWithCode(makeNetError(NetErrorCode::NameNotFound, 0, "NXDOMAIN"));
    auto resolver = core::net::InlineAddressResolver { script };

    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };

    auto answer = ResolveResult {};
    loop.blockOn(resolveInto(&resolver, "missing.test", &answer));
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code == NetErrorCode::NameNotFound);
    CHECK(answer.error().context == "cannot resolve missing.test:80: NXDOMAIN");
}

TEST_CASE("the inline resolver refuses a host no name can be before its inner resolver sees it",
          "[net][resolver]")
{
    // The same refusal as the threaded and the executor resolvers': an empty host (getaddrinfo's
    // wildcard), a NUL, or more than 253 bytes.
    struct Row
    {
        std::string_view what;
        std::string host;
    };
    auto const rows = std::vector<Row> {
        { .what = "empty", .host = {} },
        { .what = "an embedded NUL", .host = std::string { "example.test\0evil", 17 } },
        { .what = "254 bytes", .host = std::string(254, 'a') },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.what)
        {
            auto script = ScriptedAddressResolver {};
            auto resolver = core::net::InlineAddressResolver { script };
            auto clock = core::platform::ManualClock {};
            auto loop = core::net::testing::TestLoop { clock };

            auto answer = ResolveResult {};
            loop.blockOn(resolveInto(&resolver, row.host, &answer));
            REQUIRE_FALSE(answer.has_value());
            CHECK(answer.error().code == NetErrorCode::AddressError);
            CHECK(script.calls() == 0);
        }
    }
}

TEST_CASE("a held lookup waits for release, and the wait for it is bounded", "[net][resolver]")
{
    // Nothing is entered: the bounded wait says so instead of hanging.
    auto script = ScriptedAddressResolver {};
    CHECK(script.awaitEntered(1, std::chrono::milliseconds { 1 }) == 0);

    // Held, a lookup on the caller's thread would never return, so the hold is observed through the
    // count alone here; `ThreadedAddressResolver_test.cpp` drives a held lookup across a thread.
    script.hold();
    script.release();
    auto const resolved = script.resolveClassified("127.0.0.1", 1);
    CHECK(resolved.has_value());
    CHECK(script.awaitEntered(1, std::chrono::milliseconds { 1 }) == 1);
}
