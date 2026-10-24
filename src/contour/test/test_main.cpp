// SPDX-License-Identifier: Apache-2.0
//
// Test runner for the contour GUI frontend tests. Forces the Qt "offscreen" platform so the QML
// engine and scene graph run without a display server (CI-safe), then constructs the
// QGuiApplication that the QML components need before running the Catch2 session.

#include <contour/test/NotificationFixtures.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/Environment.hpp>
#include <core/testing/SuppressWindowsDialogs.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QStringList>
#include <QtGui/QGuiApplication>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include <QtQuickControls2/QQuickStyle>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace
{

/// Deletes what a test run left to deleteLater() once that run is over.
///
/// Tearing down QML hands much of the work to the event loop: a Loader, a Repeater releasing its
/// delegates and a Menu all delete their items with deleteLater(). A test that drives a page and
/// returns without spinning the loop again leaves those deletions queued, and QCoreApplication drops
/// the queue at exit without deleting a single receiver. Everything those objects still hold, the
/// font engines their texts shaped with among it, then outlives the run, and LeakSanitizer reports
/// what it can no longer reach. In a long run the next test's event processing hides this; run alone,
/// the test leaks. The application itself never stops spinning its loop, so it has no such queue.
class DeferredDeletionDrain final: public Catch::EventListenerBase
{
  public:
    using Catch::EventListenerBase::EventListenerBase;

    /// Runs the deletions queued by one run of a test case, after its locals are gone.
    void testCasePartialEnded(Catch::TestCaseStats const& /*stats*/, std::uint64_t /*partNumber*/) override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

/// Whether this process runs QtDBus's connection thread.
///
/// QtDBus starts it on the first use of any bus and keeps it until the process ends, so its presence
/// at the end of a run says that something in the run reached a session bus -- a session handed the
/// desktop's notifier, a transport built against the real bus -- whichever test did it. Read from the
/// thread names the kernel keeps per task; elsewhere than Linux there is no D-Bus to reach.
[[nodiscard]] bool reachedSessionBus()
{
    auto const tasks = std::filesystem::path { "/proc/self/task" };
    auto error = std::error_code {};
    for (auto const& task: std::filesystem::directory_iterator { tasks, error })
    {
        auto comm = std::ifstream { task.path() / "comm" };
        auto name = std::string {};
        std::getline(comm, name);
        if (name == "QDBusConnection")
            return true;
    }
    return false;
}

} // namespace

CATCH_REGISTER_LISTENER(DeferredDeletionDrain)

int main(int argc, char* argv[])
{
    core::testing::suppressWindowsDialogs();

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
    if (!contour::test::displayTestsEnabled(core::LiveEnvironment {}))
    {
#ifdef _WIN32
        _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }
#ifndef _WIN32
    else if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif

    QGuiApplication const app(argc, argv);

    // Pin a Qt Quick Controls style the way the app itself does (ContourGuiApp: a Fusion fallback, then
    // the row's style). The tests instantiate real Controls (SessionChrome's customized ScrollBar, the
    // tab flyout, ...), and the platform-native style (notably on Windows) refuses contentItem/background
    // customization, emitting a QML diagnostic that the gate below then fails on. Fusion supports
    // customization on every platform, so it is the default here as it is in production.
    //
    // CONTOUR_TEST_CONTROLS_STYLE names another style for the whole run. A style is resolved once per
    // process, before the first control exists, so the rows a style draws -- the Terminal row's
    // ContourTui, notably -- can only be rendered by a second run under it; the chrome snapshot case
    // (ChromeSnapshot_test.cpp, tag [snapshot]) draws the rows of whichever style this run has.
    QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
    QQuickStyle::setStyle(QString::fromStdString(contour::test::controlsStyleFor(core::LiveEnvironment {})));

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

    // Run-wide gate: no test reaches the machine's session bus. A session gets the harness's notifier,
    // and the cases that drive a real transport run only on a private bus a harness stands up
    // (contour::test::onPrivateSessionBus()): the wedged-bus harness's, or the display route's, where
    // a real platform plugin reaches the bus whatever the tests do.
    if (!contour::test::onPrivateSessionBus() && reachedSessionBus())
    {
        std::cerr << "\n=================================================================\n"
                  << "FAILURE: a test reached the session bus (a QDBusConnection thread is running). Give a\n"
                     "session contour::test::makeTestNotifier(), or run a real-transport case only under\n"
                     "contour::test::onPrivateSessionBus(). Run the suite by tag to find which test it was.\n"
                  << "=================================================================\n";
        return testResult != 0 ? testResult : 1;
    }

    return testResult;
}
