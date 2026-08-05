// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/UiStyleProvider.h>
#include <contour/config/UiStyle.h>

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

namespace contour::test
{

/// Installs the chrome's design tokens on @p engine, as ContourGuiApp does for the real one.
///
/// Every chrome component reads its metrics, fonts and glyphs from the `chromeStyle` context
/// property, so an engine without one loads nothing. Being a context property, it is per-engine --
/// which is what lets one test binary drive both styles (see UiStyleProvider.h for why a context
/// property rather than a QML singleton).
///
/// @param engine The engine to install into. Also becomes the provider's parent, so the tokens live
///               exactly as long as the engine that reads them.
/// @param style  The chrome style the QML under test should see. Defaults to the shipped default, so
///               a test that does not care asserts against today's appearance.
inline void installChromeStyle(QQmlEngine& engine, config::UiStyle style = config::UiStyle::Native)
{
    auto* provider = new UiStyleProvider(style, QGuiApplication::font(), &engine);
    engine.rootContext()->setContextProperty(QStringLiteral("chromeStyle"), provider);
}

} // namespace contour::test
