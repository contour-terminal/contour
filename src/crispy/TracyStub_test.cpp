// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <mutex>

#include <tracy/Tracy.hpp>

namespace
{

/// Counts its own invocations, so a test can prove a disabled macro never evaluates its arguments.
int sideEffectCount = 0;

/// Records that it was called, and yields a value usable as a Tracy plot sample.
/// @return Always 1.
int64_t sideEffect() noexcept
{
    ++sideEffectCount;
    return 1;
}

} // namespace

TEST_CASE("tracy_stub.macros_compile", "[tracy]")
{
    ZoneScoped;
    ZoneText("text", 4);
    ZoneValue(42);

    // A nested scope, because ZoneScoped and ZoneScopedN both declare the same zone name -- one per
    // scope, in the real header and in the stub alike.
    {
        ZoneScopedN("named zone");
        ZoneValue(7);
    }

    TracyMessageL("message");
    FrameMarkNamed("test frame");
    FrameMark;
    REQUIRE(true);
}

TEST_CASE("tracy_stub.arguments_are_not_evaluated", "[tracy]")
{
    ZoneScoped;
    sideEffectCount = 0;

    // One evaluated call, which does two jobs: it shows the counter works, and it keeps
    // sideEffect() emitted at all. Without it the compiler rejects the file under -Werror with
    // "function 'sideEffect' is not needed and will not be emitted" -- which is precisely the
    // property under test, since every other call below sits in an unevaluated context.
    REQUIRE(sideEffect() == 1);

    TracyPlot("plot", sideEffect());
    ZoneValue(sideEffect());

    // The stub keeps its arguments in an unevaluated sizeof, so neither of those two ran. Under
    // CONTOUR_TRACY=ON the real macros DO evaluate their arguments, hence the two expectations.
#ifndef TRACY_ENABLE
    REQUIRE(sideEffectCount == 1);
#else
    REQUIRE(sideEffectCount == 3);
#endif
}

TEST_CASE("tracy_stub.lockable_is_a_mutex", "[tracy]")
{
    TracyLockable(std::mutex, mutex);
    {
        auto const guard = std::lock_guard { mutex };
        REQUIRE(true);
    }
}
