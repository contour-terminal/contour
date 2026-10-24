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
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/window/ChromeStyleHost.hpp>
#include <contour/window/ChromeTokenObjects.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/core/Color.hpp>

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
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

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
    CHECK(provider.paneDimColor().alpha() == 153);
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
    CHECK(provider.paneDimTextColor() == QColor(Qt::white));
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
    CHECK(tokenOf(mac, "motion", "standard").toInt() == 160);
    CHECK(tokenOf(mac, "space", "s10").toReal() == 32.0);
    CHECK(tokenOf(mac, "material", "titleBand").toInt() == tokenOf(mac, "material", "glass").toInt());
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
