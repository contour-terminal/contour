// SPDX-License-Identifier: Apache-2.0
#include <contour/config/SecureInputMode.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/window/SecureInputPolicy.hpp>

#include <vtpty/InputEcho.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <ranges>

using contour::config::SecureInputMode;
using contour::platform::SecureInputState;
using contour::platform::SecureInputSupport;
using contour::window::AppActivity;
using contour::window::SecureIndicator;
using contour::window::SpotShown;
using vtpty::InputEcho;

namespace
{

constexpr auto Modes = std::array { SecureInputMode::Off, SecureInputMode::Auto };
constexpr auto Echoes =
    std::array { InputEcho::Unknown, InputEcho::Echoed, InputEcho::Raw, InputEcho::Hidden };
constexpr auto Activities = std::array { AppActivity::Background, AppActivity::Foreground };
constexpr auto Spots = std::array { SpotShown::No, SpotShown::Yes };

static_assert(Modes.size() == contour::config::SecureInputModeCount);
static_assert(Spots.size() == contour::window::SpotShownCount);

} // namespace

TEST_CASE("secureIndicatorFor locks only a hidden prompt under auto", "[secureinputpolicy]")
{
    // Review Focus 1 and 5: a raw-mode program and a device that cannot tell are never locked.
    for (auto const mode: Modes)
        for (auto const echo: Echoes)
        {
            INFO("mode " << static_cast<int>(mode) << ", echo " << static_cast<int>(echo));
            auto const expected = mode == SecureInputMode::Auto && echo == InputEcho::Hidden
                                      ? SecureIndicator::Locked
                                      : SecureIndicator::None;
            CHECK(contour::window::secureIndicatorFor(mode, echo) == expected);
        }
}

TEST_CASE("desiredSecureInput is on only for a focused hidden prompt in the front app", "[secureinputpolicy]")
{
    for (auto const mode: Modes)
        for (auto const app: Activities)
            for (auto const echo: Echoes)
            {
                INFO("mode " << static_cast<int>(mode) << ", app " << static_cast<int>(app) << ", echo "
                             << static_cast<int>(echo));
                auto const expected = mode == SecureInputMode::Auto && app == AppActivity::Foreground
                                              && echo == InputEcho::Hidden
                                          ? SecureInputState::On
                                          : SecureInputState::Off;
                CHECK(contour::window::desiredSecureInput(mode, app, echo) == expected);
            }
}

TEST_CASE("SecureIndicatorRules places the lock as R-f says", "[secureinputpolicy]")
{
    using contour::window::SecureIndicatorRules;
    using contour::window::secureIndicatorSpots;

    SECTION("one row per header, tab well and band combination")
    {
        CHECK(SecureIndicatorRules.size() == Spots.size() * Spots.size() * Spots.size());
        for (auto const header: Spots)
            for (auto const well: Spots)
                for (auto const band: Spots)
                    CHECK(std::ranges::count_if(SecureIndicatorRules,
                                                [&](auto const& rule) {
                                                    return rule.header == header && rule.tabWell == well
                                                           && rule.band == band;
                                                })
                          == 1);
    }

    SECTION("the lock is drawn on one surface, whatever is on screen")
    {
        // The header and the tab are one surface (the pane's own chrome); the chip and the pill are others.
        for (auto const& rule: SecureIndicatorRules)
        {
            auto const chrome = rule.lockInHeader == SpotShown::Yes || rule.lockOnTab == SpotShown::Yes;
            auto const surfaces =
                std::array { chrome, rule.bandChip == SpotShown::Yes, rule.lockInPane == SpotShown::Yes };
            CHECK(std::ranges::count(surfaces, true) == 1);
        }
    }

    SECTION("header and tab well: both draw it, band or not")
    {
        for (auto const band: Spots)
        {
            auto const& rule = secureIndicatorSpots(SpotShown::Yes, SpotShown::Yes, band);
            CHECK(rule.lockInHeader == SpotShown::Yes);
            CHECK(rule.lockOnTab == SpotShown::Yes);
            CHECK(rule.bandChip == SpotShown::No);
            CHECK(rule.lockInPane == SpotShown::No);
        }
    }

    SECTION("header only: the header")
    {
        for (auto const band: Spots)
        {
            auto const& rule = secureIndicatorSpots(SpotShown::Yes, SpotShown::No, band);
            CHECK(rule.lockInHeader == SpotShown::Yes);
            CHECK(rule.lockOnTab == SpotShown::No);
            CHECK(rule.bandChip == SpotShown::No);
            CHECK(rule.lockInPane == SpotShown::No);
        }
    }

    SECTION("tab well only: the tab")
    {
        for (auto const band: Spots)
        {
            auto const& rule = secureIndicatorSpots(SpotShown::No, SpotShown::Yes, band);
            CHECK(rule.lockInHeader == SpotShown::No);
            CHECK(rule.lockOnTab == SpotShown::Yes);
            CHECK(rule.bandChip == SpotShown::No);
            CHECK(rule.lockInPane == SpotShown::No);
        }
    }

    SECTION("neither, with a band: the band chip")
    {
        auto const& rule = secureIndicatorSpots(SpotShown::No, SpotShown::No, SpotShown::Yes);
        CHECK(rule.lockInHeader == SpotShown::No);
        CHECK(rule.lockOnTab == SpotShown::No);
        CHECK(rule.bandChip == SpotShown::Yes);
        CHECK(rule.lockInPane == SpotShown::No);
    }

    SECTION("neither, and no band either: a pill in the pane")
    {
        auto const& rule = secureIndicatorSpots(SpotShown::No, SpotShown::No, SpotShown::No);
        CHECK(rule.lockInHeader == SpotShown::No);
        CHECK(rule.lockOnTab == SpotShown::No);
        CHECK(rule.bandChip == SpotShown::No);
        CHECK(rule.lockInPane == SpotShown::Yes);
    }
}

TEST_CASE("secureInputStateText names what the OS does", "[secureinputpolicy]")
{
    CHECK(contour::window::secureInputStateText(SecureInputSupport::Native) == "Secure input on");
    CHECK(contour::window::secureInputStateText(SecureInputSupport::None) == "Input hidden");
}
