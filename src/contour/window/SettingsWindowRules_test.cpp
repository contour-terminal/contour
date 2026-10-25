// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsWindowRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <string_view>
#include <vector>

using contour::config::HostPlatform;
using contour::window::SettingsWindowButtons;
using contour::window::SettingsWindowPlacement;

TEST_CASE("every host has its conventions", "[contour][settingswindow][rules]")
{
    struct Expected
    {
        HostPlatform host;
        std::string_view title;
        SettingsWindowButtons buttons;
    };
    auto const expected = std::vector<Expected> {
        { HostPlatform::Other, "Settings", SettingsWindowButtons::CloseOnly },
        { HostPlatform::Windows, "Contour Settings", SettingsWindowButtons::All },
        { HostPlatform::MacOS, "Settings", SettingsWindowButtons::CloseOnly },
        { HostPlatform::KdePlasma, "Settings", SettingsWindowButtons::CloseOnly },
    };
    REQUIRE(expected.size() == contour::config::HostPlatformCount);
    for (auto const i: std::views::iota(std::size_t { 0 }, contour::config::HostPlatformCount))
    {
        auto const host = static_cast<HostPlatform>(i);
        auto const row = contour::window::settingsWindowConventionsFor(host);
        CHECK(row.host == host);
        CHECK(row.title == expected[i].title);
        CHECK(row.buttons == expected[i].buttons);
    }
}

TEST_CASE("the Settings window is placed", "[contour][settingswindow][rules]")
{
    struct Case
    {
        char const* name;
        SettingsWindowPlacement placement;
        QRect expected;
    };
    auto const preferred = QSize(960, 820);
    auto const minimum = QSize(720, 480);
    auto const big = QRect(0, 0, 1440, 875);
    auto const make = [&](QSize pref,
                          std::optional<QRect> previous,
                          QRect anchor,
                          std::vector<QRect> screens,
                          QMargins frame = {}) {
        return SettingsWindowPlacement { .preferred = pref,
                                         .minimum = minimum,
                                         .previous = previous,
                                         .anchor = anchor,
                                         .screens = std::move(screens),
                                         .frame = frame };
    };
    auto const small = QRect(0, 0, 1366, 728);
    auto const cases = std::vector<Case> {
        { "first show", make(preferred, std::nullopt, big, { big }), QRect(240, 27, 960, 820) },
        { "small screen", make(QSize(1110, 920), std::nullopt, small, { small }), QRect(128, 0, 1110, 728) },
        { "screen below the minimum",
          make(preferred, std::nullopt, QRect(0, 0, 600, 400), { QRect(0, 0, 600, 400) }),
          QRect(0, 0, 720, 480) },
        { "a title bar to keep on screen",
          make(preferred, std::nullopt, big, { big }, QMargins(0, 28, 0, 0)),
          QRect(240, 28, 960, 820) },
        { "reopened on a live second screen",
          make(preferred, QRect(1600, 100, 960, 820), big, { big, QRect(1440, 0, 1920, 1080) }),
          QRect(1600, 100, 960, 820) },
        { "reopened after its screen was unplugged",
          make(preferred, QRect(1600, 100, 960, 820), big, { big }),
          QRect(240, 27, 960, 820) },
        { "reopened half off its screen",
          make(preferred, QRect(1000, 500, 960, 820), big, { big }),
          QRect(480, 55, 960, 820) },
        { "a user's own size is kept",
          make(preferred, QRect(100, 100, 800, 500), big, { big }),
          QRect(100, 100, 800, 500) },
    };
    for (auto const& c: cases)
    {
        INFO(c.name);
        CHECK(contour::window::settingsWindowGeometry(c.placement) == c.expected);
    }
}
