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
bool pixels(HorizontalWheelGesture& gesture, int x, int y, ScrollPhase phase, bool inverted = false)
{
    return gesture.acceptsHorizontal(delta(x, y), NoDelta, phase, inverted);
}

/// Feeds an angle-only (wheel-style) event.
bool angle(HorizontalWheelGesture& gesture, int x, int y, ScrollPhase phase, bool inverted = false)
{
    return gesture.acceptsHorizontal(NoDelta, delta(x, y), phase, inverted);
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

    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::Update, false));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::NoPhase, false));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::Begin, false));
    CHECK_FALSE(gesture.acceptsHorizontal(NoDelta, NoDelta, ScrollPhase::End, false));
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

    CHECK_FALSE(gesture.acceptsHorizontal(delta(1, -40), delta(120, 0), ScrollPhase::Begin, false));
}

TEST_CASE("HorizontalWheelGesture.reset forgets the current gesture", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    CHECK_FALSE(pixels(gesture, 0, -40, ScrollPhase::Begin));
    CHECK_FALSE(pixels(gesture, 30, -5, ScrollPhase::Update));

    gesture.reset();

    CHECK(pixels(gesture, 30, -5, ScrollPhase::Update));
}

// {{{ Navigation steps — switching a tab is discrete, scrolling is not

TEST_CASE("HorizontalWheelGesture.a trackpad swipe is worth exactly one navigation step", "[contour][wheel]")
{
    // THE over-sensitivity fix. The scroll accumulator quantizes horizontal motion by CELL WIDTH, so a
    // single ~150px flick produces well over a dozen column steps. Left ungated that was a dozen tab
    // switches for one swipe; a browser moves one page for the same gesture.
    auto gesture = HorizontalWheelGesture {};

    REQUIRE(pixels(gesture, -30, 0, ScrollPhase::Begin));
    CHECK(gesture.consumeNavigationStep());

    // Every further event of the SAME gesture is refused, however long the swipe runs.
    for (auto i = 0; i < 20; ++i)
    {
        REQUIRE(pixels(gesture, -30, 0, ScrollPhase::Update));
        CHECK_FALSE(gesture.consumeNavigationStep());
    }
}

TEST_CASE("HorizontalWheelGesture.momentum does not extend a swipe into more steps", "[contour][wheel]")
{
    // A flick glides on after the finger lifts. That glide is the same gesture, so it must not keep
    // switching tabs.
    auto gesture = HorizontalWheelGesture {};

    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
    CHECK(gesture.consumeNavigationStep());

    REQUIRE(pixels(gesture, -30, 0, ScrollPhase::Momentum));
    CHECK_FALSE(gesture.consumeNavigationStep());
}

TEST_CASE("HorizontalWheelGesture.a new swipe earns a new step", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
    CHECK(gesture.consumeNavigationStep());
    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Update));
    CHECK_FALSE(gesture.consumeNavigationStep());

    // Finger lifted and put down again: a fresh gesture, a fresh step.
    pixels(gesture, 0, 0, ScrollPhase::End);
    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
    CHECK(gesture.consumeNavigationStep());
}

TEST_CASE("HorizontalWheelGesture.each discrete notch is worth its own step", "[contour][wheel]")
{
    // A wheel tilt has no gesture to bound it, and the detent is already the user's unit of intent — so
    // unlike a swipe, every notch counts.
    auto gesture = HorizontalWheelGesture {};

    for (auto i = 0; i < 5; ++i)
    {
        REQUIRE(angle(gesture, 120, 0, ScrollPhase::NoPhase));
        CHECK(gesture.consumeNavigationStep());
    }
}

TEST_CASE("HorizontalWheelGesture.reset releases the navigation latch", "[contour][wheel]")
{
    auto gesture = HorizontalWheelGesture {};

    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
    CHECK(gesture.consumeNavigationStep());
    CHECK_FALSE(gesture.consumeNavigationStep());

    gesture.reset();
    CHECK(gesture.consumeNavigationStep());
}

// }}}

TEST_CASE("HorizontalWheelGesture.every swipe earns a step when only phases are observed", "[contour][wheel]")
{
    // The regression this guards: a purely horizontal swipe carries no vertical motion, so the
    // smooth-scroll path consumes its Begin and End as zero-delta phase events and acceptsHorizontal()
    // never sees them. If the gesture learned its boundaries only from acceptsHorizontal(), the first
    // swipe would claim the navigation step and NO later swipe could ever claim another -- which looked
    // like "switching tabs only works while Shift is held", because a modifier skips that path entirely.
    auto gesture = HorizontalWheelGesture {};

    for (auto swipe = 0; swipe < 5; ++swipe)
    {
        // Only the phase is observed for Begin/End, exactly as the smooth-scroll path leaves it.
        gesture.notePhase(ScrollPhase::Begin);

        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Update));
        CHECK(gesture.consumeNavigationStep());
        // ... and the rest of that same swipe still yields nothing more.
        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Update));
        CHECK_FALSE(gesture.consumeNavigationStep());

        gesture.notePhase(ScrollPhase::End);
    }
}

TEST_CASE("HorizontalWheelGesture.notePhase does not end a gesture mid-swipe", "[contour][wheel]")
{
    // Update and Momentum are not boundaries: treating them as such would hand every event its own
    // navigation step and put the over-sensitivity straight back.
    auto gesture = HorizontalWheelGesture {};

    gesture.notePhase(ScrollPhase::Begin);
    REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Update));
    CHECK(gesture.consumeNavigationStep());

    gesture.notePhase(ScrollPhase::Update);
    CHECK_FALSE(gesture.consumeNavigationStep());
    gesture.notePhase(ScrollPhase::Momentum);
    CHECK_FALSE(gesture.consumeNavigationStep());
}

TEST_CASE("HorizontalWheelGesture.a swipe follows the finger, a wheel tilt does not", "[contour][wheel]")
{
    SECTION("a pixel-precise gesture navigates naturally")
    {
        auto gesture = HorizontalWheelGesture {};
        gesture.notePhase(ScrollPhase::Begin);
        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
        CHECK(gesture.usesNaturalDirection());
    }

    SECTION("a wheel tilt does not")
    {
        auto gesture = HorizontalWheelGesture {};
        REQUIRE(angle(gesture, 120, 0, ScrollPhase::NoPhase));
        CHECK_FALSE(gesture.usesNaturalDirection());
    }

    SECTION("the platform having already inverted the delta cancels it out")
    {
        // The double-inversion guard. On a platform whose natural-scrolling setting already flipped the
        // delta, the raw values ALREADY read as the carousel direction; inverting again would undo it
        // and send the user the wrong way.
        auto gesture = HorizontalWheelGesture {};
        gesture.notePhase(ScrollPhase::Begin);
        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin, /*inverted=*/true));
        CHECK_FALSE(gesture.usesNaturalDirection());
    }

    SECTION("both facts latch for the whole gesture")
    {
        // The event that finally claims the navigation step is usually not the one that opened the
        // gesture, and a trackpad interleaves pixel-precise events with angle-only ones inside a single
        // swipe. Judged per event, the direction would flip halfway through it.
        auto gesture = HorizontalWheelGesture {};
        gesture.notePhase(ScrollPhase::Begin);
        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
        CHECK(gesture.usesNaturalDirection());

        // An angle-only event arriving mid-swipe must not demote it to "wheel tilt".
        REQUIRE(angle(gesture, -120, 0, ScrollPhase::Update));
        CHECK(gesture.usesNaturalDirection());
    }

    SECTION("a boundary forgets it, so the next gesture is judged afresh")
    {
        auto gesture = HorizontalWheelGesture {};
        gesture.notePhase(ScrollPhase::Begin);
        REQUIRE(pixels(gesture, -40, 0, ScrollPhase::Begin));
        REQUIRE(gesture.usesNaturalDirection());

        gesture.notePhase(ScrollPhase::End);
        CHECK_FALSE(gesture.usesNaturalDirection());

        // A wheel tilt right after a swipe is a wheel tilt.
        REQUIRE(angle(gesture, 120, 0, ScrollPhase::NoPhase));
        CHECK_FALSE(gesture.usesNaturalDirection());
    }
}

TEST_CASE("horizontalNavigationButton states one rule for both wheel paths", "[contour][wheel]")
{
    using contour::horizontalNavigationButton;
    using vtbackend::MouseButton;

    // A wheel tilt means what it says.
    STATIC_CHECK(horizontalNavigationButton(/*towardsRight=*/true, /*naturalDirection=*/false)
                 == MouseButton::WheelRight);
    STATIC_CHECK(horizontalNavigationButton(/*towardsRight=*/false, /*naturalDirection=*/false)
                 == MouseButton::WheelLeft);

    // A swipe follows the fingers: dragging the content left reveals what is to the right.
    STATIC_CHECK(horizontalNavigationButton(/*towardsRight=*/false, /*naturalDirection=*/true)
                 == MouseButton::WheelRight);
    STATIC_CHECK(horizontalNavigationButton(/*towardsRight=*/true, /*naturalDirection=*/true)
                 == MouseButton::WheelLeft);
}
