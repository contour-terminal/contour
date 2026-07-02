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

namespace
{
/// A QML/JS diagnostic (ReferenceError, TypeError, binding loop, missing context property, ...) emitted
/// during the run is a real defect: Qt reports it through the message handler, NOT as a component-load error
/// or a test assertion, so without a run-wide gate a test can emit "ReferenceError: terminalSessions is not
/// defined" and still report "All tests passed". Match a QML/JS origin so unrelated library chatter (libva,
/// ffmpeg, va-api info) does not trip the gate.
[[nodiscard]] bool isQmlDiagnostic(QString const& msg)
{
    return msg.contains(QStringLiteral(".qml")) || msg.contains(QStringLiteral("qrc:/"))
           || msg.contains(QStringLiteral("ReferenceError")) || msg.contains(QStringLiteral("TypeError"));
}
} // namespace

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

    // Run-wide gate: capture every warning/critical for the whole session. Per-test QmlMessageCapture guards
    // chain to this one (see QmlMessageCapture.h), so a per-test guard never blinds the gate.
    contour::test::QmlMessageCapture gate;

    auto const testResult = Catch::Session().run(argc, argv);

    QStringList const qmlErrors = [&] {
        QStringList out;
        for (auto const& m: gate.messages())
            if (isQmlDiagnostic(m))
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
