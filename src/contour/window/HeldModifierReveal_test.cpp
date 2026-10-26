// SPDX-License-Identifier: Apache-2.0

#include <contour/window/HeldModifierReveal.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using contour::window::HeaderReveal;
using contour::window::HeldModifierReveal;

namespace
{
constexpr auto T0 = std::chrono::steady_clock::time_point { std::chrono::seconds { 1000 } };
}

TEST_CASE("heldModifier.revealsAfterTheDelay", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    CHECK(reveal.at(T0) == HeaderReveal::None);
    CHECK_FALSE(reveal.dueAt().has_value());

    reveal.modifierPressed(T0);
    CHECK(reveal.at(T0 + 499ms) == HeaderReveal::None);
    CHECK(reveal.at(T0 + 500ms) == HeaderReveal::HeldModifier);
    CHECK(reveal.dueAt() == T0 + 500ms);
}

TEST_CASE("heldModifier.otherInputDisarms", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.modifierPressed(T0);
    reveal.otherInput();
    CHECK(reveal.at(T0 + 600ms) == HeaderReveal::None);
    CHECK_FALSE(reveal.dueAt().has_value());

    reveal.modifierPressed(T0 + 700ms);
    CHECK(reveal.at(T0 + 1200ms) == HeaderReveal::HeldModifier);
}

TEST_CASE("heldModifier.releaseHides", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.modifierPressed(T0);
    REQUIRE(reveal.at(T0 + 600ms) == HeaderReveal::HeldModifier);
    reveal.modifierReleased();
    CHECK(reveal.at(T0 + 600ms) == HeaderReveal::None);
    CHECK_FALSE(reveal.dueAt().has_value());
}

TEST_CASE("heldModifier.aSecondPressRearms", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.modifierPressed(T0);
    reveal.modifierReleased();
    reveal.modifierPressed(T0 + 2s);
    CHECK(reveal.at(T0 + 2s + 499ms) == HeaderReveal::None);
    CHECK(reveal.at(T0 + 2s + 500ms) == HeaderReveal::HeldModifier);
    CHECK(reveal.dueAt() == T0 + 2s + 500ms);
}

TEST_CASE("heldModifier.aPressWhileArmedKeepsTheFirstTime", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.modifierPressed(T0);
    reveal.modifierPressed(T0 + 300ms);
    CHECK(reveal.at(T0 + 500ms) == HeaderReveal::HeldModifier);
    CHECK(reveal.dueAt() == T0 + 500ms);
}

TEST_CASE("heldModifier.aPointerPressDisarmsOnlyAPendingHold", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.modifierPressed(T0);
    reveal.pointerPressed(T0 + 100ms);
    CHECK(reveal.at(T0 + 600ms) == HeaderReveal::None);
    CHECK_FALSE(reveal.dueAt().has_value());

    reveal.modifierReleased();
    reveal.modifierPressed(T0 + 1s);
    REQUIRE(reveal.at(T0 + 1s + 500ms) == HeaderReveal::HeldModifier);
    reveal.pointerPressed(T0 + 1s + 600ms);
    CHECK(reveal.at(T0 + 1s + 700ms) == HeaderReveal::HeldModifier);
}

TEST_CASE("heldModifier.otherInputWhileReleasedIsIgnored", "[heldmodifier]")
{
    auto reveal = HeldModifierReveal { 500ms };
    reveal.otherInput();
    CHECK(reveal.at(T0) == HeaderReveal::None);
    CHECK_FALSE(reveal.dueAt().has_value());

    // A press afterwards is not pre-disarmed by that stray input.
    reveal.modifierPressed(T0);
    CHECK(reveal.at(T0 + 500ms) == HeaderReveal::HeldModifier);
}
