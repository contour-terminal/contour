// SPDX-License-Identifier: Apache-2.0
#include <contour/window/HotkeyRefusals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using contour::platform::HotkeyError;
using contour::window::HotkeyRefusalRelay;

TEST_CASE("HotkeyRefusalRelay.aRefusalBeforeAnythingCanShowItIsKept", "[quickterminal][hotkeyrefusal]")
{
    auto relay = HotkeyRefusalRelay {};
    CHECK_FALSE(relay.problem().has_value());
    CHECK_FALSE(relay.takeUnannounced().has_value());

    relay.refused(HotkeyError::AlreadyTaken);

    // Settings, built later, finds it -- every time it is built.
    CHECK(relay.problem() == HotkeyError::AlreadyTaken);
    CHECK(relay.problem() == HotkeyError::AlreadyTaken);

    // The first window announces it once; Settings keeps it.
    CHECK(relay.takeUnannounced() == HotkeyError::AlreadyTaken);
    CHECK_FALSE(relay.takeUnannounced().has_value());
    CHECK(relay.problem() == HotkeyError::AlreadyTaken);
}

TEST_CASE("HotkeyRefusalRelay.aNewRefusalIsAnnouncedAgainAndReplacesTheOld", "[quickterminal][hotkeyrefusal]")
{
    auto relay = HotkeyRefusalRelay {};
    relay.refused(HotkeyError::AlreadyTaken);
    REQUIRE(relay.takeUnannounced().has_value());

    relay.refused(HotkeyError::Declined);
    CHECK(relay.problem() == HotkeyError::Declined);
    CHECK(relay.takeUnannounced() == HotkeyError::Declined);
}

TEST_CASE("HotkeyRefusalRelay.aLaterSuccessClearsTheProblem", "[quickterminal][hotkeyrefusal]")
{
    auto relay = HotkeyRefusalRelay {};
    relay.refused(HotkeyError::Failed);
    relay.bound();
    CHECK_FALSE(relay.problem().has_value());
    // A refusal that was never announced is not announced after it no longer stands.
    CHECK_FALSE(relay.takeUnannounced().has_value());
}
