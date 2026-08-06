// SPDX-License-Identifier: Apache-2.0
//
// Tests for resolveChromeFont(), the config-to-font decision the whole terminal chrome is built on.
//
// It is worth its own file because nothing else exercises it: every other test constructs the
// provider through test/QmlChromeStyle.h, which hands it QGuiApplication::font() directly and so
// never runs this function at all. A regression here -- the wrong profile, ui_font_* applied to the
// native chrome, a lost fixed-pitch hint -- would first be seen by a user whose title bar and tab
// strip are drawn in the wrong font.

#include <contour/config/Config.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <text_shaper/Font.hpp>

#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>

using contour::config::UiStyle;
using contour::window::resolveChromeFont;

namespace
{

/// A config with two profiles whose fonts differ, which is what makes "which profile?" observable.
///
/// @return A config carrying `main` (12pt) and `big` (20pt), with `main` as the default.
[[nodiscard]] contour::config::Config twoProfileConfig()
{
    auto config = contour::config::Config {};

    auto& profiles = config.profiles.value();
    REQUIRE(profiles.contains("main"));

    auto& mainProfile = profiles.at("main");
    mainProfile.fonts.value().regular.familyName = "Main Mono";
    mainProfile.fonts.value().size = text::FontSize { 12.0 };

    auto big = mainProfile;
    big.fonts.value().regular.familyName = "Big Mono";
    big.fonts.value().size = text::FontSize { 20.0 };
    profiles["big"] = std::move(big);

    config.defaultProfileName = "main";
    return config;
}

} // namespace

TEST_CASE("resolveChromeFont: the native chrome keeps the platform UI font", "[contour][uistyle]")
{
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Native;

    // Not merely "some font": the point of the native style is that its chrome is indistinguishable
    // from any other application's, which is exactly what the platform font buys.
    CHECK(resolveChromeFont(config, "main") == QGuiApplication::font());

    // And the ui_font_* keys, which exist to put a cell-counting chrome on the terminal's grid, are
    // deliberately not applied to a chrome that has no grid -- setting one must not restyle it.
    config.uiFontFamily = "JetBrains Mono";
    config.uiFontSize = 18.0;
    CHECK(resolveChromeFont(config, "main") == QGuiApplication::font());
}

TEST_CASE("resolveChromeFont: the terminal chrome inherits the running profile's font", "[contour][uistyle]")
{
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Terminal;

    SECTION("the default profile, when that is what the window runs")
    {
        auto const font = resolveChromeFont(config, "main");
        CHECK(font.family() == QStringLiteral("Main Mono"));
        CHECK(font.pointSizeF() == 12.0);
    }

    SECTION("the profile actually launched, not the configured default")
    {
        // `contour profile=big` puts a 20pt grid on screen. A chrome quantized to the 12pt default's
        // cell would line up with nothing below it, which is the entire premise of the style.
        auto const font = resolveChromeFont(config, "big");
        CHECK(font.family() == QStringLiteral("Big Mono"));
        CHECK(font.pointSizeF() == 20.0);
    }

    SECTION("a fixed-pitch substitute when the family is missing")
    {
        // The style counts in character cells, so a proportional substitute would put the whole
        // chrome off the grid -- worse than a family that merely looks different.
        CHECK(resolveChromeFont(config, "main").styleHint() == QFont::Monospace);
    }
}

TEST_CASE("resolveChromeFont: ui_font_* override the profile's font", "[contour][uistyle]")
{
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Terminal;

    SECTION("family and size independently")
    {
        config.uiFontFamily = "JetBrains Mono";
        auto const family = resolveChromeFont(config, "big");
        CHECK(family.family() == QStringLiteral("JetBrains Mono"));
        CHECK(family.pointSizeF() == 20.0); // still the profile's size

        config.uiFontFamily = "";
        config.uiFontSize = 9.5;
        auto const size = resolveChromeFont(config, "big");
        CHECK(size.family() == QStringLiteral("Big Mono")); // still the profile's family
        CHECK(size.pointSizeF() == 9.5);
    }

    SECTION("the unset values inherit rather than clearing the font")
    {
        // Empty and 0 are the documented "inherit" values, so they must not reach QFont -- an empty
        // family or a zero point size would leave the chrome unrenderable rather than inherited.
        config.uiFontFamily = "";
        config.uiFontSize = 0.0;
        auto const font = resolveChromeFont(config, "main");
        CHECK(font.family() == QStringLiteral("Main Mono"));
        CHECK(font.pointSizeF() == 12.0);
    }
}

TEST_CASE("resolveChromeFont: an unknown profile still resolves to a usable font", "[contour][uistyle]")
{
    // Reachable from a mistyped `-p` and from a partial configuration whose `default_profile` names
    // a profile that is not there. The chrome has to come up either way; it just cannot promise to
    // match a grid it was given no font for (the fallback is logged, see UiStyleProvider.cpp).
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Terminal;

    auto const font = resolveChromeFont(config, "no-such-profile");
    CHECK(font.family() == QGuiApplication::font().family());
    CHECK(font.styleHint() == QFont::Monospace);

    // The explicit override still applies: it does not depend on a profile having been found.
    config.uiFontFamily = "JetBrains Mono";
    CHECK(resolveChromeFont(config, "no-such-profile").family() == QStringLiteral("JetBrains Mono"));
}

TEST_CASE("UiStyleProvider resolves the token row against the chrome font's cell", "[contour][uistyle]")
{
    // The provider is what turns a row of units into logical pixels, and the unit is the only
    // difference between the two styles. Pinning both ends of that keeps a token from being resolved
    // with the wrong multiplier -- which no QML test would notice, because the QML just reads it.
    auto const font = QGuiApplication::font();
    auto const metrics = QFontMetricsF(font);

    auto const native = contour::window::UiStyleProvider(UiStyle::Native, font);
    auto const& nativeTokens = contour::config::uiStyleTokens(UiStyle::Native);
    CHECK(native.widthQuantum() == 1.0); // pixels: every token passes through unscaled
    CHECK(native.labelPadding() == nativeTokens.labelPaddingUnits);
    CHECK(native.trailingPadding() == nativeTokens.trailingPaddingUnits);
    CHECK(native.tabSlack() == nativeTokens.tabSlackUnits);

    auto const terminal = contour::window::UiStyleProvider(UiStyle::Terminal, font);
    auto const& terminalTokens = contour::config::uiStyleTokens(UiStyle::Terminal);
    CHECK(terminal.widthQuantum() == terminal.cellWidth());
    CHECK(terminal.cellWidth() == metrics.horizontalAdvance(QStringLiteral("M")));
    CHECK(terminal.labelPadding() == terminalTokens.labelPaddingUnits * terminal.cellWidth());
    // Both ends of the tab, in cells. The trailing one is what keeps the close glyph off the rule
    // this style ends every tab with.
    CHECK(terminal.trailingPadding() == terminalTokens.trailingPaddingUnits * terminal.cellWidth());
    CHECK(terminal.trailingPadding() > 0.0);
    CHECK(terminal.tabSlack() == 0.0); // a cell of padding at either end is already the breathing room

    // The three translucent overlays are the token row's alphas applied to a palette color, so they
    // keep the color and change only its opacity -- a scrim that dropped the hue would dim the window
    // to grey rather than to shadow.
    auto const shadow = QColor(0x20, 0x30, 0x40);
    for (auto const style: { UiStyle::Native, UiStyle::Terminal })
    {
        auto const provider = contour::window::UiStyleProvider(style, font);
        auto const& tokens = contour::config::uiStyleTokens(style);
        INFO("style: " << static_cast<int>(style));

        // One 8-bit step of margin: QColor stores alpha as a byte, so a percentage never survives
        // the round trip exactly and an equality here would be asserting the quantization, not the
        // token.
        auto const percent = [](int value) {
            return Catch::Approx(static_cast<double>(value) / 100.0).margin(1.0 / 255.0);
        };

        CHECK(provider.modalScrim(shadow).rgb() == shadow.rgb());
        CHECK(provider.modalScrim(shadow).alphaF() == percent(tokens.modalScrimPercent));
        CHECK(provider.modelessScrim(shadow).alphaF() == percent(tokens.modelessScrimPercent));
        // A modal popup must read as more firmly "not now" than a merely dimmed one.
        CHECK(provider.modalScrim(shadow).alphaF() > provider.modelessScrim(shadow).alphaF());
        CHECK(provider.wash(shadow).alphaF() == percent(tokens.hoverWashPercent));
    }

    // QML reaches these by NAME through the meta-object -- the style's Popup.qml and Menu.qml write
    // `chromeStyle.modalScrim(control.palette.shadow)` -- so being Q_INVOKABLE is part of the
    // contract, not an implementation detail. A plain member function leaves the binding resolving to
    // undefined, i.e. a fully transparent scrim, behind a runtime warning nobody reads.
    auto styleProvider = contour::window::UiStyleProvider(UiStyle::Terminal, font);
    for (auto const* name: { "wash", "modalScrim", "modelessScrim" })
    {
        INFO("method: " << name);
        auto overlay = QColor {};
        REQUIRE(QMetaObject::invokeMethod(
            &styleProvider, name, Q_RETURN_ARG(QColor, overlay), Q_ARG(QColor, QColor(Qt::black))));
        CHECK(overlay.alphaF() > 0.0); // an invisible overlay is the bug, not the feature
        CHECK(overlay.alphaF() < 1.0); // and an opaque one hides what it is laid over
    }

    // Every glyph the chrome draws has to arrive in QML as a non-empty string, or the control that
    // draws it silently disappears. The separator is the documented exception.
    for (auto const style: { UiStyle::Native, UiStyle::Terminal })
    {
        auto const provider = contour::window::UiStyleProvider(style, font);
        INFO("style: " << static_cast<int>(style));
        CHECK(!provider.closeGlyph().isEmpty());
        CHECK(!provider.zoomGlyph().isEmpty());
        CHECK(!provider.newTabGlyph().isEmpty());
        CHECK(!provider.menuGlyph().isEmpty());
        CHECK(!provider.submenuGlyph().isEmpty());
    }
}
