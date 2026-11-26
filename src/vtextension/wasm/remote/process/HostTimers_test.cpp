// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <vector>

#include <vtextension/wasm/remote/process/HostTimers.hpp>

using vtextension::wasm::remote::process::HostTimers;
using vtextension::wasm::remote::process::TimerKey;
using namespace std::chrono_literals;

namespace
{

/// An instant every case counts from.
constexpr auto T0 = core::SteadyTimePoint {} + 1h;

constexpr auto Key = TimerKey { .helper = 7, .serial = 1 };

} // namespace

TEST_CASE("HostTimers.expiresAtTheBudget", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(Key, T0, 10ms);
    CHECK(timers.nextDeadline() == T0 + 10ms);
    CHECK(timers.expire(T0 + 9ms).empty());
    CHECK(timers.armed() == 1);
    CHECK(timers.expire(T0 + 10ms) == std::vector { Key });
    CHECK(timers.armed() == 0);
    CHECK(timers.nextDeadline() == std::nullopt);
}

TEST_CASE("HostTimers.pauseStopsTheClock", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(Key, T0, 50ms);
    timers.pause(Key, T0 + 10ms);
    CHECK(timers.nextDeadline() == std::nullopt);
    CHECK(timers.expire(T0 + 1000ms).empty());
    CHECK(timers.armed() == 1);

    // 40 ms were left when it paused: they run from the resume on.
    timers.resume(Key, T0 + 1010ms);
    CHECK(timers.nextDeadline() == T0 + 1050ms);
    CHECK(timers.expire(T0 + 1049ms).empty());
    CHECK(timers.expire(T0 + 1050ms) == std::vector { Key });
}

TEST_CASE("HostTimers.pauseAndResumeAreIdempotent", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(Key, T0, 50ms);
    timers.resume(Key, T0 + 5ms); // running already: ignored
    timers.pause(Key, T0 + 10ms);
    timers.pause(Key, T0 + 30ms); // paused already: ignored
    timers.resume(Key, T0 + 100ms);
    CHECK(timers.nextDeadline() == T0 + 140ms);

    // Unknown keys are ignored.
    timers.pause(TimerKey { .helper = 8, .serial = 1 }, T0);
    timers.resume(TimerKey { .helper = 8, .serial = 1 }, T0);
    CHECK(timers.armed() == 1);
}

TEST_CASE("HostTimers.disarmIsFinal", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(Key, T0, 10ms);
    timers.disarm(Key);
    CHECK(timers.armed() == 0);
    CHECK(timers.nextDeadline() == std::nullopt);
    CHECK(timers.expire(T0 + 1h).empty());
    // Resuming what is gone does not bring it back.
    timers.resume(Key, T0 + 1h);
    CHECK(timers.armed() == 0);
}

TEST_CASE("HostTimers.nextDeadlineIsTheEarliest", "[vtextension]")
{
    auto timers = HostTimers {};
    auto const later = TimerKey { .helper = 1, .serial = 1 };
    auto const earlier = TimerKey { .helper = 2, .serial = 1 };
    auto const paused = TimerKey { .helper = 3, .serial = 1 };
    timers.arm(later, T0, 30ms);
    timers.arm(earlier, T0 + 5ms, 10ms);
    timers.arm(paused, T0, 1ms);
    timers.pause(paused, T0);
    CHECK(timers.nextDeadline() == T0 + 15ms);
    CHECK(timers.expire(T0 + 30ms) == std::vector { later, earlier });
    CHECK(timers.nextDeadline() == std::nullopt);
    CHECK(timers.armed() == 1);
}

TEST_CASE("HostTimers.rearmingReplaces", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(Key, T0, 10ms);
    timers.arm(Key, T0 + 5ms, 10ms);
    CHECK(timers.armed() == 1);
    CHECK(timers.nextDeadline() == T0 + 15ms);
}

TEST_CASE("HostTimers.disarmHelperForgetsItsTimersOnly", "[vtextension]")
{
    auto timers = HostTimers {};
    timers.arm(TimerKey { .helper = 1, .serial = 1 }, T0, 10ms);
    timers.arm(TimerKey { .helper = 1, .serial = 2 }, T0, 10ms);
    timers.arm(TimerKey { .helper = 2, .serial = 1 }, T0, 20ms);
    timers.disarmHelper(1);
    CHECK(timers.armed() == 1);
    CHECK(timers.nextDeadline() == T0 + 20ms);
}
