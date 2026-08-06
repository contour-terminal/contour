// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QStringList>
#include <QtCore/QtGlobal>

#include <algorithm>

namespace contour::test
{

/// Classifies a captured Qt message as a QML/JS diagnostic — a real defect the test gate must fail on —
/// as opposed to unrelated library chatter (libva, ffmpeg, va-api info) that must not trip the gate.
///
/// Matched origins: a source location (".qml" / "qrc:/"), a JS error class (ReferenceError/TypeError), or a
/// declarative binding loop. "Binding loop" is matched on the message body because components created via
/// QQmlComponent::setData() with a non-.qml URL report it with an "<Unknown File>" context — no ".qml" or
/// "qrc:/" appears anywhere in the message.
[[nodiscard]] inline bool isQmlDiagnostic(QString const& msg)
{
    return msg.contains(QStringLiteral(".qml")) || msg.contains(QStringLiteral("qrc:/"))
           || msg.contains(QStringLiteral("ReferenceError")) || msg.contains(QStringLiteral("TypeError"))
           || msg.contains(QStringLiteral("Binding loop"));
}

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
    QmlMessageCapture():
        _previousHandler(qInstallMessageHandler(&QmlMessageCapture::trampoline)),
        _previousGuard(ActiveCapture)
    {
        ActiveCapture = this;
    }

    ~QmlMessageCapture()
    {
        qInstallMessageHandler(_previousHandler);
        ActiveCapture = _previousGuard;
    }

    QmlMessageCapture(QmlMessageCapture const&) = delete;
    QmlMessageCapture& operator=(QmlMessageCapture const&) = delete;
    QmlMessageCapture(QmlMessageCapture&&) = delete;
    QmlMessageCapture& operator=(QmlMessageCapture&&) = delete;

    /// Upper bound on RETAINED messages. A defect that emits messages from an event/binding loop
    /// floods the handler millions of times; retaining every copy once ate all host RAM (the list
    /// alone reached gigabytes) and got the test run OOM-killed. Past the cap, messages are only
    /// counted (overflowCount), which still fails the gate while keeping memory bounded.
    static constexpr qsizetype MaxRetainedMessages = 2048;

    /// @return The captured warning/critical messages, in order (at most MaxRetainedMessages).
    [[nodiscard]] QStringList const& messages() const noexcept { return _messages; }

    /// @return How many further messages arrived after the retention cap was reached.
    [[nodiscard]] qint64 overflowCount() const noexcept { return _overflow; }

    /// @return How many captured messages satisfy @p predicate — the count-of-matching-warnings idiom the
    ///         tests assert against (e.g. contains "TypeError").
    template <typename Predicate>
    [[nodiscard]] long count(Predicate predicate) const
    {
        return std::count_if(_messages.begin(), _messages.end(), predicate);
    }

  private:
    /// The capture-less handler Qt calls; records warning/critical messages on EVERY live guard
    /// (innermost to outermost, so a per-test capture never blinds the run-wide gate), then forwards
    /// exactly once to the non-guard handler installed before the outermost guard (Qt's default).
    ///
    /// It must NOT forward by calling the previously installed function pointer: every guard installs
    /// this same trampoline, so with nested guards that pointer IS the trampoline again — and because
    /// ActiveCapture still names the innermost guard, the "chain" re-enters itself. The tail call gets
    /// optimized into a flat infinite loop, so the FIRST message captured under nested guards spun
    /// forever, growing the retained list until the OOM killer took the whole host down. Walking the
    /// guard list sidesteps re-entry entirely.
    static void trampoline(QtMsgType type, QMessageLogContext const& context, QString const& msg)
    {
        QtMessageHandler preGuardHandler = nullptr;
        for (auto* guard = ActiveCapture; guard != nullptr; guard = guard->_previousGuard)
        {
            if (type == QtWarningMsg || type == QtCriticalMsg)
            {
                if (guard->_messages.size() < MaxRetainedMessages)
                    guard->_messages << msg;
                else
                    ++guard->_overflow;
            }
            preGuardHandler = guard->_previousHandler;
        }
        if (preGuardHandler != nullptr && preGuardHandler != &QmlMessageCapture::trampoline)
            preGuardHandler(type, context, msg);
    }

    QStringList _messages;
    qint64 _overflow = 0;
    QtMessageHandler _previousHandler = nullptr;
    QmlMessageCapture* _previousGuard = nullptr;

    // Innermost live guard the trampoline records on. Set to `this` in the ctor (saving the outer guard),
    // restored to the outer guard in the dtor. Single-threaded, stack-scoped use in the test binary.
    static inline QmlMessageCapture* ActiveCapture = nullptr;
};

} // namespace contour::test
