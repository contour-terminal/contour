// SPDX-License-Identifier: Apache-2.0
#include <contour/config/SecureInputMode.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/window/SecureInputPolicy.hpp>

#include <vtpty/InputEcho.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
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

TEST_CASE("secureInputStateText says 'on' only for a pane the OS secures", "[secureinputpolicy]")
{
    // Review I3: the words are a claim about one pane. Every combination, so a new indicator is checked.
    for (auto const support: { SecureInputSupport::Native, SecureInputSupport::None })
        for (auto const indicator:
             { SecureIndicator::None, SecureIndicator::Locked, SecureIndicator::LockedSecured })
        {
            auto const claimsOn =
                support == SecureInputSupport::Native && indicator == SecureIndicator::LockedSecured;
            CHECK((contour::window::secureInputStateText(support, indicator) == "Secure input on")
                  == claimsOn);
            CHECK((contour::window::secureInputStateText(support, indicator) == "Input hidden") == !claimsOn);
        }
}

TEST_CASE("secureIndicatorUnder secures only the focused locked pane while the OS is on",
          "[secureinputpolicy]")
{
    using contour::window::KeyOwnership;
    for (auto const base: { SecureIndicator::None, SecureIndicator::Locked })
        for (auto const gate: { SecureInputState::Off, SecureInputState::On })
            for (auto const focus: { KeyOwnership::Elsewhere, KeyOwnership::Holder })
            {
                auto const secured = base == SecureIndicator::Locked && gate == SecureInputState::On
                                     && focus == KeyOwnership::Holder;
                CHECK(contour::window::secureIndicatorUnder(base, gate, focus)
                      == (secured ? SecureIndicator::LockedSecured : base));
            }
}

TEST_CASE("inputEchoPollFor polls only inside the window after the last activity", "[secureinputpolicy]")
{
    using contour::window::InputEchoPoll;
    using contour::window::InputEchoPollWindow;
    auto const t0 = std::chrono::steady_clock::time_point {} + std::chrono::hours { 1 };

    CHECK(contour::window::inputEchoPollFor(t0, std::nullopt) == InputEchoPoll::Idle);
    CHECK(contour::window::inputEchoPollFor(t0, t0) == InputEchoPoll::Polling);
    CHECK(contour::window::inputEchoPollFor(t0 + InputEchoPollWindow - std::chrono::milliseconds { 1 }, t0)
          == InputEchoPoll::Polling);
    CHECK(contour::window::inputEchoPollFor(t0 + InputEchoPollWindow, t0) == InputEchoPoll::Idle);
}
