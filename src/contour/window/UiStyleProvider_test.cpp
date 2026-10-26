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
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/window/ChromeStyleHost.hpp>
#include <contour/window/ChromeTokenObjects.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TitleBandSurface.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <text_shaper/Font.hpp>

#include <QtCore/QByteArrayView>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/SessionModel.hpp>

using contour::config::UiStyle;
using contour::window::resolveChromeFont;

namespace
{

/// The chrome row a style resolves to on a host with no platform convention of its own, so the
/// assertions below do not depend on the machine running them.
[[nodiscard]] contour::config::ChromeStyle rowOf(UiStyle style)
{
    return contour::config::resolveChromeStyle(style, contour::config::HostPlatform::Other);
}

/// @return The row @p config's `ui_style` resolves to, per rowOf().
[[nodiscard]] contour::config::ChromeStyle chromeRowOf(contour::config::Config const& config)
{
    return rowOf(config.uiStyle.value());
}

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
    CHECK(resolveChromeFont(config, chromeRowOf(config), "main") == QGuiApplication::font());

    // And the ui_font_* keys, which exist to put a cell-counting chrome on the terminal's grid, are
    // deliberately not applied to a chrome that has no grid -- setting one must not restyle it.
    config.uiFontFamily = "JetBrains Mono";
    config.uiFontSize = 18.0;
    CHECK(resolveChromeFont(config, chromeRowOf(config), "main") == QGuiApplication::font());
}

TEST_CASE("resolveChromeFont: the terminal chrome inherits the running profile's font", "[contour][uistyle]")
{
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Terminal;

    SECTION("the default profile, when that is what the window runs")
    {
        auto const font = resolveChromeFont(config, chromeRowOf(config), "main");
        CHECK(font.family() == QStringLiteral("Main Mono"));
        CHECK(font.pointSizeF() == 12.0);
    }

    SECTION("the profile actually launched, not the configured default")
    {
        // `contour profile=big` puts a 20pt grid on screen. A chrome quantized to the 12pt default's
        // cell would line up with nothing below it, which is the entire premise of the style.
        auto const font = resolveChromeFont(config, chromeRowOf(config), "big");
        CHECK(font.family() == QStringLiteral("Big Mono"));
        CHECK(font.pointSizeF() == 20.0);
    }

    SECTION("a fixed-pitch substitute when the family is missing")
    {
        // The style counts in character cells, so a proportional substitute would put the whole
        // chrome off the grid -- worse than a family that merely looks different.
        CHECK(resolveChromeFont(config, chromeRowOf(config), "main").styleHint() == QFont::Monospace);
    }
}

TEST_CASE("resolveChromeFont: ui_font_* override the profile's font", "[contour][uistyle]")
{
    auto config = twoProfileConfig();
    config.uiStyle = UiStyle::Terminal;

    SECTION("family and size independently")
    {
        config.uiFontFamily = "JetBrains Mono";
        auto const family = resolveChromeFont(config, chromeRowOf(config), "big");
        CHECK(family.family() == QStringLiteral("JetBrains Mono"));
        CHECK(family.pointSizeF() == 20.0); // still the profile's size

        config.uiFontFamily = "";
        config.uiFontSize = 9.5;
        auto const size = resolveChromeFont(config, chromeRowOf(config), "big");
        CHECK(size.family() == QStringLiteral("Big Mono")); // still the profile's family
        CHECK(size.pointSizeF() == 9.5);
    }

    SECTION("a generic family is the one the grid is drawn in")
    {
        // Qt knows `monospace` only through fontconfig; elsewhere it picks a substitute of its own,
        // which is not the grid's, and a chrome counting in that font's cells would not line up.
        config.uiFontFamily = "monospace";
        auto const font = resolveChromeFont(config, chromeRowOf(config), "main");
        CHECK(font.family() == QString::fromUtf8(text::platformFontFamily("monospace")));
    }

    SECTION("the unset values inherit rather than clearing the font")
    {
        // Empty and 0 are the documented "inherit" values, so they must not reach QFont -- an empty
        // family or a zero point size would leave the chrome unrenderable rather than inherited.
        config.uiFontFamily = "";
        config.uiFontSize = 0.0;
        auto const font = resolveChromeFont(config, chromeRowOf(config), "main");
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

    auto const font = resolveChromeFont(config, chromeRowOf(config), "no-such-profile");
    CHECK(font.family() == QGuiApplication::font().family());
    CHECK(font.styleHint() == QFont::Monospace);

    // The explicit override still applies: it does not depend on a profile having been found.
    config.uiFontFamily = "JetBrains Mono";
    CHECK(resolveChromeFont(config, chromeRowOf(config), "no-such-profile").family()
          == QStringLiteral("JetBrains Mono"));
}

TEST_CASE("UiStyleProvider resolves the token row against the chrome font's cell", "[contour][uistyle]")
{
    // The provider is what turns a row of units into logical pixels, and the unit is the only
    // difference between the two styles. Pinning both ends of that keeps a token from being resolved
    // with the wrong multiplier -- which no QML test would notice, because the QML just reads it.
    auto const font = QGuiApplication::font();
    auto const metrics = QFontMetricsF(font);

    auto const native = contour::window::UiStyleProvider(rowOf(UiStyle::Native), font);
    auto const& nativeTokens = contour::config::uiStyleTokens(rowOf(UiStyle::Native));
    CHECK(native.widthQuantum() == 1.0); // pixels: every token passes through unscaled
    CHECK(native.labelPadding() == nativeTokens.labelPaddingUnits);
    CHECK(native.trailingPadding() == nativeTokens.trailingPaddingUnits);
    CHECK(native.tabSlack() == nativeTokens.tabSlackUnits);

    auto const terminal = contour::window::UiStyleProvider(rowOf(UiStyle::Terminal), font);
    auto const& terminalTokens = contour::config::uiStyleTokens(rowOf(UiStyle::Terminal));
    CHECK(terminal.widthQuantum() == terminal.cellWidth());
    CHECK(terminal.cellWidth() == metrics.horizontalAdvance(QStringLiteral("M")));
    CHECK(terminal.labelPadding() == terminalTokens.labelPaddingUnits * terminal.cellWidth());
    // Both ends of the tab, in cells. The trailing one is what keeps the close glyph off the rule
    // this style ends every tab with.
    CHECK(terminal.trailingPadding() == terminalTokens.trailingPaddingUnits * terminal.cellWidth());
    CHECK(terminal.trailingPadding() > 0.0);
    CHECK(terminal.tabSlack() == 0.0); // a cell of padding at either end is already the breathing room

    // The window-control extents go through the SAME quantum, which is the whole reason they live in
    // this table rather than in the window-control style's own row: it is what lets a macOS-style
    // traffic light land on a whole cell under `ui_style: terminal` instead of straddling two.
    CHECK(native.trafficLightDotSize() == nativeTokens.trafficLightDotUnits);
    CHECK(native.trafficLightGap() == nativeTokens.trafficLightGapUnits);
    CHECK(native.windowControlInset() == nativeTokens.windowControlInsetUnits);
    CHECK(native.windowControlGutter() == nativeTokens.windowControlGutterUnits);

    CHECK(terminal.trafficLightDotSize() == terminal.cellWidth());
    CHECK(terminal.trafficLightGap() == terminal.cellWidth());
    CHECK(terminal.windowControlInset() == terminal.cellWidth());
    CHECK(terminal.windowControlGutter() == terminal.cellWidth());

    // Note the check above is against cellWidth, not cellHeight: a dot is a circle, and resolving its
    // width and height through different quanta would make it an ellipse wherever the cell is not
    // square -- which is every font this chrome is ever set in.

    // The glyph is the switch the QML branches on. Empty means "draw the shape", so a style that
    // stated one would silently change which delegate every window control uses.
    CHECK(native.trafficLightGlyph().isEmpty());
    CHECK(terminal.trafficLightGlyph() == QStringLiteral("●"));

    // The three translucent overlays are the token row's alphas applied to a palette color, so they
    // keep the color and change only its opacity -- a scrim that dropped the hue would dim the window
    // to grey rather than to shadow.
    auto const shadow = QColor(0x20, 0x30, 0x40);
    for (auto const style: { UiStyle::Native, UiStyle::Terminal })
    {
        auto const provider = contour::window::UiStyleProvider(rowOf(style), font);
        auto const& tokens = contour::config::uiStyleTokens(rowOf(style));
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
    auto styleProvider = contour::window::UiStyleProvider(rowOf(UiStyle::Terminal), font);
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
        auto const provider = contour::window::UiStyleProvider(rowOf(style), font);
        INFO("style: " << static_cast<int>(style));
        CHECK(!provider.closeGlyph().isEmpty());
        CHECK(!provider.zoomGlyph().isEmpty());
        CHECK(!provider.newTabGlyph().isEmpty());
        CHECK(!provider.menuGlyph().isEmpty());
        CHECK(!provider.submenuGlyph().isEmpty());
    }
}

namespace
{

/// Runs @p body with the application palette's Window colour set to @p window, restoring it after.
template <typename Body>
void withWindowColor(QColor const& window, Body&& body)
{
    auto const saved = QGuiApplication::palette();
    auto palette = saved;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::Highlight, QColor(0x12, 0x34, 0x56));
    QGuiApplication::setPalette(palette);
    std::forward<Body>(body)();
    QGuiApplication::setPalette(saved);
}

} // namespace

TEST_CASE("UiStyleProvider.statusColor picks the light or dark pair by the palette's window",
          "[contour][uistyle]")
{
    using contour::config::StatusColorRole;
    auto const provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    auto const role = [](StatusColorRole value) {
        return static_cast<int>(value);
    };

    withWindowColor(QColor(0xF6, 0xF6, 0xF6), [&] {
        CHECK(provider.statusColor(0) == QColor(0xD7, 0x00, 0x15));
        CHECK(provider.statusColor(role(StatusColorRole::Warning)) == QColor(0xC9, 0x34, 0x00));
        CHECK(provider.statusColor(role(StatusColorRole::Success)) == QColor(0x24, 0x8A, 0x3D));
    });

    withWindowColor(QColor(0x1E, 0x1E, 0x1E), [&] {
        CHECK(provider.statusColor(0) == QColor(0xFF, 0x69, 0x61));
        CHECK(provider.statusColor(role(StatusColorRole::Warning)) == QColor(0xFF, 0x9F, 0x0A));
        CHECK(provider.statusColor(role(StatusColorRole::Success)) == QColor(0x32, 0xD7, 0x4B));
    });
}

TEST_CASE("UiStyleProvider.statusColor: Running is the palette accent and bad roles are transparent",
          "[contour][uistyle]")
{
    auto const provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    auto const running = static_cast<int>(contour::config::StatusColorRole::Running);

    for (auto const window: { QColor(0xF6, 0xF6, 0xF6), QColor(0x1E, 0x1E, 0x1E) })
        withWindowColor(window, [&] {
            CHECK(provider.statusColor(running) == QColor(0x12, 0x34, 0x56));
            CHECK(provider.statusColor(-1).alpha() == 0);
            CHECK(provider.statusColor(99).alpha() == 0);
        });
}

TEST_CASE("UiStyleProvider publishes the pane-dim, notice and font-scale tokens", "[contour][uistyle]")
{
    auto const font = QGuiApplication::font();
    auto const provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), font);

    CHECK(provider.paneDimColor().rgb() == QColor(Qt::black).rgb());
    CHECK(provider.dotGlyphColor().alpha() == 153);
    CHECK(provider.captionFont().pointSizeF() < font.pointSizeF());
    CHECK(provider.headlineFont().pointSizeF() > font.pointSizeF());
}

TEST_CASE("UiStyleProvider.onStatusColor picks the more legible of black and white", "[contour][uistyle]")
{
    using contour::config::StatusColorRole;
    auto const provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    auto const danger = static_cast<int>(StatusColorRole::Danger);

    auto const ratio = [](QColor const& a, QColor const& b) {
        return vtbackend::contrastRatio(contour::platform::toRGBColor(a), contour::platform::toRGBColor(b));
    };

    withWindowColor(QColor(0x1E, 0x1E, 0x1E), [&] {
        CHECK(provider.onStatusColor(danger) == QColor(Qt::black));
        CHECK(ratio(provider.onStatusColor(danger), provider.statusColor(danger)) >= 4.5);
    });
    withWindowColor(QColor(0xF6, 0xF6, 0xF6), [&] {
        CHECK(provider.onStatusColor(danger) == QColor(Qt::white));
        CHECK(ratio(provider.onStatusColor(danger), provider.statusColor(danger)) >= 4.5);
    });
}

TEST_CASE("chromeStyle names the status roles as properties in QML", "[contour][uistyle][qml]")
{
    using contour::window::UiStyleProvider;
    auto engine = QQmlEngine();
    contour::test::installChromeStyle(engine);

    auto const evaluate = [&engine](char const* expression) {
        auto qmlExpression = QQmlExpression(engine.rootContext(), nullptr, QString::fromLatin1(expression));
        auto ok = false;
        auto const value = qmlExpression.evaluate(&ok);
        REQUIRE(!qmlExpression.hasError());
        return value;
    };

    // Not undefined (which toInt() would also report as 0):
    CHECK(evaluate("typeof chromeStyle.statusDanger").toString() == QStringLiteral("number"));
    // Not undefined: statusColor(undefined) would quietly be transparent and nothing else would notice.
    CHECK(evaluate("chromeStyle.statusDanger").toInt() == 0);
    CHECK(evaluate("chromeStyle.statusWarning").toInt() == 1);
    CHECK(evaluate("chromeStyle.statusSuccess").toInt() == 2);
    CHECK(evaluate("chromeStyle.statusRunning").toInt() == 3);

    withWindowColor(QColor(0xF6, 0xF6, 0xF6), [&] {
        auto const color = evaluate("chromeStyle.statusColor(chromeStyle.statusDanger)").value<QColor>();
        CHECK(color == QColor(0xD7, 0x00, 0x15));
    });

    // A new config::StatusColorRole must get a QML name too: one `status...` property per role.
    auto const& meta = UiStyleProvider::staticMetaObject;
    auto const statusProperties =
        std::ranges::count_if(std::views::iota(meta.propertyOffset(), meta.propertyCount()), [](int index) {
            return QByteArrayView(UiStyleProvider::staticMetaObject.property(index).name())
                .startsWith("status");
        });
    CHECK(static_cast<size_t>(statusProperties) == contour::config::StatusColorRoleCount);
}

TEST_CASE("UiStyleProvider.paletteRevision changes when the application palette does", "[contour][uistyle]")
{
    auto provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    auto notified = 0;
    QObject::connect(
        &provider, &contour::window::UiStyleProvider::paletteRevisionChanged, [&] { ++notified; });

    auto const before = provider.paletteRevision();
    withWindowColor(QColor(0x1E, 0x1E, 0x1E), [&] { CHECK(provider.paletteRevision() != before); });
    CHECK(notified >= 2); // Once for the change, once for the restore.
}

TEST_CASE("chromeStyle.statusColor bindings re-evaluate on a palette change", "[contour][uistyle][qml]")
{
    auto engine = QQmlEngine();
    contour::test::installChromeStyle(engine);

    auto component = QQmlComponent(&engine);
    component.setData("import QtQuick\n"
                      "QtObject {\n"
                      "    property color danger: chromeStyle.paletteRevision < 0\n"
                      "        ? \"transparent\" : chromeStyle.statusColor(chromeStyle.statusDanger)\n"
                      "}\n",
                      QUrl());
    withWindowColor(QColor(0xF6, 0xF6, 0xF6), [&] {
        auto const object = std::unique_ptr<QObject>(component.create());
        REQUIRE(object);
        CHECK(object->property("danger").value<QColor>() == QColor(0xD7, 0x00, 0x15));

        withWindowColor(QColor(0x1E, 0x1E, 0x1E), [&] {
            CHECK(object->property("danger").value<QColor>() == QColor(0xFF, 0x69, 0x61));
        });
    });
}

namespace
{

/// @return @p provider's group @p group, as QML reaches it through `chromeStyle.<group>`.
[[nodiscard]] QObject* groupOf(contour::window::UiStyleProvider const& provider, char const* group)
{
    auto* object = provider.property(group).value<QObject*>();
    REQUIRE(object != nullptr);
    return object;
}

/// @return Token @p name of @p provider's group @p group.
[[nodiscard]] QVariant tokenOf(contour::window::UiStyleProvider const& provider,
                               char const* group,
                               char const* name)
{
    auto const value = groupOf(provider, group)->property(name);
    REQUIRE(value.isValid());
    return value;
}

} // namespace

TEST_CASE("UiStyleProvider publishes the 1a token groups per row", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();

    auto const mac = UiStyleProvider(chromeInputsFor(ChromeStyle::MacOS, font));
    CHECK(tokenOf(mac, "size", "toast").toReal() == 36.0);
    CHECK(tokenOf(mac, "radii", "toast").toReal() == 18.0);
    CHECK(tokenOf(mac, "size", "swatchWidth").toReal() == 16.0);
    CHECK(tokenOf(mac, "size", "swatchHeight").toReal() == 16.0);
    CHECK(tokenOf(mac, "size", "settingsWidth").toReal() == 960.0);
    CHECK(tokenOf(mac, "size", "settingsMinHeight").toReal() == 480.0);
    CHECK(tokenOf(mac, "motion", "standard").toInt() == 160);
    CHECK(tokenOf(mac, "space", "s10").toReal() == 32.0);
    CHECK(tokenOf(mac, "material", "titleBand").toInt() == tokenOf(mac, "material", "solid").toInt());
    CHECK(tokenOf(mac, "layout", "dialog").toInt() == tokenOf(mac, "layout", "sheet").toInt());
    CHECK(tokenOf(mac, "contrast", "separatorAlpha").toReal() == Catch::Approx(0.10));
    CHECK(tokenOf(mac, "type", "body").value<QFont>().pixelSize() == 13);
    auto* e2 = tokenOf(mac, "elevation", "e2").value<QObject*>();
    REQUIRE(e2 != nullptr);
    CHECK(e2->property("blur").toReal() == 16.0);
    CHECK(e2->property("opacity").toReal() == Catch::Approx(0.25));

    auto const terminal = UiStyleProvider(chromeInputsFor(ChromeStyle::Terminal, font));
    CHECK(tokenOf(terminal, "size", "toast").toReal() == terminal.cellHeight());
    CHECK(tokenOf(terminal, "size", "dialogWidth").toReal() == Catch::Approx(32 * terminal.widthQuantum()));
    // A swatch is a two-cell block, one row high.
    CHECK(tokenOf(terminal, "size", "swatchWidth").toReal() == Catch::Approx(2 * terminal.cellWidth()));
    CHECK(tokenOf(terminal, "size", "swatchHeight").toReal() == Catch::Approx(terminal.cellHeight()));
    CHECK(tokenOf(terminal, "size", "settingsWidth").toReal()
          == Catch::Approx(120 * terminal.widthQuantum()));
    CHECK(tokenOf(terminal, "size", "settingsHeight").toReal() == Catch::Approx(40 * terminal.cellHeight()));
    CHECK(tokenOf(terminal, "size", "settingsMinWidth").toReal()
          == Catch::Approx(90 * terminal.widthQuantum()));
    CHECK(tokenOf(terminal, "radii", "toast").toReal() == 0.0);
    CHECK(tokenOf(terminal, "tui", "toastStyle").toInt() == tokenOf(terminal, "tui", "inverse").toInt());
    CHECK(tokenOf(terminal, "tui", "inactivePaneOpacity").toReal() == Catch::Approx(0.62));
    // A zero type size is the chrome font itself, which keeps the Terminal row on the grid.
    CHECK(tokenOf(terminal, "type", "title").value<QFont>() == font);
}

TEST_CASE("UiStyleProvider applies Reduce Motion", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::MotionPreference;
    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());
    inputs.accessibility.motion = MotionPreference::Reduced;
    auto const provider = contour::window::UiStyleProvider(inputs);

    CHECK(tokenOf(provider, "motion", "fast").toInt() == 100);
    CHECK(tokenOf(provider, "motion", "standard").toInt() == 100);
    CHECK(tokenOf(provider, "motion", "emphasized").toInt() == 100);
    CHECK(tokenOf(provider, "motion", "travel").toReal() == 0.0);
}

TEST_CASE("UiStyleProvider takes Running from the platform accent", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::window::UiStyleProvider;
    using vtbackend::operator""_rgb;
    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());

    SECTION("with a platform accent")
    {
        inputs.appearance.accent = 0xFF00AA_rgb;
        auto const provider = UiStyleProvider(inputs);
        CHECK(provider.statusColor(UiStyleProvider::statusRunning()) == QColor(0xFF, 0x00, 0xAA));
        CHECK(provider.accentColor() == QColor(0xFF, 0x00, 0xAA));
        CHECK(provider.property("accentColor").value<QColor>() == QColor(0xFF, 0x00, 0xAA));
    }

    SECTION("without one")
    {
        auto const provider = UiStyleProvider(inputs);
        auto const highlight = QGuiApplication::palette().color(QPalette::Highlight);
        CHECK(provider.statusColor(UiStyleProvider::statusRunning()) == highlight);
        CHECK(provider.accentColor() == highlight);
    }
}

TEST_CASE("UiStyleProvider resolves the colour roles", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::SchemeSlot;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();

    auto terminalInputs = contour::window::chromeInputsFor(ChromeStyle::Terminal, font);
    terminalInputs.scheme.bySlot[static_cast<size_t>(SchemeSlot::Bar)] = vtbackend::RGBColor(0x123456);
    auto const terminal = UiStyleProvider(terminalInputs);
    CHECK(tokenOf(terminal, "colors", "bannerFill").value<QColor>() == QColor(0x12, 0x34, 0x56));

    auto const mac = UiStyleProvider(contour::window::chromeInputsFor(ChromeStyle::MacOS, font));
    auto const toastFill = tokenOf(mac, "colors", "toastFill").value<QColor>();
    CHECK(toastFill.rgb() == QColor(0x28, 0x28, 0x2C).rgb());
    CHECK(toastFill.alphaF() == Catch::Approx(0.92).margin(0.005));

    // A palette source follows the application palette, and says so.
    auto notified = 0;
    auto const* colors = qobject_cast<contour::window::ChromeColorTokens const*>(groupOf(mac, "colors"));
    REQUIRE(colors != nullptr);
    QObject::connect(colors, &contour::window::ChromeColorTokens::changed, [&] { ++notified; });
    withWindowColor(QColor(0x1E, 0x1E, 0x1E), [&] {
        CHECK(tokenOf(mac, "colors", "bannerFill").value<QColor>() == QColor(0x1E, 0x1E, 0x1E));
    });
    CHECK(notified >= 2);
}

TEST_CASE("ChromeStyleHost republishes chromeStyle and keeps the old provider alive for the event",
          "[contour][chromehost][qml]")
{
    using contour::config::ChromeStyle;
    using contour::config::MotionPreference;
    using contour::window::ChromeStyleHost;

    // Declared before the engine so the host, and the provider the engine borrows, outlive it.
    auto host = std::optional<ChromeStyleHost>();
    auto engine = QQmlEngine();

    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());
    host.emplace(*engine.rootContext(), inputs);
    // After the first provider: its font metrics may log Qt's one-off font-alias notice, which says
    // nothing about the QML under test.
    auto const capture = contour::test::QmlMessageCapture();

    auto component = QQmlComponent(&engine);
    component.setData("import QtQuick\nItem { property int d: chromeStyle.motion.standard }\n", QUrl());
    auto const object = std::unique_ptr<QObject>(component.create());
    REQUIRE(object);
    CHECK(object->property("d").toInt() == 160);

    auto* const unchanged = &host->provider();
    host->update(inputs);
    CHECK(&host->provider() == unchanged);

    auto const first = QPointer<contour::window::UiStyleProvider>(&host->provider());
    inputs.accessibility.motion = MotionPreference::Reduced;
    host->update(inputs);
    CHECK(object->property("d").toInt() == 100);
    CHECK(host->inputs() == inputs);
    CHECK(&host->provider() != first.data());
    CHECK(!first.isNull());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(first.isNull());
    INFO(capture.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(capture.messages().isEmpty());
}

namespace
{

/// @p top laid over the opaque @p bottom, as the compositor paints it.
[[nodiscard]] QColor over(QColor const& top, QColor const& bottom)
{
    auto const a = top.alphaF();
    auto const mix = [a](float t, float b) {
        return (t * a) + (b * (1.0F - a));
    };
    return QColor::fromRgbF(
        mix(top.redF(), bottom.redF()), mix(top.greenF(), bottom.greenF()), mix(top.blueF(), bottom.blueF()));
}

/// @return The WCAG contrast ratio of two opaque colours.
[[nodiscard]] double contrastOf(QColor const& a, QColor const& b)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(a), contour::platform::toRGBColor(b));
}

/// A model that reports nothing, for reading its tab colour palette.
class SilentModelEvents final: public vtworkspace::ModelEvents
{
  public:
    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override {}
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override {}
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override {}
    void tabTitleChanged(vtworkspace::TabId) override {}
    void tabColorChanged(vtworkspace::TabId) override {}
};

/// The window colours of the two chromes the kit's tables are drawn for.
constexpr auto LightChrome = QRgb { 0xF5F5F7 };
constexpr auto DarkChrome = QRgb { 0x1E1E1E };

} // namespace

TEST_CASE("UiStyleProvider resolves the tab and chip roles on a light and a dark chrome",
          "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::platform::atOpacity;
    auto const mac = contour::window::UiStyleProvider(
        contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font()));
    auto const role = [&mac](char const* name) {
        return tokenOf(mac, "colors", name).value<QColor>();
    };

    withWindowColor(QColor(LightChrome), [&] {
        CHECK(role("tabWellFill") == atOpacity(QColor(Qt::black), 5));
        CHECK(role("tabHoverFill") == atOpacity(QColor(Qt::black), 8));
        CHECK(role("controlPressedFill") == atOpacity(QColor(Qt::black), 12));
        CHECK(role("tabTextActive") == atOpacity(QColor(Qt::black), 88));
        CHECK(role("tabText") == atOpacity(QColor(Qt::black), 60));
        CHECK(role("tabTextDimmed") == atOpacity(QColor(Qt::black), 45));
        CHECK(role("tabActiveFill") == QColor(0xFFFFFF));
        CHECK(role("chipFill") == atOpacity(QColor(Qt::black), 5));
        CHECK(role("chipTextStrong") == atOpacity(QColor(Qt::black), 88));
    });
    withWindowColor(QColor(DarkChrome), [&] {
        CHECK(role("tabWellFill") == atOpacity(QColor(Qt::white), 6));
        CHECK(role("tabTextActive") == atOpacity(QColor(Qt::white), 92));
        CHECK(role("tabText") == atOpacity(QColor(Qt::white), 62));
        CHECK(role("tabTextDimmed") == atOpacity(QColor(Qt::white), 50));
        CHECK(role("tabActiveFill") == QColor(0x2C2C2E));
    });
}

TEST_CASE("UiStyleProvider: the tab text reads at 4.5:1 on every fill it sits on", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    auto const mac = contour::window::UiStyleProvider(
        contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font()));
    auto const role = [&mac](char const* name) {
        return tokenOf(mac, "colors", name).value<QColor>();
    };
    auto const tint = tokenOf(mac, "tint", "tab").toReal();
    CHECK(tint == Catch::Approx(0.20));

    auto events = SilentModelEvents {};
    auto nextSession = uint64_t { 1 };
    auto const model =
        vtworkspace::SessionModel { events, [&] { return vtworkspace::SessionId { nextSession++ }; } };
    REQUIRE(!model.colorPalette().empty());

    for (auto const window: { QColor(LightChrome), QColor(DarkChrome) })
        withWindowColor(window, [&] {
            INFO("chrome: " << window.name().toStdString());
            auto const well = over(role("tabWellFill"), window);
            auto const active = over(role("tabActiveFill"), window);
            auto const chip = over(role("chipFill"), window);
            CHECK(contrastOf(over(role("tabTextActive"), active), active) >= 4.5);
            CHECK(contrastOf(over(role("tabText"), well), well) >= 4.5);
            CHECK(contrastOf(over(role("chipText"), chip), chip) >= 4.5);
            // The dimmed label is the inactive window's: WCAG 1.4.3 exempts an inactive component's
            // text, so it is held to the 3:1 a non-text mark (the uncoloured tab icon) needs.
            CHECK(contrastOf(over(role("tabTextDimmed"), well), well) >= 3.0);

            for (auto const& swatch: model.colorPalette())
            {
                auto tinted = contour::platform::toQColor(swatch);
                tinted.setAlphaF(static_cast<float>(tint));
                auto const activePill = over(tinted, active);
                auto const inactivePill = over(tinted, well);
                INFO("swatch: " << contour::platform::toQColor(swatch).name().toStdString());
                CHECK(contrastOf(over(role("tabTextActive"), activePill), activePill) >= 4.5);
                // A coloured inactive tab is still active UI: its label is the tinted one.
                CHECK(contrastOf(over(role("tabTextTinted"), inactivePill), inactivePill) >= 4.5);
            }
        });
}

namespace
{
/// @return The hue of @p color in degrees.
[[nodiscard]] double hueOf(QColor const& color)
{
    return color.hsvHueF() * 360.0;
}

/// @return How far apart two hues are on the colour wheel, in degrees.
[[nodiscard]] double hueDistance(double a, double b)
{
    auto const apart = std::fmod(std::abs(a - b), 360.0);
    return std::min(apart, 360.0 - apart);
}
} // namespace

TEST_CASE("UiStyleProvider: the native rows' status marks read as their meaning", "[contour][uistyle]")
{
    // A mark -- a badge's dot, ring, disc or glyph, the host chip's dot -- is drawn in the status hue's mark
    // ink, not in the ink its text is darkened to: on a light chrome the text Warning is darkened so far
    // towards red that a Blocked disc read as a failure. A mark is a non-text graphic, so it is held to
    // WCAG 1.4.11's 3:1 against every surface it sits on.
    using contour::config::ChromeStyle;
    using contour::config::StatusColorRole;
    using contour::window::TabBadgeKind;
    using vtbackend::operator""_rgb;
    constexpr auto MarkKinds = std::array {
        TabBadgeKind::Output,  TabBadgeKind::Error,   TabBadgeKind::Done,
        TabBadgeKind::Blocked, TabBadgeKind::Working,
    };
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        auto inputs = contour::window::chromeInputsFor(row, QGuiApplication::font());
        inputs.appearance.accent = 0x007AFF_rgb; // Running is the system accent: measured at the kit's
        auto const provider = contour::window::UiStyleProvider(inputs);
        auto const role = [&provider](char const* name) {
            return tokenOf(provider, "colors", name).value<QColor>();
        };
        for (auto const window: { QColor(LightChrome), QColor(DarkChrome) })
            withWindowColor(window, [&] {
                INFO("row " << int(row) << ", chrome " << window.name().toStdString());
                auto const warning = provider.statusInk(static_cast<int>(StatusColorRole::Warning));
                auto const danger = provider.statusInk(static_cast<int>(StatusColorRole::Danger));
                // The kit's own pair (#FF9500, #D70015) is 41 degrees apart; the text pair, 21.
                CHECK(hueDistance(hueOf(warning), hueOf(danger)) >= 30.0);

                auto const surfaces = std::array {
                    window,
                    over(role("tabWellFill"), window),
                    over(role("tabActiveFill"), window),
                    over(role("chipFill"), window),
                };
                auto marks = std::vector<std::pair<std::string, QColor>> {};
                for (auto const kind: MarkKinds)
                {
                    auto const look = provider.badgeLook(static_cast<int>(kind));
                    marks.emplace_back(look.value("name").toString().toStdString(),
                                       look.value("ink").value<QColor>());
                }
                for (auto const state:
                     std::views::iota(std::size_t { 0 }, contour::window::HostChipStateCount))
                    marks.emplace_back(
                        "host dot " + std::to_string(state),
                        provider.hostChipLook(static_cast<int>(state)).value("dot").value<QColor>());
                for (auto const& [name, ink]: marks)
                    for (auto const& surface: surfaces)
                    {
                        INFO(name << " on " << surface.name().toStdString());
                        CHECK(contrastOf(over(ink, surface), surface) >= 3.0);
                    }
            });
    }
}

TEST_CASE("UiStyleProvider: progressColor is the accent while running", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::StatusColorRole;
    using vtbackend::operator""_rgb;
    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());
    inputs.appearance.accent = 0x007AFF_rgb;
    auto const provider = contour::window::UiStyleProvider(inputs);
    auto const danger = static_cast<int>(StatusColorRole::Danger);

    for (auto const window: { QColor(LightChrome), QColor(DarkChrome) })
        withWindowColor(window, [&] {
            CHECK(provider.progressColor(1) == QColor(0x007AFF));
            CHECK(provider.progressColor(3) == QColor(0x007AFF));
            // A progress bar is a mark: the mark ink, which for Danger is its text ink too.
            CHECK(provider.progressColor(2) == provider.statusColor(danger));
            CHECK(provider.progressColor(4)
                  == provider.statusColor(StatusColorRole::Warning, contour::config::StatusInkUse::Mark));
            CHECK(provider.progressColor(0).alpha() == 0);
            CHECK(provider.progressColor(5).alpha() == 0);
            CHECK(provider.progressColor(-1).alpha() == 0);
            // "This failed" is the status token, on any surface the error text sits on.
            CHECK(provider.errorColor() == provider.statusColor(danger));
        });
    CHECK(provider.statusInk(-1).alpha() == 0);
    CHECK(provider.statusInk(static_cast<int>(contour::config::StatusColorRoleCount)).alpha() == 0);
}

TEST_CASE("UiStyleProvider: the Terminal row's inks stay legible on its bar", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::SchemeSlot;
    using contour::config::StatusColorRole;
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes("contour-light", palette));
    auto inputs = contour::window::chromeInputsFor(ChromeStyle::Terminal, QGuiApplication::font());
    inputs.scheme = contour::config::chromeSchemeColorsOf(palette);
    auto const terminal = contour::window::UiStyleProvider(inputs);

    auto const slot = [&](SchemeSlot value) {
        return contour::platform::toQColor(inputs.scheme.bySlot[static_cast<size_t>(value)]);
    };
    // Blue on contour-light's dark bar fails 4.5:1, so Running falls back to the bar's ink.
    CHECK(terminal.statusInk(static_cast<int>(StatusColorRole::Running)) == slot(SchemeSlot::BarInk));
    CHECK(terminal.progressColor(1) == slot(SchemeSlot::BarInk));
    for (auto const role: std::views::iota(0, static_cast<int>(contour::config::StatusColorRoleCount)))
    {
        CAPTURE(role);
        CHECK(contrastOf(terminal.statusInk(role), slot(SchemeSlot::Bar)) >= 4.5);
    }
    auto const role = [&terminal](char const* name) {
        return tokenOf(terminal, "colors", name).value<QColor>();
    };
    CHECK(role("tabActiveFill") == slot(SchemeSlot::Selection));
    CHECK(role("tabTextActive") == slot(SchemeSlot::SelectionText));
    CHECK(role("tabText") == slot(SchemeSlot::BarInk));
    CHECK(contrastOf(role("tabTextActive"), role("tabActiveFill")) >= 4.5);
    CHECK(contrastOf(role("tabText"), role("tabWellFill")) >= 4.5);
}

TEST_CASE("UiStyleProvider: badgeLook, hostChipLook, titleBand and tabWellLayout answer the pure tables",
          "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();
    auto const mac = UiStyleProvider(chromeInputsFor(ChromeStyle::MacOS, font));

    auto const blocked = mac.badgeLook(static_cast<int>(contour::window::TabBadgeKind::Blocked));
    CHECK(blocked.value("mark").toString() == QStringLiteral("disc"));
    CHECK(blocked.value("glyph").toString() == QStringLiteral("!"));
    CHECK(blocked.value("name").toString() == QStringLiteral("Waiting for you"));
    CHECK(blocked.value("ink").value<QColor>()
          == mac.statusInk(static_cast<int>(contour::config::StatusColorRole::Warning)));
    CHECK(mac.badgeLook(-1).isEmpty());
    CHECK(mac.badgeLook(static_cast<int>(contour::window::TabBadgeKindCount)).isEmpty());

    auto const reconnecting =
        mac.hostChipLook(static_cast<int>(contour::window::HostChipState::Reconnecting));
    CHECK(reconnecting.value("status").toString() == QStringLiteral("Reconnecting…"));
    CHECK(reconnecting.value("accessibleState").toString() == QStringLiteral("reconnecting"));
    CHECK(reconnecting.value("dot").value<QColor>()
          == mac.statusInk(static_cast<int>(contour::config::StatusColorRole::Warning)));
    CHECK(mac.hostChipLook(99).isEmpty());

    // The chip shows for a window with a host to name (window::hostChipPresence).
    CHECK(mac.isHostChipShown(QStringLiteral("home")));
    CHECK_FALSE(mac.isHostChipShown({}));

    auto const macBand = mac.titleBand(0, 0, 0, 0, {}, 0);
    CHECK(macBand.value("band").toString() == QStringLiteral("shown"));
    CHECK(macBand.value("bandEdge").toString() == QStringLiteral("top"));
    CHECK(macBand.value("centre").toString() == QStringLiteral("title"));
    CHECK(macBand.value("tabBar").toString() == QStringLiteral("hidden"));
    auto const fluent = UiStyleProvider(chromeInputsFor(ChromeStyle::Fluent, font));
    CHECK(fluent.titleBand(0, 0, 0, 0, {}, 0).value("band").toString() == QStringLiteral("hidden"));
    CHECK(fluent.titleBand(0, 0, 1, 0, {}, 0).value("centre").toString() == QStringLiteral("tabs"));
    CHECK(mac.titleBand(0, 1, 1, 0, {}, 0).value("band").toString() == QStringLiteral("reveal"));
    CHECK(mac.titleBand(2, 0, 0, 0, {}, 0).isEmpty());
    CHECK(mac.titleBand(0, 0, 0, -1, {}, 0).isEmpty());
    // A remote window whose connection is not up shows the strip whatever the gate says; a local one, or a
    // connected one, does not.
    constexpr auto Reconnecting = static_cast<int>(contour::window::HostChipState::Reconnecting);
    constexpr auto Disconnected = static_cast<int>(contour::window::HostChipState::Disconnected);
    CHECK(fluent.titleBand(0, 0, 0, 0, QStringLiteral("home"), Reconnecting).value("band").toString()
          == QStringLiteral("shown"));
    CHECK(fluent.titleBand(0, 0, 0, 1, QStringLiteral("home"), Disconnected).value("bandEdge").toString()
          == QStringLiteral("bottom"));
    CHECK(fluent.titleBand(0, 0, 0, 0, QStringLiteral("home"), 0).value("band").toString()
          == QStringLiteral("hidden"));
    CHECK(fluent.titleBand(0, 0, 0, 0, {}, Disconnected).value("band").toString()
          == QStringLiteral("hidden"));
    CHECK(fluent.titleBand(0, 0, 0, 0, {}, 3).isEmpty());

    auto const crowded = mac.tabWellLayout(300, 12);
    CHECK(crowded.value("scrolls").toBool());
    CHECK(crowded.value("label").toString() == QStringLiteral("iconOnly"));
    CHECK(crowded.value("tabWidth").toReal() == 34.0);
    auto const roomy = mac.tabWellLayout(1000, 3);
    CHECK(!roomy.value("scrolls").toBool());
    CHECK(roomy.value("tabWidth").toReal() == 240.0);
    CHECK(roomy.value("wellWidth").toReal() == 728.0);
    CHECK(roomy.value("label").toString() == QStringLiteral("full"));
}

TEST_CASE("UiStyleProvider: snapToCells rounds up to whole cells on the Terminal row", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();

    auto const terminal = UiStyleProvider(chromeInputsFor(ChromeStyle::Terminal, font));
    auto const cell = terminal.widthQuantum();
    REQUIRE(cell > 1.0);
    CHECK(terminal.snapToCells(8.75 * cell) == Catch::Approx(9 * cell));
    CHECK(terminal.snapToCells(9 * cell) == Catch::Approx(9 * cell));
    CHECK(terminal.snapToCells(0.0) == 0.0);

    auto const mac = UiStyleProvider(chromeInputsFor(ChromeStyle::MacOS, font));
    CHECK(mac.snapToCells(70) == 70.0);
}

TEST_CASE("UiStyleProvider: the active tab's font is bold only where the row sets it apart by weight",
          "[contour][uistyle]")
{
    using contour::config::ActiveTabWeight;
    using contour::config::ChromeStyle;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto base = QGuiApplication::font();
    base.setBold(false);
    for (auto const index: std::views::iota(std::size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        auto const row = static_cast<ChromeStyle>(index);
        CAPTURE(index);
        auto const provider = UiStyleProvider(chromeInputsFor(row, base));
        auto const font = provider.activeTabFont(base);
        auto const bold =
            contour::config::ChromeTokenRows[index].layout.activeTabWeight == ActiveTabWeight::Bold;
        CHECK(font.bold() == bold);
        CHECK(font.family() == base.family());
        CHECK(font.pointSizeF() == base.pointSizeF());
    }
}

TEST_CASE("UiStyleProvider: the shape and motion flags QML reads follow the tokens", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();
    for (auto const index: std::views::iota(std::size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        CAPTURE(index);
        auto provider = UiStyleProvider(chromeInputsFor(static_cast<ChromeStyle>(index), font));
        auto const* layout = provider.layout();
        auto const* motion = provider.motion();
        REQUIRE(layout != nullptr);
        REQUIRE(motion != nullptr);
        CHECK(
            layout->property("isPill").toBool()
            == (contour::config::ChromeTokenRows[index].layout.tabShape == contour::config::TabShape::Pill));
        CHECK(motion->property("animates").toBool()); // no row reduces motion of its own accord
    }

    auto inputs = chromeInputsFor(ChromeStyle::MacOS, font);
    inputs.accessibility.motion = contour::config::MotionPreference::Reduced;
    auto reduced = UiStyleProvider(inputs);
    CHECK_FALSE(reduced.motion()->property("animates").toBool());
}

TEST_CASE("UiStyleProvider publishes the band's tokens", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::MotionPreference;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();

    auto const mac = UiStyleProvider(chromeInputsFor(ChromeStyle::MacOS, font));
    CHECK(tokenOf(mac, "size", "tabIcon").toReal() == 14.0);
    CHECK(tokenOf(mac, "size", "toolbarButton").toReal() == 28.0);
    CHECK(tokenOf(mac, "size", "hostDot").toReal() == 7.0);
    CHECK(tokenOf(mac, "space", "dragReserve").toReal() == 40.0);
    CHECK(tokenOf(mac, "space", "revealEdge").toReal() == 2.0);
    CHECK(tokenOf(mac, "space", "toolbarChipPadding").toReal() == 9.0);
    CHECK(tokenOf(mac, "radii", "tab").toReal() == 12.0);
    CHECK(tokenOf(mac, "radii", "tabWell").toReal() == 14.0);
    CHECK(tokenOf(mac, "radii", "toolbarChip").toReal() == 11.0);
    CHECK(tokenOf(mac, "motion", "spinner").toInt() == 1200);
    CHECK(tokenOf(mac, "motion", "pulse").toInt() == 700);
    CHECK(tokenOf(mac, "motion", "style").toInt() == tokenOf(mac, "motion", "full").toInt());
    CHECK(tokenOf(mac, "motion", "full").toInt() != tokenOf(mac, "motion", "crossFade").toInt());
    CHECK(tokenOf(mac, "layout", "tabShape").toInt() == tokenOf(mac, "layout", "pill").toInt());
    CHECK(tokenOf(mac, "layout", "flat").toInt() != tokenOf(mac, "layout", "pill").toInt());
    CHECK(tokenOf(mac, "layout", "profileMenu").toInt() == tokenOf(mac, "layout", "secondaryClick").toInt());
    CHECK(tokenOf(mac, "layout", "caret").toInt() != tokenOf(mac, "layout", "secondaryClick").toInt());
    // The Pill band is the title bar's own height; a Flat bar keeps today's strip height.
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        CAPTURE(static_cast<int>(row));
        auto const provider = UiStyleProvider(chromeInputsFor(row, font));
        auto const expected = row == ChromeStyle::MacOS ? 38.0 : 34.0;
        CHECK(provider.property("bandHeight").toReal() == expected);
        // A Flat tab's corners are the 1a corner radius (3), not radii.tab (the pill's, 6 or 8 on
        // Fluent and Linux): binding the Flat strip to the pill's radius would move today's geometry.
        CHECK(provider.radius() == 3);
        CHECK(provider.radius() == contour::config::uiStyleTokens(row).cornerRadiusPixels);
    }
    CHECK(tokenOf(UiStyleProvider(chromeInputsFor(ChromeStyle::Fluent, font)), "radii", "tab").toReal()
          != 3.0);
    CHECK(mac.property("tabColorGlyph").toString().isEmpty());
    CHECK(mac.property("hostDotGlyph").toString().isEmpty());

    auto const fluent = UiStyleProvider(chromeInputsFor(ChromeStyle::Fluent, font));
    CHECK(fluent.property("bandHeight").toReal() == fluent.chromeHeight());
    CHECK(fluent.chromeHeight() == 34.0);
    CHECK(tokenOf(fluent, "layout", "tabShape").toInt() == tokenOf(fluent, "layout", "flat").toInt());
    CHECK(tokenOf(fluent, "layout", "profileMenu").toInt() == tokenOf(fluent, "layout", "caret").toInt());

    auto inputs = chromeInputsFor(ChromeStyle::MacOS, font);
    inputs.accessibility.motion = MotionPreference::Reduced;
    auto const reduced = UiStyleProvider(inputs);
    CHECK(tokenOf(reduced, "motion", "style").toInt() == tokenOf(reduced, "motion", "crossFade").toInt());
    CHECK(tokenOf(reduced, "motion", "spinner").toInt() == 1200);

    auto const terminal = UiStyleProvider(chromeInputsFor(ChromeStyle::Terminal, font));
    CHECK(tokenOf(terminal, "size", "tabIcon").toReal() == Catch::Approx(terminal.widthQuantum()));
    CHECK(tokenOf(terminal, "size", "hostChip").toReal() == Catch::Approx(terminal.cellHeight()));
    CHECK(tokenOf(terminal, "space", "dragReserve").toReal() == Catch::Approx(4 * terminal.widthQuantum()));
    CHECK(tokenOf(terminal, "tint", "tab").toReal() == 0.0);
    CHECK(terminal.property("bandHeight").toReal() == terminal.chromeHeight());
    CHECK(terminal.property("tabColorGlyph").toString() == QStringLiteral("▌"));
    CHECK(terminal.property("chipOpenGlyph").toString() == QStringLiteral("["));
    CHECK(terminal.property("chipCloseGlyph").toString() == QStringLiteral("]"));
    CHECK(terminal.property("hostDotGlyph").toString() == QStringLiteral("●"));
}

// {{{ Panes (Step 2)

namespace
{

/// @return @p provider's resolution of pane colour role @p role on @p background.
[[nodiscard]] QColor paneColorOn(contour::window::UiStyleProvider const& provider,
                                 contour::config::ChromeColorRole role,
                                 QColor const& background)
{
    return provider.colorOn(static_cast<int>(role), background);
}

/// The inputs of @p row in @p density, nothing else set.
[[nodiscard]] contour::window::ChromeInputs densityInputs(contour::config::ChromeStyle row,
                                                          contour::config::ChromeDensity density)
{
    auto inputs = contour::window::chromeInputsFor(row, QGuiApplication::font());
    inputs.density = density;
    return inputs;
}

} // namespace

TEST_CASE("UiStyleProvider takes the density from its inputs", "[contour][uistyle]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    using contour::window::UiStyleProvider;

    auto const minimalInputs = densityInputs(ChromeStyle::MacOS, ChromeDensity::Minimal);
    auto const comfortableInputs = densityInputs(ChromeStyle::MacOS, ChromeDensity::Comfortable);
    CHECK(minimalInputs != comfortableInputs);
    CHECK(contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font()).density
          == ChromeDensity::Comfortable);

    auto const minimal = UiStyleProvider(minimalInputs);
    CHECK(minimal.property("density").toString() == QStringLiteral("minimal"));
    CHECK(tokenOf(minimal, "size", "paneHeader").toReal() == 0.0);
    CHECK(tokenOf(minimal, "space", "paneGap").toReal() == 1.0);
    CHECK(tokenOf(minimal, "space", "splitHit").toReal() == 8.0);
    CHECK(tokenOf(minimal, "radii", "paneCard").toReal() == 0.0);
    // The held modifier's overlay takes no room, so Minimal still draws it at the row's header height.
    CHECK(minimal.property("overlayPaneHeader").toReal() == 22.0);

    auto const comfortable = UiStyleProvider(comfortableInputs);
    CHECK(comfortable.property("density").toString() == QStringLiteral("comfortable"));
    CHECK(tokenOf(comfortable, "size", "paneHeader").toReal() == 22.0);
    CHECK(comfortable.overlayPaneHeader() == 22.0);
}

TEST_CASE("UiStyleProvider publishes the pane tokens", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::PaneHeaderAction;
    using contour::window::chromeInputsFor;
    using contour::window::UiStyleProvider;
    auto const font = QGuiApplication::font();

    auto const mac = UiStyleProvider(chromeInputsFor(ChromeStyle::MacOS, font));
    CHECK(tokenOf(mac, "size", "bareBand").toReal() == 32.0);
    CHECK(tokenOf(mac, "size", "grabber").toReal() == 2.0);
    CHECK(tokenOf(mac, "size", "grabberLength").toReal() == 32.0);
    CHECK(tokenOf(mac, "size", "paneHeaderAction").toReal() == 22.0);
    CHECK(tokenOf(mac, "size", "paneFooter").toReal() == 34.0);
    CHECK(tokenOf(mac, "size", "footerButton").toReal() == 24.0);
    CHECK(tokenOf(mac, "space", "paneGapRows").toReal() == 6.0);
    CHECK(tokenOf(mac, "space", "paneHeaderGap").toReal() == 6.0);
    CHECK(tokenOf(mac, "space", "footerLeading").toReal() == 14.0);
    CHECK(tokenOf(mac, "radii", "grabber").toReal() == 1.0);
    CHECK(tokenOf(mac, "type", "paneHeader").value<QFont>().pixelSize() == 11);
    CHECK(tokenOf(mac, "type", "paneHeader").value<QFont>().weight() == QFont::Medium);
    CHECK(tokenOf(mac, "type", "paneHeaderWeight").toInt() == 500);
    CHECK(tokenOf(mac, "motion", "headerReveal").toInt() == 500);
    CHECK(tokenOf(mac, "layout", "paneActions").toInt() == tokenOf(mac, "layout", "onHover").toInt());
    CHECK(tokenOf(mac, "layout", "onHover").toInt() != tokenOf(mac, "layout", "onHoverOrFocus").toInt());

    auto const glyphs = mac.property("paneActionGlyphs").toStringList();
    auto const glyphCount = static_cast<size_t>(glyphs.size());
    REQUIRE(glyphCount == contour::config::PaneHeaderActionCount);
    CHECK(glyphs[static_cast<qsizetype>(PaneHeaderAction::Zoom)] == QStringLiteral("⤢"));
    CHECK(mac.property("splitLineGlyphs").toStringList() == QStringList { QString(), QString() });
    CHECK(mac.property("restartGlyph").toString() == QStringLiteral("↩"));

    // The accessible names come from the header actions' own table, in its order.
    auto const names = mac.property("paneHeaderActionNames").toStringList();
    auto const nameCount = static_cast<size_t>(names.size());
    REQUIRE(nameCount == contour::window::PaneHeaderActions.size());
    for (auto const& row: contour::window::PaneHeaderActions)
        CHECK(names[static_cast<qsizetype>(row.action)] == QString::fromUtf8(row.accessibleName));

    auto const terminal = UiStyleProvider(chromeInputsFor(ChromeStyle::Terminal, font));
    // A vertical gap is one cell's height on the Terminal row, its horizontal twin one cell's width.
    CHECK(tokenOf(terminal, "space", "paneGapRows").toReal() == Catch::Approx(terminal.cellHeight()));
    CHECK(tokenOf(terminal, "space", "paneGap").toReal() == Catch::Approx(terminal.widthQuantum()));
    CHECK(tokenOf(terminal, "size", "paneFooter").toReal() == Catch::Approx(terminal.cellHeight()));
    CHECK(tokenOf(terminal, "size", "grabber").toReal() == 0.0);
    CHECK(tokenOf(terminal, "layout", "paneActions").toInt()
          == tokenOf(terminal, "layout", "onHoverOrFocus").toInt());
    CHECK(terminal.property("paneActionGlyphs").toStringList()[static_cast<qsizetype>(PaneHeaderAction::Zoom)]
          == QStringLiteral("Z"));
    CHECK(terminal.property("splitLineGlyphs").toStringList()
          == QStringList { QStringLiteral("│"), QStringLiteral("─") });
}

TEST_CASE("chromeStyle names the pane colour roles and the badge kinds in QML", "[contour][uistyle][qml]")
{
    using contour::config::ChromeColorRole;
    auto engine = QQmlEngine();
    contour::test::installChromeStyle(engine);
    auto const evaluate = [&engine](char const* expression) {
        auto qmlExpression = QQmlExpression(engine.rootContext(), nullptr, QString::fromLatin1(expression));
        auto const value = qmlExpression.evaluate();
        REQUIRE(!qmlExpression.hasError());
        return value;
    };

    CHECK(evaluate("chromeStyle.roles.paneGapFill").toInt()
          == static_cast<int>(ChromeColorRole::PaneGapFill));
    CHECK(evaluate("chromeStyle.roles.paneHeaderText").toInt()
          == static_cast<int>(ChromeColorRole::PaneHeaderText));
    CHECK(evaluate("chromeStyle.roles.paneFooterText").toInt()
          == static_cast<int>(ChromeColorRole::PaneFooterText));
    CHECK(evaluate("chromeStyle.badgeKinds.working").toInt()
          == static_cast<int>(contour::window::TabBadgeKind::Working));

    // One name per pane role and per badge kind, so a new enumerator needs a QML name too.
    auto const propertiesOf = [](QMetaObject const& meta) {
        return static_cast<size_t>(meta.propertyCount() - meta.propertyOffset());
    };
    CHECK(propertiesOf(contour::window::ChromePaneRoleNames::staticMetaObject)
          == contour::config::ChromeColorRoleCount - static_cast<size_t>(ChromeColorRole::PaneGapFill));
    CHECK(propertiesOf(contour::window::ChromeBadgeKindNames::staticMetaObject)
          == contour::window::TabBadgeKindCount);
}

TEST_CASE("UiStyleProvider: statusMarkOn inks a mark for the ground it sits on", "[contour][uistyle]")
{
    using contour::config::StatusColorRole;
    using contour::config::StatusInkUse;
    auto const style = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    auto const warning = static_cast<int>(StatusColorRole::Warning);
    auto const& hues = contour::config::StatusColors[static_cast<size_t>(warning)].hues(StatusInkUse::Mark);

    // The tone of the ground picks the hue, not the palette's: a dark pill on a light chrome takes the
    // dark-ground hue.
    auto const black = QColor(Qt::black);
    CHECK(style.statusMarkOn(warning, black) == QColor::fromRgb(hues.dark));
    // Every answer reads at the mark ratio on its ground, falling back to black or white where the hue
    // cannot.
    for (auto const& ground:
         { QColor(Qt::black), QColor(Qt::white), QColor(0xD9, 0xDC, 0xE3), QColor(0x80, 0x80, 0x80) })
    {
        INFO("ground " << ground.name().toStdString());
        CHECK(vtbackend::contrastRatio(contour::platform::toRGBColor(style.statusMarkOn(warning, ground)),
                                       contour::platform::toRGBColor(ground))
              >= contour::config::MinimumMarkContrast);
    }
    CHECK(style.statusMarkOn(static_cast<int>(StatusColorRole::Running), black) == style.accentColor());
    CHECK(style.statusMarkOn(-1, black) == QColor(Qt::transparent));
}

TEST_CASE("chromeStyle names an ended pane's ends and restart offers in QML", "[contour][uistyle][qml]")
{
    using contour::session::EndReason;
    using contour::session::FooterRestart;
    auto engine = QQmlEngine();
    contour::test::installChromeStyle(engine);
    auto const evaluate = [&engine](char const* expression) {
        auto qmlExpression = QQmlExpression(engine.rootContext(), nullptr, QString::fromLatin1(expression));
        auto const value = qmlExpression.evaluate();
        REQUIRE(!qmlExpression.hasError());
        return value;
    };

    CHECK(evaluate("chromeStyle.paneEnds.none").toInt() == static_cast<int>(EndReason::None));
    CHECK(evaluate("chromeStyle.paneEnds.exited").toInt() == static_cast<int>(EndReason::Exited));
    CHECK(evaluate("chromeStyle.paneEnds.signalled").toInt() == static_cast<int>(EndReason::Signalled));
    CHECK(evaluate("chromeStyle.paneEnds.startFailed").toInt() == static_cast<int>(EndReason::StartFailed));
    CHECK(evaluate("chromeStyle.paneEnds.disconnected").toInt() == static_cast<int>(EndReason::Disconnected));
    CHECK(evaluate("chromeStyle.paneEnds.restartUnavailable").toInt()
          == static_cast<int>(FooterRestart::Unavailable));
    CHECK(evaluate("chromeStyle.paneEnds.restartOffered").toInt()
          == static_cast<int>(FooterRestart::Offered));

    // One name per end and per offer, so a new enumerator needs a QML name too.
    auto const& meta = contour::window::ChromePaneEndNames::staticMetaObject;
    CHECK(static_cast<size_t>(meta.propertyCount() - meta.propertyOffset())
          == contour::session::PaneFooterRows.size() + contour::session::FooterRestartCount);
}

TEST_CASE("UiStyleProvider: paneHeader answers the pure policy", "[contour][uistyle]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    using contour::window::UiStyleProvider;

    auto const comfortable = UiStyleProvider(densityInputs(ChromeStyle::MacOS, ChromeDensity::Comfortable));
    auto const header = comfortable.paneHeader(0, 1, 1, 2, 400, 300);
    CHECK(header.value("content").toString() == QStringLiteral("nameAndCwd"));
    CHECK(header.value("actions").toString() == QStringLiteral("shown"));
    CHECK(header.value("placement").toString() == QStringLiteral("inline"));
    CHECK(header.value("emphasis").toString() == QStringLiteral("primary"));
    // A lone pane has no header, and a pane too short for the header and a row of cells neither.
    CHECK(comfortable.paneHeader(0, 1, 1, 1, 400, 300).value("content").toString()
          == QStringLiteral("hidden"));
    CHECK(comfortable.paneHeader(0, 1, 1, 2, 400, 22).value("content").toString()
          == QStringLiteral("hidden"));
    // Each enum's first value past its end, as QML could pass it, answers nothing.
    auto constexpr PastReveal = static_cast<int>(contour::window::HeaderRevealCount);
    auto constexpr PastPointer = static_cast<int>(contour::window::PointerPresenceCount);
    auto constexpr PastFocus = static_cast<int>(contour::window::PaneFocusCount);
    CHECK(comfortable.paneHeader(-1, 0, 0, 2, 400, 300).isEmpty());
    CHECK(comfortable.paneHeader(PastReveal, 0, 0, 2, 400, 300).isEmpty());
    CHECK(comfortable.paneHeader(0, PastPointer, 0, 2, 400, 300).isEmpty());
    CHECK(comfortable.paneHeader(0, 0, PastFocus, 2, 400, 300).isEmpty());
    CHECK(!comfortable.paneHeader(PastReveal - 1, PastPointer - 1, PastFocus - 1, 2, 400, 300).isEmpty());

    auto const minimal = UiStyleProvider(densityInputs(ChromeStyle::MacOS, ChromeDensity::Minimal));
    CHECK(minimal.paneHeader(1, 0, 0, 2, 400, 300).value("placement").toString()
          == QStringLiteral("overlay"));
    CHECK(minimal.paneHeader(0, 0, 0, 2, 400, 300).value("content").toString() == QStringLiteral("hidden"));

    // The Terminal row shows the actions on the focused pane too.
    auto const terminal = UiStyleProvider(densityInputs(ChromeStyle::Terminal, ChromeDensity::Comfortable));
    CHECK(terminal.paneHeader(0, 0, 1, 2, 400, 300).value("actions").toString() == QStringLiteral("shown"));
    CHECK(comfortable.paneHeader(0, 0, 1, 2, 400, 300).value("actions").toString()
          == QStringLiteral("hidden"));

    CHECK(comfortable.paneHeaderName(QStringLiteral("vim"), QStringLiteral("~/src"))
          == QStringLiteral("vim"));
    CHECK(comfortable.paneHeaderName(QString(), QStringLiteral("~/src")) == QStringLiteral("~/src"));
}

TEST_CASE("UiStyleProvider: colorOn picks the label by the pane's own background", "[contour][uistyle]")
{
    using contour::config::ChromeColorRole;
    using contour::config::ChromeStyle;
    using contour::platform::atOpacity;
    using contour::window::UiStyleProvider;
    auto const mac =
        UiStyleProvider(contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font()));

    withWindowColor(QColor(DarkChrome), [&] {
        CHECK(paneColorOn(mac, ChromeColorRole::PaneHeaderText, QColor(0xFAFAF8))
              == atOpacity(QColor(Qt::black), 88));
        CHECK(paneColorOn(mac, ChromeColorRole::PaneHeaderText, QColor(0x1C1E24))
              == atOpacity(QColor(Qt::white), 92));
        CHECK(paneColorOn(mac, ChromeColorRole::PaneCardBorder, QColor(0xFAFAF8))
              == atOpacity(QColor(Qt::black), 10));
        CHECK(paneColorOn(mac, ChromeColorRole::PaneCardBorder, QColor(0x1C1E24))
              == atOpacity(QColor(Qt::white), 10));
    });
    CHECK(mac.colorOn(-1, QColor(Qt::black)).alpha() == 0);
    CHECK(mac.colorOn(static_cast<int>(contour::config::ChromeColorRoleCount), QColor(Qt::black)).alpha()
          == 0);

    // The gap is the chrome scheme's background shaded: a SchemeShade ignores the pane's background.
    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());
    inputs.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Background)] =
        vtbackend::RGBColor(0x1C1E24);
    auto const shaded = UiStyleProvider(inputs);
    auto const gap = paneColorOn(shaded, ChromeColorRole::PaneGapFill, QColor(Qt::white));
    CHECK(gap
          == contour::platform::toQColor(contour::config::shadeLightness(vtbackend::RGBColor(0x1C1E24), -4)));
    CHECK(tokenOf(shaded, "colors", "paneGapFill").value<QColor>() == gap);
}

TEST_CASE("UiStyleProvider: the pane text reads at 4.5:1 on every row, appearance and bundled scheme",
          "[contour][uistyle]")
{
    using contour::config::ChromeColorRole;
    using contour::config::ChromeStyle;
    using contour::window::UiStyleProvider;

    for (auto const name: vtbackend::defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, palette));
        auto const background = contour::platform::toQColor(palette.defaultBackground);
        for (auto const index: std::views::iota(size_t { 0 }, contour::config::detail::ChromeStyleCount))
        {
            CAPTURE(index);
            auto inputs =
                contour::window::chromeInputsFor(static_cast<ChromeStyle>(index), QGuiApplication::font());
            inputs.scheme = contour::config::chromeSchemeColorsOf(palette);
            auto const provider = UiStyleProvider(inputs);
            auto const on = [&](ChromeColorRole role, QColor const& ground) {
                return over(paneColorOn(provider, role, background), ground);
            };
            for (auto const window: { QColor(LightChrome), QColor(DarkChrome) })
                withWindowColor(window, [&] {
                    INFO("chrome: " << window.name().toStdString());
                    auto const header = on(ChromeColorRole::PaneHeaderFill, background);
                    CHECK(contrastOf(on(ChromeColorRole::PaneHeaderText, header), header) >= 4.5);
                    CHECK(contrastOf(on(ChromeColorRole::PaneHeaderTextDimmed, header), header) >= 4.5);
                    auto const footer = on(ChromeColorRole::PaneFooterFill, background);
                    CHECK(contrastOf(on(ChromeColorRole::PaneFooterText, footer), footer) >= 4.5);
                });
        }
    }
}

TEST_CASE("UiStyleProvider: the pane dim is the kit's token", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::ContrastPreference;
    using contour::window::UiStyleProvider;
    auto const mac =
        UiStyleProvider(contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font()));

    withWindowColor(QColor(DarkChrome), [&] {
        CHECK(mac.paneDimColor().rgb() == QColor(Qt::black).rgb());
        CHECK(mac.paneDimColor().alphaF() == Catch::Approx(0.15).margin(0.005));
    });
    withWindowColor(QColor(LightChrome),
                    [&] { CHECK(mac.paneDimColor().alphaF() == Catch::Approx(0.10).margin(0.005)); });

    auto inputs = contour::window::chromeInputsFor(ChromeStyle::MacOS, QGuiApplication::font());
    inputs.accessibility.contrast = ContrastPreference::Increased;
    auto const increased = UiStyleProvider(inputs);
    for (auto const window: { QColor(LightChrome), QColor(DarkChrome) })
        withWindowColor(
            window, [&] { CHECK(increased.paneDimColor().alphaF() == Catch::Approx(0.05).margin(0.005)); });

    // The dim follows the palette, so it notifies with it.
    auto const& meta = UiStyleProvider::staticMetaObject;
    auto const property = meta.property(meta.indexOfProperty("paneDimColor"));
    REQUIRE(property.hasNotifySignal());
    CHECK(property.notifySignal().name() == QByteArrayLiteral("paletteRevisionChanged"));
    auto dark = QColor();
    auto light = QColor();
    withWindowColor(QColor(DarkChrome), [&] { dark = mac.property("paneDimColor").value<QColor>(); });
    withWindowColor(QColor(LightChrome), [&] { light = mac.property("paneDimColor").value<QColor>(); });
    CHECK(dark != light);
}

TEST_CASE("UiStyleProvider: unfocusedDim floors Minimal and caps Increased Contrast", "[contour][uistyle]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    using contour::config::ContrastPreference;
    using contour::window::UiStyleProvider;

    auto const comfortable = UiStyleProvider(densityInputs(ChromeStyle::MacOS, ChromeDensity::Comfortable));
    auto const minimal = UiStyleProvider(densityInputs(ChromeStyle::MacOS, ChromeDensity::Minimal));
    withWindowColor(QColor(DarkChrome), [&] {
        CHECK(comfortable.unfocusedDim(0) == 0.0);
        CHECK(comfortable.unfocusedDim(0.3) == Catch::Approx(0.3));
        CHECK(minimal.unfocusedDim(0) == Catch::Approx(0.15));
        CHECK(minimal.unfocusedDim(0.3) == Catch::Approx(0.3));
    });
    withWindowColor(QColor(LightChrome), [&] { CHECK(minimal.unfocusedDim(0) == Catch::Approx(0.10)); });

    for (auto const density: std::views::iota(size_t { 0 }, contour::config::ChromeDensityCount))
    {
        CAPTURE(density);
        auto inputs = densityInputs(ChromeStyle::MacOS, static_cast<ChromeDensity>(density));
        inputs.accessibility.contrast = ContrastPreference::Increased;
        auto const increased = UiStyleProvider(inputs);
        CHECK(increased.unfocusedDim(0.3) == Catch::Approx(0.05));
    }
}

TEST_CASE("UiStyleProvider: hairline is one device pixel", "[contour][uistyle]")
{
    auto const provider = contour::window::UiStyleProvider(rowOf(UiStyle::Native), QGuiApplication::font());
    CHECK(provider.hairline(2) == 0.5);
    CHECK(provider.hairline(1) == 1.0);
    CHECK(provider.hairline(0) == 1.0);
}

// }}}

namespace
{
/// The macOS row's inputs with @p background as the chrome scheme's background.
[[nodiscard]] contour::window::ChromeInputs bandInputs(contour::config::ChromeStyle row,
                                                       vtbackend::RGBColor background)
{
    auto inputs = contour::window::chromeInputsFor(row, QGuiApplication::font());
    inputs.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Background)] = background;
    return inputs;
}
} // namespace

TEST_CASE("bandFill follows the material, the mode and the tone", "[contour][uistyle]")
{
    using contour::config::ChromeStyle;
    using contour::config::TransparencyPreference;
    using contour::window::ChromeTone;
    using contour::window::legibleBandTint;
    using contour::window::UiStyleProvider;
    using contour::window::WindowFrameCount;
    using contour::window::WindowModeCount;
    auto const tabs = QStringLiteral("tabs");
    auto const paletteWindow = [] {
        return QGuiApplication::palette().color(QPalette::Window);
    };
    // The chrome's tone and the scheme's background agree in each pass (preflight F19), so the band paints
    // the tint; the scheme's background stands off the palette's window, so the tint is not the palette.
    struct TonePass
    {
        QRgb chrome = 0;
        vtbackend::RGBColor background {};
        ChromeTone tone = ChromeTone::Light;
    };
    for (auto const& pass: { TonePass { DarkChrome, vtbackend::RGBColor(0x1C1E24), ChromeTone::Dark },
                             TonePass { LightChrome, vtbackend::RGBColor(0xFAFAF8), ChromeTone::Light } })
    {
        CAPTURE(QColor(pass.chrome).name().toStdString());
        auto const tint = contour::platform::toQColor(*legibleBandTint(pass.background, pass.tone));
        withWindowColor(QColor(pass.chrome), [&] {
            auto const mac = UiStyleProvider(bandInputs(ChromeStyle::MacOS, pass.background));
            CHECK(mac.bandFill(0, 0, tabs) == tint);
            CHECK(mac.bandFill(0, 0, tabs) != paletteWindow());
            CHECK(mac.bandFill(0, 0, QStringLiteral("title")) == tint);
            CHECK(mac.bandFill(0, 0, QStringLiteral("empty")) == tint);

            // Reduce Transparency (Review Focus 2): the same opaque tint.
            auto reducedInputs = bandInputs(ChromeStyle::MacOS, pass.background);
            reducedInputs.accessibility.transparency = TransparencyPreference::Reduced;
            auto const reduced = UiStyleProvider(reducedInputs);
            CHECK(reduced.bandFill(0, 0, tabs) == tint);
            CHECK(reduced.bandFill(0, 0, tabs).alpha() == 255);

            // Full screen (Review Focus 5): opaque, never cleared behind terminal text.
            CHECK(mac.bandFill(0, 1, tabs) == tint);
            CHECK(mac.bandFill(0, 1, tabs).alpha() == 255);

            // A native frame has no band of ours: the palette, as today.
            CHECK(mac.bandFill(1, 0, tabs) == paletteWindow());

            // Linux and Windows unchanged: every frame and mode paints the palette window colour.
            for (auto const row: { ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
            {
                CAPTURE(contour::config::chromeStyleName(row));
                auto const provider = UiStyleProvider(bandInputs(row, pass.background));
                for (auto const frame: std::views::iota(0, static_cast<int>(WindowFrameCount)))
                    for (auto const mode: std::views::iota(0, static_cast<int>(WindowModeCount)))
                        CHECK(provider.bandFill(frame, mode, tabs) == paletteWindow());
            }

            // Out of range, or a centre titleBand() never names: the palette window colour.
            CHECK(mac.bandFill(7, 9, tabs) == paletteWindow());
            CHECK(mac.bandFill(-1, 0, tabs) == paletteWindow());
            CHECK(mac.bandFill(0, 0, QStringLiteral("bogus")) == paletteWindow());
        });
    }

    // A dark scheme under a light chrome (Review Focus 3): the tint would carry the chrome's dark ink on a
    // dark ground, so the band keeps the palette window colour.
    withWindowColor(QColor(LightChrome), [&] {
        auto const mac = UiStyleProvider(bandInputs(ChromeStyle::MacOS, vtbackend::RGBColor(0x1E1E1E)));
        CHECK(mac.bandFill(0, 0, tabs) == QColor(LightChrome));
        CHECK(mac.bandFill(0, 1, tabs) == QColor(LightChrome));
    });
}

TEST_CASE("bandFill keeps Minimal's bare band legible on every bundled scheme", "[contour][uistyle]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    using contour::window::ChromeTone;
    using contour::window::UiStyleProvider;
    auto const empty = QStringLiteral("empty");
    // The band's ink is the chrome's (the host chip's text, the drift chip's): 92 % white on a dark chrome,
    // 88 % black on a light one, as 1c's label(1).
    auto const inkOver = [](vtbackend::RGBColor ground, ChromeTone tone) {
        auto const dark = tone == ChromeTone::Dark;
        auto const alpha = dark ? 0.92 : 0.88;
        auto const ink = dark ? 255.0 : 0.0;
        auto const mix = [&](uint8_t channel) {
            return static_cast<uint8_t>(std::lround((ink * alpha) + (channel * (1 - alpha))));
        };
        return vtbackend::RGBColor { mix(ground.red), mix(ground.green), mix(ground.blue) };
    };
    auto mismatched = 0;
    for (auto const name: vtbackend::defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = vtbackend::ColorPalette {};
        REQUIRE(vtbackend::defaultColorPalettes(name, palette));
        auto inputs = bandInputs(ChromeStyle::MacOS, palette.defaultBackground);
        inputs.density = ChromeDensity::Minimal;
        auto const provider = UiStyleProvider(inputs);
        for (auto const& [chrome, tone]:
             { std::pair { DarkChrome, ChromeTone::Dark }, std::pair { LightChrome, ChromeTone::Light } })
        {
            CAPTURE(QColor(chrome).name().toStdString());
            withWindowColor(QColor(chrome), [&] {
                auto const fill = provider.bandFill(0, 0, empty);
                auto const sameTone = contour::window::toneOf(palette.defaultBackground) == tone;
                // The terminal background where it has the chrome's tone; else the palette window colour,
                // as the tint falls back.
                CHECK(
                    fill
                    == (sameTone ? contour::platform::toQColor(palette.defaultBackground) : QColor(chrome)));
                mismatched += sameTone ? 0 : 1;
                auto const ground = vtbackend::RGBColor { static_cast<uint8_t>(fill.red()),
                                                          static_cast<uint8_t>(fill.green()),
                                                          static_cast<uint8_t>(fill.blue()) };
                CHECK(vtbackend::contrastRatio(ground, inkOver(ground, tone)) >= 4.5);
            });
        }
    }
    // The pass is not vacuous: every scheme is mismatched under one of the two chromes.
    CHECK(mismatched > 0);
}
