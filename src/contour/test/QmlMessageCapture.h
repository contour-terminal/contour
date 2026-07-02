// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QStringList>
#include <QtCore/QtGlobal>

#include <algorithm>

namespace contour::test
{

/// RAII guard that installs a Qt message handler capturing warning/critical messages for the lifetime of the
/// scope, then restores the previously installed handler on destruction.
///
/// QML/JS diagnostics (a ReferenceError, TypeError, binding loop, missing context property, ...) are real
/// defects, but Qt reports them through the global message handler — NOT as a component-load error or a test
/// assertion — so without capturing them a test can emit "ReferenceError: terminalSessions is not defined"
/// and still report "All tests passed". This guard collects them so a test (or the whole run) can fail.
///
/// It CHAINS to the previously installed handler, so guards nest correctly: a per-test capture forwards to
/// the process-wide gate (installed in test_main.cpp), which forwards to Qt's default — a per-test capture
/// never blinds the run-wide gate. `qInstallMessageHandler` takes a plain function pointer (no captures), so
/// the installed trampoline routes to the innermost live guard via a file-scope pointer; guards are
/// stack-scoped and single-threaded in these tests, so one "active" pointer chained through `_previousGuard`
/// is sufficient.
///
/// Exception-safe: the destructor restores during stack unwinding even when a Catch2 REQUIRE throws.
class QmlMessageCapture
{
  public:
    QmlMessageCapture()
    {
        _previousGuard = s_activeCapture;
        s_activeCapture = this;
        _previousHandler = qInstallMessageHandler(&QmlMessageCapture::trampoline);
    }

    ~QmlMessageCapture()
    {
        qInstallMessageHandler(_previousHandler);
        s_activeCapture = _previousGuard;
    }

    QmlMessageCapture(QmlMessageCapture const&) = delete;
    QmlMessageCapture& operator=(QmlMessageCapture const&) = delete;
    QmlMessageCapture(QmlMessageCapture&&) = delete;
    QmlMessageCapture& operator=(QmlMessageCapture&&) = delete;

    /// @return The captured warning/critical messages, in order.
    [[nodiscard]] QStringList const& messages() const noexcept { return _messages; }

    /// @return How many captured messages satisfy @p predicate — the count-of-matching-warnings idiom the
    ///         tests assert against (e.g. contains "TypeError").
    template <typename Predicate>
    [[nodiscard]] long count(Predicate predicate) const
    {
        return std::count_if(_messages.begin(), _messages.end(), predicate);
    }

  private:
    /// The capture-less handler Qt calls; records warning/critical messages on the innermost live guard and
    /// forwards to the guard that was active before it (which chains outward to the run-wide gate and finally
    /// Qt's default), so nested guards all observe every message.
    static void trampoline(QtMsgType type, QMessageLogContext const& context, QString const& msg)
    {
        auto* self = s_activeCapture;
        if (self == nullptr)
            return;
        if (type == QtWarningMsg || type == QtCriticalMsg)
            self->_messages << msg;
        if (self->_previousHandler != nullptr)
            self->_previousHandler(type, context, msg);
    }

    QStringList _messages;
    QtMessageHandler _previousHandler = nullptr;
    QmlMessageCapture* _previousGuard = nullptr;

    // Innermost live guard the trampoline records on. Set to `this` in the ctor (saving the outer guard),
    // restored to the outer guard in the dtor. Single-threaded, stack-scoped use in the test binary.
    static inline QmlMessageCapture* s_activeCapture = nullptr;
};

} // namespace contour::test
