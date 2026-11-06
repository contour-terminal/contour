// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CrossingHandle.hpp>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <Windows.h>
#endif

using vtpty::CrossingHandle;

TEST_CASE("a crossing handle round-trips a native handle", "[vtpty][handover]")
{
#ifdef _WIN32
    auto* const event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    auto const crossing = CrossingHandle::of(event);
    CHECK_FALSE(crossing.isNone());
    CHECK(crossing.native() == event);
    CloseHandle(event);
#else
    CHECK(CrossingHandle::of(5).native() == 5);
    CHECK(CrossingHandle::of(5) == CrossingHandle { 5 });
#endif
}

TEST_CASE("no handle crosses as none", "[vtpty][handover]")
{
    CHECK(CrossingHandle::of(core::platform::InvalidHandle).isNone());
    CHECK(CrossingHandle {}.isNone());
    CHECK(CrossingHandle {}.native() == core::platform::InvalidHandle);
#ifdef _WIN32
    CHECK(CrossingHandle::of(nullptr).isNone());
    CHECK(CrossingHandle::of(INVALID_HANDLE_VALUE).isNone());
#endif
}
