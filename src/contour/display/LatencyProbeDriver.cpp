// SPDX-License-Identifier: Apache-2.0
#include <contour/display/LatencyProbeDriver.hpp>
#include <contour/display/TerminalDisplay.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QKeyEvent>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <utility>

#include <vthost/latency/EchoProtocol.hpp>
#include <vthost/latency/KeystrokeProbe.hpp>

namespace contour::display
{

namespace
{
    constexpr auto TickInterval = std::chrono::milliseconds { 1 };

    void sendKey(QQuickWindow& window, int qtKey, Qt::KeyboardModifiers modifiers, QString const& text)
    {
        auto press = QKeyEvent(QEvent::KeyPress, qtKey, modifiers, text);
        QCoreApplication::sendEvent(&window, &press);
        auto release = QKeyEvent(QEvent::KeyRelease, qtKey, modifiers, text);
        QCoreApplication::sendEvent(&window, &release);
    }
} // namespace

LatencyProbeDriver::LatencyProbeDriver(QQuickWindow& window,
                                       TerminalDisplay& display,
                                       vthost::latency::KeystrokeProbe& probe,
                                       std::function<void()> onFinished):
    _window { &window }, _probe { probe }, _onFinished { std::move(onFinished) }
{
    display.forceActiveFocus();
    // The render thread swaps the frame, so the handler runs there: that is the instant the probe stamps.
    // It holds the display's cursor-column cell, never the display, which the GUI thread may destroy
    // while a frame is in flight.
    connect(
        &window,
        &QQuickWindow::frameSwapped,
        this,
        [&probe, column = display.drawnCursorColumn()] { probe.frameSwapped(column->load()); },
        Qt::DirectConnection);
    _timer.setTimerType(Qt::PreciseTimer);
    _timer.setInterval(TickInterval);
    connect(&_timer, &QTimer::timeout, this, &LatencyProbeDriver::tick);
    _timer.start();
}

void LatencyProbeDriver::tick()
{
    using vthost::latency::ProbePhase;
    if (auto const key = _probe.takeDueKey())
        typeKey(*key);
    switch (_probe.phase())
    {
        case ProbePhase::Finished:
        case ProbePhase::PromptTimedOut:
        case ProbePhase::EchoTimedOut: finish(); return;
        case ProbePhase::AwaitingPrompt:
        case ProbePhase::Pausing:
        case ProbePhase::AwaitingEcho: return;
    }
}

void LatencyProbeDriver::typeKey(char key)
{
    if (_window)
        sendKey(*_window, Qt::Key_A + (key - 'a'), Qt::NoModifier, QString(QChar(key)));
}

void LatencyProbeDriver::finish()
{
    _timer.stop();
    // Ctrl+D ends the echo child, so an in-process session closes and a daemon session ends.
    if (_window)
        sendKey(*_window, Qt::Key_D, Qt::ControlModifier, QString(QChar(vthost::latency::QuitByte)));
    if (auto const onFinished = std::exchange(_onFinished, nullptr))
        onFinished();
}

} // namespace contour::display
