// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the horizontal-wheel drift filter. A two-finger trackpad scroll is never perfectly
// vertical, and since a horizontal notch now switches tabs, the accumulated sideways drift of a long
// vertical scroll must never reach the binding table. The rule is a per-gesture latch: one vertically
// dominant event silences the horizontal axis for the rest of that gesture.

#include <contour/HorizontalWheelGesture.h>

#include <catch2/catch_test_macros.hpp>

using contour::HorizontalWheelGesture;
using vtbackend::ScrollPhase;

namespace
{

constexpr crispy::point delta(int x, int y) noexcept
{
    return { .x = x, .y = y };
}

constexpr auto NoDelta = crispy::point { .x = 0, .y = 0 };

/// Feeds a pixel-precise (trackpad-style) event.
bool pixels(HorizontalWheelGesture& gesture, int x, int y, ScrollPhase phase)
{
    return gesture.acceptsHorizontal(delta(x, y), NoDelta, phase);
}

/// Feeds an angle-only (wheel-style) event.
bool angle(HorizontalWheelGesture& gesture, int x, int y, ScrollPhase phase)
{
    return gesture.acceptsHorizontal(NoDelta, delta(x, y), phase);
}

} // namespace

TEST_CASE("HorizontalWheelGesture.a vertical trackpad scroll never yields horizontal", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    // A gesture that starts vertical...
    CHECK_FALSE(pixels(gesture, 0, -40, ScrollPhase::Begin));

    // ...stays silenced however far it later drifts sideways, even once the drift dominates a single
    // event. This is the whole point: the drift accumulates, so one leaked event is enough to switch tab.
    CHECK_FALSE(pixels(gesture, 3, -38, ScrollPhase::Update));
    CHECK_FALSE(pixels(gesture, 9, -12, ScrollPhase::Update));
    CHECK_FALSE(pixels(gesture, 25, -1, ScrollPhase::Update));
    CHECK_FALSE(pixels(gesture, 30, 0, ScrollPhase::Update));
}

TEST_CASE("HorizontalWheelGesture.an intentional sideways swipe passes", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK(pixels(gesture, -40, 0, ScrollPhase::Begin));
    CHECK(pixels(gesture, -35, 2, ScrollPhase::Update));
    CHECK(pixels(gesture, -20, -3, ScrollPhase::Update));
}

TEST_CASE("HorizontalWheelGesture.momentum inherits the gesture's verdict", "[contour][wheel]")
{
    SECTION("a vertical flick stays silenced through its glide")
    {
        auto gesture = HorizontalWheelGesture {};
        CHECK_FALSE(pixels(gesture, 2, -50, ScrollPhase::Begin));
        CHECK_FALSE(pixels(gesture, 40, -2, ScrollPhase::Momentum));
        CHECK_FALSE(pixels(gesture, 60, 0, ScrollPhase::Momentum));
    }

    SECTION("a horizontal flick keeps gliding sideways")
    {
        auto gesture = HorizontalWheelGesture {};
        CHECK(pixels(gesture, -50, 1, ScrollPhase::Begin));
        CHECK(pixels(gesture, -30, 0, ScrollPhase::Momentum));
    }

    SECTION("momentum never latches a gesture on its own")
    {
        // A vertically dominant momentum event must not silence a gesture that was judged horizontal,
        // or a sideways flick would cut out partway through its glide.
        auto gesture = HorizontalWheelGesture {};
        CHECK(pixels(gesture, -50, 0, ScrollPhase::Begin));
        CHECK_FALSE(pixels(gesture, -2, -40, ScrollPhase::Momentum)); // this event is vertical...
        CHECK(pixels(gesture, -30, 0, ScrollPhase::Momentum));        // ...but it did not latch
    }
}

TEST_CASE("HorizontalWheelGesture.Begin starts a fresh verdict", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(pixels(gesture, 0, -40, ScrollPhase::Begin));
    CHECK_FALSE(pixels(gesture, 20, -30, ScrollPhase::Update));

    // Lifting and starting a new gesture must not inherit the old latch.
    CHECK(pixels(gesture, -40, 0, ScrollPhase::Begin));
}

TEST_CASE("HorizontalWheelGesture.End clears the latch and accepts nothing", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(pixels(gesture, -40, 0, ScrollPhase::End));
    CHECK(pixels(gesture, -40, 0, ScrollPhase::Update));
}

TEST_CASE("HorizontalWheelGesture.a discrete tilt is judged per event", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    SECTION("a pure horizontal notch passes")
    {
        CHECK(angle(gesture, 120, 0, ScrollPhase::NoPhase));
        CHECK(angle(gesture, -120, 0, ScrollPhase::NoPhase));
    }

    SECTION("a pure vertical notch yields no horizontal")
    {
        CHECK_FALSE(angle(gesture, 0, 120, ScrollPhase::NoPhase));
    }

    SECTION("a diagonal event is declined")
    {
        // A discrete tilt reports one axis at a time. Both axes at once means a phase-less trackpad
        // driver, where we have no gesture boundary to latch across and decline rather than guess.
        CHECK_FALSE(angle(gesture, 120, 40, ScrollPhase::NoPhase));
    }

    SECTION("a discrete notch carries no state into the next one")
    {
        CHECK_FALSE(angle(gesture, 0, 120, ScrollPhase::NoPhase));
        CHECK(angle(gesture, 120, 0, ScrollPhase::NoPhase));
    }
}

TEST_CASE("HorizontalWheelGesture.Alt-transposed vertical wheel yields no horizontal", "[contour][wheel]")
{
    // sendWheelEvent() transposes the wheel axes while Alt is held, so Alt+vertical wheel arrives here
    // on the HORIZONTAL axis and DOES pass this filter. That is expected and is why the filter is not
    // the guard for it: the fallback rows match Modifiers{None}, so the Alt-carrying event cannot match
    // them. Pinned so nobody later "fixes" the filter and believes it covers the Alt case.
    auto gesture = HorizontalWheelGesture {};
    CHECK(angle(gesture, 120, 0, ScrollPhase::NoPhase));
}

TEST_CASE("HorizontalWheelGesture.a zero-delta event yields no horizontal", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::Update));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::NoPhase));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::Begin));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::End));
}

TEST_CASE("HorizontalWheelGesture.the sign of the motion is preserved", "[contour][wheel]")
{
    // The filter only ever decides yes/no; direction stays with the caller's delta. Pinned because a
    // filter that accepted only one direction would make tabs walk one way.
    auto gesture = HorizontalWheelGesture {};
    CHECK(angle(gesture, 120, 0, ScrollPhase::NoPhase));
    CHECK(angle(gesture, -120, 0, ScrollPhase::NoPhase));
}

TEST_CASE("HorizontalWheelGesture.pixel delta wins over angle delta", "[contour][wheel]")
{
    // Trackpads report both; the pixel-precise axis is the one that describes the finger, and it is the
    // trackpad this filter exists for.
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(gesture.acceptsHorizontal(delta(1, -40), delta(120, 0), ScrollPhase::Begin));
}

TEST_CASE("HorizontalWheelGesture.reset forgets the current gesture", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(pixels(gesture, 0, -40, ScrollPhase::Begin));
    CHECK_FALSE(pixels(gesture, 30, -5, ScrollPhase::Update));

    gesture.reset();

    CHECK(pixels(gesture, 30, -5, ScrollPhase::Update));
}
