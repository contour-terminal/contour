// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <functional>

class QQuickWindow;

namespace vthost::latency
{
class KeystrokeProbe;
}

namespace contour::display
{

class TerminalDisplay;

/// The Qt adapter of the keystroke probe: types the probe's keys into a window and tells the probe
/// which cursor column each swapped frame drew.
///
/// Key-to-pixel starts when takeDueKey() reads the clock on the GUI thread just before the key press
/// is delivered to the window, and ends in the frameSwapped handler of the first frame that drew the
/// cursor where the key's echo leaves it. The driver never decides anything: the probe does.
class LatencyProbeDriver final: public QObject
{
    Q_OBJECT

  public:
    /// Starts driving at once.
    /// @param window The window the keys are delivered to and whose frames are watched.
    /// @param display The terminal display whose last drawn cursor column each frame reports. Given
    ///        keyboard focus. Touched only here: frames read its shared cursor-column cell, so the display
    ///        may be destroyed before the driver (its frames then stop advancing the probe).
    /// @param probe The probe to feed and to ask for keys. Borrowed: it must outlive the driver.
    /// @param onFinished Called once, on the GUI thread, after the probe reached a terminal phase and
    ///        the quit byte was sent.
    LatencyProbeDriver(QQuickWindow& window,
                       TerminalDisplay& display,
                       vthost::latency::KeystrokeProbe& probe,
                       std::function<void()> onFinished);

  private:
    void tick();
    void typeKey(char key);
    void finish();

    /// Null once the window is gone: the driver then types nothing, and the probe's timeouts end the run.
    QPointer<QQuickWindow> _window;
    vthost::latency::KeystrokeProbe& _probe;
    std::function<void()> _onFinished;
    QTimer _timer;
};

} // namespace contour::display
