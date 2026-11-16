// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using core::ManualClock;
using core::SteadyClock;
using core::SteadyTimePoint;

TEST_CASE("SteadyClock::now is monotonic", "[clock]")
{
    auto const clock = SteadyClock {};
    auto const first = clock.now();
    auto const second = clock.now();
    REQUIRE(second >= first);
}

TEST_CASE("ManualClock starts at the constructed value and does not advance on its own", "[clock]")
{
    auto const start = SteadyTimePoint {} + 1s;
    auto const clock = ManualClock { start };
    REQUIRE(clock.now() == start);
    // Reading twice yields the same value: time is frozen until advance/setNow.
    REQUIRE(clock.now() == start);
}

TEST_CASE("ManualClock::advance moves time forward by the delta, and advances accumulate", "[clock]")
{
    auto clock = ManualClock {};
    auto const origin = clock.now();
    clock.advance(100ms);
    REQUIRE(clock.now() == origin + 100ms);
    clock.advance(200ms);
    clock.advance(50ms);
    REQUIRE(clock.now() == origin + 350ms);
}

TEST_CASE("ManualClock::setNow hard-sets the value", "[clock]")
{
    auto clock = ManualClock { SteadyTimePoint {} + 5s };
    auto const target = SteadyTimePoint {} + 2s;
    clock.setNow(target);
    REQUIRE(clock.now() == target);
}

TEST_CASE("defaultSteadyClock returns a usable singleton", "[clock]")
{
    auto& a = core::defaultSteadyClock();
    auto& b = core::defaultSteadyClock();
    REQUIRE(&a == &b);
    // Sample in a defined order (the macro's argument evaluation order is
    // unspecified, so two inline now() calls could otherwise read out of order).
    auto const first = a.now();
    auto const second = b.now();
    REQUIRE(second >= first);
}

TEST_CASE("refresh is a no-op for clocks that read their source directly", "[clock]")
{
    // SteadyClock and ManualClock inherit the default. An event loop calls refresh()
    // unconditionally on whatever clock it was given, so this must be safe and must not disturb
    // a test's manual timeline. Sampled into locals in a defined order: the two sides of a
    // REQUIRE are not sequenced.
    auto steady = SteadyClock {};
    auto const before = steady.now();
    steady.refresh();
    auto const after = steady.now();
    REQUIRE(after >= before);

    auto manual = ManualClock { SteadyTimePoint { 7s } };
    manual.refresh();
    REQUIRE(manual.now() == SteadyTimePoint { 7s });
}
