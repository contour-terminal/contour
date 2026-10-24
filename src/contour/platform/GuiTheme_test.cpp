// SPDX-License-Identifier: Apache-2.0
//
// Which light or dark the GUI chrome takes for a configured theme, and the `terminal` theme in
// particular: the chrome follows the luminance of the terminal's own background.

#include <contour/config/Config.hpp>
#include <contour/platform/GuiTheme.hpp>

#include <vtbackend/core/Color.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using contour::config::ColorConfig;
using contour::config::DualColorConfig;
using contour::config::GuiTheme;
using contour::config::SimpleColorConfig;
using contour::platform::colorSchemeOfBackground;
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
