// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/DesktopNotification.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <vthost/NotifyWire.hpp>

using vtbackend::DisplayOccasion;
using vthost::displayOccasionOf;
using vthost::wireOccasionOf;
using vthost::proto::NotifyOccasion;

TEST_CASE("every stated occasion reads back as itself", "[vthost][wire]")
{
    for (auto const occasion:
         { DisplayOccasion::Always, DisplayOccasion::Unfocused, DisplayOccasion::Invisible })
        CHECK(displayOccasionOf(wireOccasionOf(occasion)) == occasion);
}

TEST_CASE("an unstated occasion stays unstated, and an unknown one reads as unstated", "[vthost][wire]")
{
    CHECK(wireOccasionOf(std::nullopt) == NotifyOccasion::Unstated);
    CHECK_FALSE(displayOccasionOf(NotifyOccasion::Unstated).has_value());
    CHECK_FALSE(displayOccasionOf(static_cast<NotifyOccasion>(200)).has_value());
}
