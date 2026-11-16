// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Spinner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <span>
#include <string_view>
#include <type_traits>

using core::ManualClock;
using core::tui::Spinner;
using core::tui::SpinnerType;

using namespace std::chrono_literals;

TEST_CASE("Spinner: a frame lasts its interval on the injected clock", "[spinner]")
{
    auto clock = ManualClock {};
    static constexpr auto Frames = std::array<std::string_view, 3> { "a", "b", "c" };
    auto spinner = Spinner { Frames, 100ms, clock };
    REQUIRE(spinner.currentFrame() == "a");

    clock.advance(99ms);
    CHECK_FALSE(spinner.tick());
    CHECK(spinner.currentFrame() == "a");

    clock.advance(1ms);
    CHECK(spinner.tick());
    CHECK(spinner.currentFrame() == "b");

    // The interval restarts at the tick that advanced, not at the one before it.
    clock.advance(99ms);
    CHECK_FALSE(spinner.tick());
    clock.advance(1ms);
    CHECK(spinner.tick());
    CHECK(spinner.currentFrame() == "c");

    clock.advance(100ms);
    CHECK(spinner.tick());
    CHECK(spinner.currentFrame() == "a"); // Wraps around.
}

TEST_CASE("Spinner: a long pause advances one frame, not one per interval missed", "[spinner]")
{
    auto clock = ManualClock {};
    static constexpr auto Frames = std::array<std::string_view, 3> { "a", "b", "c" };
    auto spinner = Spinner { Frames, 100ms, clock };

    clock.advance(1000ms);
    CHECK(spinner.tick());
    CHECK(spinner.frameIndex() == 1);
    CHECK_FALSE(spinner.tick()); // No time has passed since.
}

TEST_CASE("Spinner: reset returns to the first frame and restarts the interval", "[spinner]")
{
    auto clock = ManualClock {};
    auto spinner = Spinner { SpinnerType::Line, clock };
    auto const interval = spinner.interval();

    clock.advance(interval);
    REQUIRE(spinner.tick());
    REQUIRE(spinner.frameIndex() == 1);

    clock.advance(interval / 2);
    spinner.reset();
    CHECK(spinner.frameIndex() == 0);
    clock.advance(interval - 1ms);
    CHECK_FALSE(spinner.tick()); // Measured from the reset, not from the last frame.
    clock.advance(1ms);
    CHECK(spinner.tick());
}

TEST_CASE("Spinner: a temporary clock is refused", "[spinner]")
{
    // The spinner keeps the clock it is given, so binding a temporary would dangle.
    STATIC_CHECK(std::is_constructible_v<Spinner, SpinnerType, ManualClock&>);
    STATIC_CHECK_FALSE(std::is_constructible_v<Spinner, SpinnerType, ManualClock>);
    STATIC_CHECK_FALSE(std::is_constructible_v<Spinner,
                                               std::span<std::string_view const>,
                                               std::chrono::milliseconds,
                                               ManualClock>);
}
