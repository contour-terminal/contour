// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <chrono>
#include <functional>

class QQuickWindow;

namespace contour::display
{

/// Reports a window whose GPU cannot render: no first frame within a budget, or a scene-graph error.
class FirstFrameWatchdog: public QObject
{
    Q_OBJECT

  public:
    /// Starts watching immediately.
    /// @param window The window whose first frame is awaited.
    /// @param budget How long the first frame may take.
    /// @param onFailure Called at most once.
    FirstFrameWatchdog(QQuickWindow& window,
                       std::chrono::milliseconds budget,
                       std::function<void()> onFailure);

  private:
    void fail();

    std::function<void()> _onFailure;
    QTimer _timer;
};

} // namespace contour::display
