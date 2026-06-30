// SPDX-License-Identifier: Apache-2.0
//
// Test runner for the contour GUI frontend tests. Forces the Qt "offscreen" platform so the QML
// engine and scene graph run without a display server (CI-safe), then constructs the
// QGuiApplication that the QML components need before running the Catch2 session.

#include <crispy/SuppressWindowsDialogs.hpp>

#include <QtGui/QGuiApplication>

#include <cstdlib>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    crispy::testing::suppressWindowsDialogs();

    // Run headless: no display server required.
#if defined(_WIN32)
    _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
    setenv("QT_QPA_PLATFORM", "offscreen", /*overwrite*/ 1);
#endif

    QGuiApplication app(argc, argv);

    return Catch::Session().run(argc, argv);
}
