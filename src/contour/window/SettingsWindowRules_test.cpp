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
        // m3: a size remembered from a larger screen, or one below today's minimum, is clamped.
        { "a remembered size larger than its screen",
          make(preferred, QRect(0, 0, 3000, 2000), big, { big }),
          QRect(0, 0, 1440, 875) },
        { "a remembered size below the minimum",
          make(preferred, QRect(100, 100, 300, 200), big, { big }),
          QRect(100, 100, 720, 480) },
        { "a remembered size larger than its screen, inside its frame",
          make(preferred, QRect(0, 28, 3000, 2000), big, { big }, QMargins(0, 28, 0, 0)),
          QRect(0, 28, 1440, 847) },
    };
    for (auto const& c: cases)
    {
        INFO(c.name);
        CHECK(contour::window::settingsWindowGeometry(c.placement) == c.expected);
    }
}

TEST_CASE("a maximised or full-screen Settings window keeps its geometry on reopen",
          "[contour][settingswindow][rules]")
{
    using contour::window::SettingsWindowReopen;
    using contour::window::settingsWindowReopenFor;

    struct Case
    {
        std::string_view name;
        Qt::WindowStates states;
        SettingsWindowReopen expected = SettingsWindowReopen::Place;
    };
    auto const cases = std::vector<Case> {
        { "normal", Qt::WindowNoState, SettingsWindowReopen::Place },
        { "minimised", Qt::WindowMinimized, SettingsWindowReopen::Place },
        { "maximised", Qt::WindowMaximized, SettingsWindowReopen::KeepGeometry },
        { "full screen", Qt::WindowFullScreen, SettingsWindowReopen::KeepGeometry },
        { "minimised from maximised",
          Qt::WindowMaximized | Qt::WindowMinimized,
          SettingsWindowReopen::KeepGeometry },
    };
    for (auto const& c: cases)
    {
        INFO(c.name);
        CHECK(settingsWindowReopenFor(c.states) == c.expected);
    }
}

TEST_CASE("the Settings window's own controls are its conventions' buttons where the desktop puts them",
          "[contour][settingswindow][rules]")
{
    using contour::config::WindowButtonLayout;
    using contour::window::settingsWindowControls;
    using enum contour::config::WindowControlKind;

    struct Case
    {
        std::string_view name;
        WindowButtonLayout placement;
        SettingsWindowButtons buttons;
        WindowButtonLayout expected;
    };
    auto const cases = std::vector<Case> {
        { "close only, trailing",
          { .leading = {}, .trailing = { Minimize, Maximize, Close } },
          SettingsWindowButtons::CloseOnly,
          { .leading = {}, .trailing = { Close } } },
        // GNOME's `close,minimize,maximize:` keeps close at the leading edge.
        { "close only, leading",
          { .leading = { Close, Minimize, Maximize }, .trailing = {} },
          SettingsWindowButtons::CloseOnly,
          { .leading = { Close }, .trailing = {} } },
        // A frameless window the desktop gives no close button could not be closed with the pointer: it
        // still draws one, at the trailing edge.
        { "close only, no buttons",
          {},
          SettingsWindowButtons::CloseOnly,
          { .leading = {}, .trailing = { Close } } },
        { "close only, close missing",
          { .leading = { Minimize }, .trailing = { Maximize } },
          SettingsWindowButtons::CloseOnly,
          { .leading = {}, .trailing = { Close } } },
        { "all",
          { .leading = { Close }, .trailing = { Minimize, Maximize } },
          SettingsWindowButtons::All,
          { .leading = { Close }, .trailing = { Minimize, Maximize } } },
    };
    for (auto const& c: cases)
    {
        INFO(c.name);
        CHECK(settingsWindowControls(c.placement, c.buttons) == c.expected);
    }
}

TEST_CASE("the Settings window draws its own frame only beside a client-drawn header bar",
          "[contour][settingswindow][rules]")
{
    using contour::config::HeaderBarArrangement;
    using contour::platform::WindowDecoration;
    using contour::window::settingsWindowDecoration;

    STATIC_CHECK(settingsWindowDecoration(HeaderBarArrangement::Unified, WindowDecoration::Client)
                 == WindowDecoration::Client);
    // The default profile asked for the OS's frame: the Settings window keeps it too.
    STATIC_CHECK(settingsWindowDecoration(HeaderBarArrangement::Unified, WindowDecoration::Server)
                 == WindowDecoration::Server);
    // Every row but the Linux header bar's keeps 4a's OS frame.
    STATIC_CHECK(settingsWindowDecoration(HeaderBarArrangement::Band, WindowDecoration::Client)
                 == WindowDecoration::Server);
    STATIC_CHECK(settingsWindowDecoration(HeaderBarArrangement::Band, WindowDecoration::Server)
                 == WindowDecoration::Server);
}
