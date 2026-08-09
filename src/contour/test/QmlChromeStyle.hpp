// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace contour::test
{

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
/// @param style              The chrome style the QML under test should see. Defaults to the shipped
///                           default, so a test that does not care asserts against today's
///                           appearance.
/// @param windowControlStyle Which window controls the QML under test should see. Defaults to
///                           @c Windows rather than to the shipped @c Auto ON PURPOSE: Auto resolves
///                           against the host, so a geometry assertion written against it would pass
///                           on Linux and fail on macOS. A test that cares names the style it means.
inline void installChromeStyle(
    QQmlEngine& engine,
    config::UiStyle style = config::UiStyle::Native,
    config::WindowControlStyle windowControlStyle = config::WindowControlStyle::Windows)
{
    auto* provider = new contour::window::UiStyleProvider(style, QGuiApplication::font(), &engine);
    engine.rootContext()->setContextProperty(QStringLiteral("chromeStyle"), provider);

    auto* controls = new contour::window::WindowControlStyleProvider(windowControlStyle, &engine);
    engine.rootContext()->setContextProperty(QStringLiteral("windowControls"), controls);
}

} // namespace contour::test
