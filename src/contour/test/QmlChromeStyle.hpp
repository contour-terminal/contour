// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/window/PopupShadowImageProvider.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace contour::test
{

/// Everything the chrome's two context properties are built from, so a test can name a row
/// (including ones that no HostPlatform::Other resolution reaches, such as MacOS and Fluent).
struct ChromeSetup
{
    config::ChromeStyle row;                   ///< The resolved chrome token row.
    QFont font;                                ///< The application font the tokens derive text sizes from.
    config::WindowControlStyle windowControls; ///< Which window controls the QML sees.
    config::AccessibilityPreferences accessibility {}; ///< The OS accessibility choices the tokens apply.
    platform::PlatformAppearance appearance {};        ///< What the OS publishes about its own look.
    /// The terminal scheme the Terminal row paints with: the default palette's unless a test names one.
    config::ChromeSchemeColors scheme { config::chromeSchemeColorsOf(vtbackend::ColorPalette {}) };
    config::ChromeDensity density {}; ///< The chrome density the tokens resolve with.
};

/// Installs the chrome's design tokens on @p engine, as ContourGuiApp does for the real one.
///
/// Every chrome component reads its metrics, fonts and glyphs from the `chromeStyle` context
/// property and its window-control placement from `windowControls`, so an engine without both loads
/// nothing. Being context properties they are per-engine -- which is what lets one test binary drive
/// every combination of the two (see UiStyleProvider.hpp for why context properties rather than QML
/// singletons).
///
/// Both are installed together, by one call, because a chrome component needs both to load: making
/// the second a separate call every test had to remember would mean the ones that forgot fail with
/// a QML reference error rather than with whatever they were actually asserting.
///
/// @param engine             The engine to install into. Also becomes both providers' parent, so the
///                           tokens live exactly as long as the engine that reads them.
/// @param setup              The row, font and window controls to install.
inline void installChromeStyle(QQmlEngine& engine, ChromeSetup const& setup)
{
    auto* provider = new contour::window::UiStyleProvider(
        contour::window::ChromeInputs { .row = setup.row,
                                        .font = setup.font,
                                        .accessibility = setup.accessibility,
                                        .appearance = setup.appearance,
                                        .scheme = setup.scheme,
                                        .density = setup.density },
        &engine);
    engine.rootContext()->setContextProperty(QStringLiteral("chromeStyle"), provider);

    auto* controls = new contour::window::WindowControlStyleProvider(setup.windowControls, &engine);
    engine.rootContext()->setContextProperty(QStringLiteral("windowControls"), controls);

    // The popups' drop shadow is served as an image, exactly as ContourGuiApp serves it: without
    // this every popup logs "Invalid image provider", which the run-wide gate turns into a failure.
    engine.addImageProvider(QStringLiteral("popupshadow"), new contour::window::PopupShadowImageProvider());
}

/// Builds the chrome setup that a test sees when it names no row: @p style resolved against
/// HostPlatform::Other, the application font, and @p windowControlStyle.
/// @param style              The chrome style to resolve.
/// @param windowControlStyle Which window controls the QML under test should see.
/// @return The resolved setup.
[[nodiscard]] inline ChromeSetup defaultChromeSetup(
    config::UiStyle style = config::UiStyle::Native,
    config::WindowControlStyle windowControlStyle = config::WindowControlStyle::Windows)
{
    return ChromeSetup { .row = config::resolveChromeStyle(style, config::HostPlatform::Other),
                         .font = QGuiApplication::font(),
                         .windowControls = windowControlStyle };
}

/// Installs the chrome's design tokens resolved from @p style against HostPlatform::Other, so a
/// test that names no row sees today's tokens. See the ChromeSetup overload for the details.
/// @param engine             The engine to install into.
/// @param style              The chrome style the QML under test should see.
/// @param windowControlStyle Which window controls the QML under test should see. Defaults to
///                           @c Windows rather than @c Auto ON PURPOSE: Auto resolves against the host.
inline void installChromeStyle(
    QQmlEngine& engine,
    config::UiStyle style = config::UiStyle::Native,
    config::WindowControlStyle windowControlStyle = config::WindowControlStyle::Windows)
{
    installChromeStyle(engine, defaultChromeSetup(style, windowControlStyle));
}

/// Forces the application's palette and color scheme to one appearance for the lifetime of the scope.
///
/// The suite never pins a scheme (see test_main.cpp), so the destructor gives back the palette it found
/// and unpins the scheme.
class ScopedChromeAppearance
{
  public:
    /// @param appearance The appearance to force: buildThemePalette's palette, and on Qt 6.8 or later
    ///                   the matching QStyleHints color scheme.
    explicit ScopedChromeAppearance(vtbackend::ColorPreference appearance):
        _saved { QGuiApplication::palette() }
    {
        auto const scheme =
            appearance == vtbackend::ColorPreference::Light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark;
        QGuiApplication::setPalette(platform::buildThemePalette(scheme));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QGuiApplication::styleHints()->setColorScheme(scheme);
#endif
    }

    ~ScopedChromeAppearance()
    {
        QGuiApplication::setPalette(_saved);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QGuiApplication::styleHints()->unsetColorScheme();
#endif
    }

    ScopedChromeAppearance(ScopedChromeAppearance const&) = delete;
    ScopedChromeAppearance& operator=(ScopedChromeAppearance const&) = delete;
    ScopedChromeAppearance(ScopedChromeAppearance&&) = delete;
    ScopedChromeAppearance& operator=(ScopedChromeAppearance&&) = delete;

  private:
    QPalette _saved;
};

} // namespace contour::test
