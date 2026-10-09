// SPDX-License-Identifier: Apache-2.0
#include <contour/display/FirstFrameWatchdog.hpp>

#include <QtQuick/QQuickWindow>

#include <utility>

namespace contour::display
{

FirstFrameWatchdog::FirstFrameWatchdog(QQuickWindow& window,
                                       std::chrono::milliseconds budget,
                                       std::function<void()> onFailure):
    _onFailure { std::move(onFailure) }
{
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &FirstFrameWatchdog::fail);
    connect(&window, &QQuickWindow::frameSwapped, this, [this] {
        _timer.stop();
        _onFailure = nullptr;
    });
    connect(&window, &QQuickWindow::sceneGraphError, this, &FirstFrameWatchdog::fail);
    _timer.start(budget);
}

void FirstFrameWatchdog::fail()
{
    _timer.stop();
    if (auto onFailure = std::exchange(_onFailure, nullptr))
        onFailure();
}

} // namespace contour::display
