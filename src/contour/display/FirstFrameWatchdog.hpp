// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <chrono>
#include <functional>

class QQuickWindow;

namespace contour::display
{

/// Reports a window whose GPU cannot render: no first frame within a budget of its exposure, or a
/// scene-graph error. A window that is not exposed (minimized, on another workspace, occluded) is not
/// expected to draw, so its budget starts only when it is first exposed.
class FirstFrameWatchdog: public QObject
{
    Q_OBJECT

  public:
    /// Starts watching immediately; the budget starts once @p window is exposed.
    /// @param window The window whose first frame is awaited.
    /// @param budget How long the first frame may take, counted from the window's first exposure.
    /// @param onFailure Called at most once.
    FirstFrameWatchdog(QQuickWindow& window,
                       std::chrono::milliseconds budget,
                       std::function<void()> onFailure);

  protected:
    /// Starts the budget when @p watched receives its first Expose event while exposed.
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void startBudget();
    void fail();

    QQuickWindow* _window;
    std::chrono::milliseconds _budget;
    std::function<void()> _onFailure;
    QTimer _timer;
};

} // namespace contour::display
