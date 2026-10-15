// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/ClickCounter.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <type_traits>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{
constexpr auto Here = CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) };
constexpr auto There = CellLocation { .line = LineOffset(2), .column = ColumnOffset(4) };

// The counter borrows its clock for its whole life, so a temporary one -- which would dangle -- is refused at
// compile time, beside the named clock it must still accept.
static_assert(std::is_constructible_v<ClickCounter, core::platform::ManualClock&, std::chrono::milliseconds>);
static_assert(!std::is_constructible_v<ClickCounter, core::platform::ManualClock, std::chrono::milliseconds>);
} // namespace

TEST_CASE("ClickCounter.countsPressesOnOneCellUpToFourThenStartsOver", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 3);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 4);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}

TEST_CASE("ClickCounter.theIntervalIsInclusive", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.advance(1000ms);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    clock.advance(1001ms);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}

TEST_CASE("ClickCounter.anotherCellStartsANewSequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Left) == 2);
}

TEST_CASE("ClickCounter.eachButtonKeepsItsOwnSequence", "[semanticblocks][clickcounter]")
{
    // A right-click for the context menu between two left clicks does not end the double-click.
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Right) == 1);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    CHECK(counter.press(Here, MouseButton::Middle) == 1);
    CHECK(counter.press(There, MouseButton::Right) == 2);
}

TEST_CASE("ClickCounter.wheelStepsAndReleasesAreAlwaysSingle", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::WheelUp) == 1);
    CHECK(counter.press(Here, MouseButton::WheelUp) == 1);
    CHECK(counter.press(Here, MouseButton::Release) == 1);
    CHECK(counter.press(Here, MouseButton::Release) == 1);
}

TEST_CASE("ClickCounter.countsClicksForExactlyTheButtonsItCounts", "[semanticblocks][clickcounter]")
{
    using enum MouseButton;
    for (auto const button: { Left, Right, Middle, Release, WheelUp, WheelDown, WheelLeft, WheelRight })
    {
        INFO(std::format("{}", button));
        CHECK(ClickCounter::countsClicks(button)
              == std::ranges::contains(ClickCounter::CountedButtons, button));

        // A counted button continues a sequence; any other is a single click every time.
        auto clock = core::platform::ManualClock {};
        auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };
        (void) counter.press(Here, button);
        CHECK((counter.press(Here, button) == 2) == ClickCounter::countsClicks(button));
    }
    CHECK(ClickCounter::countsClicks(Left));
    CHECK_FALSE(ClickCounter::countsClicks(WheelUp));
}

TEST_CASE("ClickCounter.resetEndsEverySequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    CHECK(counter.press(Here, MouseButton::Right) == 1);
    counter.reset();
    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(Here, MouseButton::Right) == 1);
}

TEST_CASE("ClickCounter.aClockThatStepsBackStartsANewSequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 5s };
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.setNow(core::platform::SteadyTimePoint {} + 4s);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}
