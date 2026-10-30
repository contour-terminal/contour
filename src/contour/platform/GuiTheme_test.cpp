// SPDX-License-Identifier: Apache-2.0
//
// Which light or dark the GUI chrome takes for a configured theme, and the `terminal` theme in
// particular: the chrome follows the luminance of the terminal's own background.

#include <contour/config/ConfigFwd.hpp>
#include <contour/platform/GuiTheme.hpp>

#include <vtbackend/core/Color.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <ranges>

using contour::config::ColorConfig;
using contour::config::DualColorConfig;
using contour::config::GuiTheme;
using contour::config::SimpleColorConfig;
using contour::platform::colorSchemeOfBackground;
using contour::platform::ContrastTheme;
using contour::platform::effectiveGuiTheme;
using contour::platform::qtColorSchemeFor;
using vtbackend::operator""_rgb;

namespace
{
ColorConfig simpleWithBackground(vtbackend::RGBColor background)
{
    auto simple = SimpleColorConfig {};
    simple.colors.defaultBackground = background;
    return ColorConfig { simple };
}
} // namespace

TEST_CASE("GuiTheme.colorSchemeOfBackground splits dark from light at the luminance threshold", "[platform]")
{
    CHECK(colorSchemeOfBackground(0x000000_rgb) == Qt::ColorScheme::Dark);
    CHECK(colorSchemeOfBackground(0x1C1E24_rgb) == Qt::ColorScheme::Dark);
    CHECK(colorSchemeOfBackground(0xFAFAF8_rgb) == Qt::ColorScheme::Light);
    CHECK(colorSchemeOfBackground(0xFFFFFF_rgb) == Qt::ColorScheme::Light);
}

TEST_CASE("GuiTheme.terminalFollowsBackground", "[platform]")
{
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, simpleWithBackground(0x1C1E24_rgb)) == Qt::ColorScheme::Dark);
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, simpleWithBackground(0xFAFAF8_rgb)) == Qt::ColorScheme::Light);
    // A dual config already follows the OS; the chrome must follow it too.
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, ColorConfig { DualColorConfig {} }) == std::nullopt);
    CHECK(qtColorSchemeFor(GuiTheme::Dark, simpleWithBackground(0xFAFAF8_rgb)) == Qt::ColorScheme::Dark);
    CHECK(qtColorSchemeFor(GuiTheme::Light, simpleWithBackground(0x000000_rgb)) == Qt::ColorScheme::Light);
    CHECK(qtColorSchemeFor(GuiTheme::System, simpleWithBackground(0x000000_rgb)) == std::nullopt);
    CHECK(contour::config::Config {}.theme.value() == GuiTheme::Terminal);
}

TEST_CASE("GuiTheme.liveBackgroundOutranksTheConfiguredOne", "[guitheme][chromefollow]")
{
    auto const simpleBlack = simpleWithBackground(0x000000_rgb);
    auto const dual = ColorConfig { DualColorConfig {} };
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, simpleBlack, 0xFAFAF8_rgb) == Qt::ColorScheme::Light);
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, dual, 0xFAFAF8_rgb) == std::nullopt);
    CHECK(qtColorSchemeFor(GuiTheme::Dark, simpleBlack, 0xFAFAF8_rgb) == Qt::ColorScheme::Dark);
}

TEST_CASE("GuiTheme.aContrastThemeDefersToTheSystemPalette", "[guitheme]")
{
    // Terminal is the last enumerator, so this walks every configurable theme.
    for (auto const value: std::views::iota(0, static_cast<int>(GuiTheme::Terminal) + 1))
    {
        auto const theme = static_cast<GuiTheme>(value);
        INFO("theme: " << value);
        CHECK(effectiveGuiTheme(theme, ContrastTheme::None) == theme);
        CHECK(effectiveGuiTheme(theme, ContrastTheme::Active) == GuiTheme::System);
    }
}

TEST_CASE("GuiTheme.osColorPreference prefers what the OS publishes over Qt's style hints", "[guitheme]")
{
    using contour::platform::osColorPreference;
    using vtbackend::ColorPreference;

    struct Row
    {
        std::optional<ColorPreference> published;
        Qt::ColorScheme qtScheme = Qt::ColorScheme::Unknown;
        ColorPreference expected = ColorPreference::Dark;
    };
    auto const rows = std::array {
        Row { .published = ColorPreference::Dark,
              .qtScheme = Qt::ColorScheme::Light,
              .expected = ColorPreference::Dark },
        Row { .published = ColorPreference::Light,
              .qtScheme = Qt::ColorScheme::Dark,
              .expected = ColorPreference::Light },
        Row {
            .published = std::nullopt, .qtScheme = Qt::ColorScheme::Dark, .expected = ColorPreference::Dark },
        Row { .published = std::nullopt,
              .qtScheme = Qt::ColorScheme::Light,
              .expected = ColorPreference::Light },
        Row { .published = std::nullopt,
              .qtScheme = Qt::ColorScheme::Unknown,
              .expected = ColorPreference::Light },
    };
    for (auto const& row: rows)
    {
        INFO("row " << (&row - rows.data()));
        CHECK(osColorPreference(row.published, row.qtScheme) == row.expected);
    }
}

TEST_CASE("GuiTheme.terminalColorPreference follows a pinned chrome and otherwise the OS", "[guitheme]")
{
    using contour::config::GuiTheme;
    using contour::platform::terminalColorPreference;
    using vtbackend::ColorPreference;

    constexpr auto Themes =
        std::array { GuiTheme::System, GuiTheme::Dark, GuiTheme::Light, GuiTheme::Terminal };
    constexpr auto OsPreferences = std::array { ColorPreference::Dark, ColorPreference::Light };
    auto const expectedFor = [](GuiTheme theme, ColorPreference os) {
        switch (theme)
        {
            case GuiTheme::Dark: return ColorPreference::Dark;
            case GuiTheme::Light: return ColorPreference::Light;
            case GuiTheme::System:
            case GuiTheme::Terminal: return os;
        }
        return os;
    };
    for (auto const theme: Themes)
        for (auto const os: OsPreferences)
        {
            INFO("theme " << static_cast<int>(theme) << ", os " << static_cast<int>(os));
            CHECK(terminalColorPreference(theme, os) == expectedFor(theme, os));
        }
}
