// SPDX-License-Identifier: Apache-2.0
//
// Test runner for the contour GUI frontend tests. Forces the Qt "offscreen" platform so the QML
// engine and scene graph run without a display server (CI-safe), then constructs the
// QGuiApplication that the QML components need before running the Catch2 session.

#include <contour/test/QmlMessageCapture.h>

#include <crispy/SuppressWindowsDialogs.hpp>

#include <QtCore/QStringList>
#include <QtGui/QGuiApplication>

#include <iostream>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    crispy::suppressWindowsDialogs();

    /// Default to the headless "offscreen" platform (CI-safe, no display server). The Qt Quick scene
    /// graph, however, cannot stand up a real render loop there — so the display-gated rendering
    /// tests (DisplayRendering_test.cpp, tag [display]) need a REAL windowing system. Setting
    /// CONTOUR_TEST_DISPLAY=1 opts into one, with this platform-selection contract:
    ///   - An explicitly set QT_QPA_PLATFORM is respected verbatim (the CI display route runs the
    ///     suite under a private headless weston and sets `wayland`; see
    ///     scripts/run-display-tests.sh).
    ///   - Otherwise, when DISPLAY is available, default to `xcb`: a LIVE desktop Wayland compositor
    ///     may withhold frame callbacks from unexposed/occluded test windows, stalling
    ///     QQuickWindow::grabWindow() indefinitely, while X11/XWayland keeps delivering frames.
    ///   - Otherwise leave the platform untouched and let Qt pick.
    if (qgetenv("CONTOUR_TEST_DISPLAY") != "1")
    {
#if defined(_WIN32)
        _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
        setenv("QT_QPA_PLATFORM", "offscreen", /*overwrite*/ 1);
#endif
    }
#if !defined(_WIN32)
    else if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY"))
        setenv("QT_QPA_PLATFORM", "xcb", /*overwrite*/ 0);
#endif

    QGuiApplication app(argc, argv);

    // Run-wide gate: capture every warning/critical for the whole session. Per-test QmlMessageCapture guards
    // chain to this one (see QmlMessageCapture.h), so a per-test guard never blinds the gate.
    contour::test::QmlMessageCapture gate;

    auto const testResult = Catch::Session().run(argc, argv);

    QStringList const qmlErrors = [&] {
        QStringList out;
        for (auto const& m: gate.messages())
            if (contour::test::isQmlDiagnostic(m))
                out << m;
        return out;
    }();

    if (!qmlErrors.isEmpty())
    {
        std::cerr << "\n=================================================================\n"
                  << "FAILURE: " << qmlErrors.size()
                  << " QML error(s)/warning(s) were emitted during the test run (a passing test that emits a "
                     "QML error is still a bug):\n";
        for (auto const& m: qmlErrors)
            std::cerr << "  - " << m.toStdString() << '\n';
        std::cerr << "=================================================================\n";
        return testResult != 0 ? testResult : 1;
    }

    return testResult;
}
