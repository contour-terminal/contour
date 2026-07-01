// SPDX-License-Identifier: Apache-2.0
//
// Test runner for the contour GUI frontend tests. Forces the Qt "offscreen" platform so the QML
// engine and scene graph run without a display server (CI-safe), then constructs the
// QGuiApplication that the QML components need before running the Catch2 session.

#include <crispy/SuppressWindowsDialogs.hpp>

#include <QtCore/QStringList>
#include <QtGui/QGuiApplication>

#include <iostream>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

namespace
{
/// Collects the QML warning/critical messages seen during the run and chains to the previous handler. Any
/// QML/JS diagnostic emitted DURING the test run (a ReferenceError, TypeError, binding loop, missing
/// context property, ...) is a real defect: Qt reports those through the message handler, NOT as a
/// component-load error or a test assertion, so without this gate a test can emit "ReferenceError:
/// terminalSessions is not defined" and still report "All tests passed". Encapsulated (no free globals) so
/// the collected list is owned in one place; main() fails the run if it is non-empty.
class QmlMessageGate;

// The active gate the capture-less Qt handler forwards to (a file-scope anonymous-namespace pointer, not a
// static class member, because qInstallMessageHandler takes a plain function pointer). One gate is alive at
// a time for the whole run.
QmlMessageGate* activeQmlGate = nullptr;

class QmlMessageGate
{
  public:
    QmlMessageGate()
    {
        activeQmlGate = this;
        _previous = qInstallMessageHandler(&QmlMessageGate::handler);
    }
    ~QmlMessageGate()
    {
        qInstallMessageHandler(_previous);
        activeQmlGate = nullptr;
    }

    QmlMessageGate(QmlMessageGate const&) = delete;
    QmlMessageGate& operator=(QmlMessageGate const&) = delete;
    QmlMessageGate(QmlMessageGate&&) = delete;
    QmlMessageGate& operator=(QmlMessageGate&&) = delete;

    [[nodiscard]] QStringList const& messages() const { return _messages; }

  private:
    static void handler(QtMsgType type, QMessageLogContext const& context, QString const& msg)
    {
        // Match a QML/JS origin so unrelated library chatter (libva, ffmpeg, va-api info lines) does not
        // trip the gate.
        if (activeQmlGate != nullptr && (type == QtWarningMsg || type == QtCriticalMsg)
            && (msg.contains(QStringLiteral(".qml")) || msg.contains(QStringLiteral("qrc:/"))
                || msg.contains(QStringLiteral("ReferenceError"))
                || msg.contains(QStringLiteral("TypeError"))))
        {
            activeQmlGate->_messages << msg;
        }
        if (activeQmlGate != nullptr && activeQmlGate->_previous != nullptr)
            activeQmlGate->_previous(type, context, msg);
    }

    QStringList _messages;
    QtMessageHandler _previous = nullptr;
};
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

    QmlMessageGate gate;

    auto const testResult = Catch::Session().run(argc, argv);

    if (!gate.messages().isEmpty())
    {
        std::cerr << "\n=================================================================\n"
                  << "FAILURE: " << gate.messages().size()
                  << " QML error(s)/warning(s) were emitted during the test run (a passing test that emits a "
                     "QML error is still a bug):\n";
        for (auto const& m: gate.messages())
            std::cerr << "  - " << m.toStdString() << '\n';
        std::cerr << "=================================================================\n";
        return testResult != 0 ? testResult : 1;
    }

    return testResult;
}
