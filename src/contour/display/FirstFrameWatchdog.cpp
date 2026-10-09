// SPDX-License-Identifier: Apache-2.0
#include <contour/display/FirstFrameWatchdog.hpp>

#include <QtCore/QEvent>
#include <QtQuick/QQuickWindow>

#include <utility>

namespace contour::display
{

FirstFrameWatchdog::FirstFrameWatchdog(QQuickWindow& window,
                                       std::chrono::milliseconds budget,
                                       std::function<void()> onFailure):
    _window { &window }, _budget { budget }, _onFailure { std::move(onFailure) }
{
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &FirstFrameWatchdog::fail);
    connect(&window, &QQuickWindow::frameSwapped, this, &FirstFrameWatchdog::settle);
    connect(&window, &QQuickWindow::sceneGraphError, this, &FirstFrameWatchdog::fail);
    if (window.isExposed())
        startBudget();
    else
        // An event filter on Expose: QWindow::exposeEvent is no signal, and visibleChanged fires before
        // the platform has actually exposed the surface (a minimized window is visible but not exposed).
        window.installEventFilter(this);
}

bool FirstFrameWatchdog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _window && event->type() == QEvent::Expose && _window->isExposed())
        startBudget();
    return QObject::eventFilter(watched, event);
}

void FirstFrameWatchdog::startBudget()
{
    _window->removeEventFilter(this);
    if (_onFailure) // a frame or a scene-graph error may already have settled it
        _timer.start(_budget);
}

void FirstFrameWatchdog::settle()
{
    _timer.stop();
    _onFailure = nullptr;
    // Hand scene-graph errors back to Qt: while this is connected, Qt assumes they are handled.
    disconnect(_window, nullptr, this, nullptr);
    _window->removeEventFilter(this);
}

void FirstFrameWatchdog::fail()
{
    if (auto onFailure = std::exchange(_onFailure, nullptr))
    {
        settle();
        onFailure();
    }
}

} // namespace contour::display
