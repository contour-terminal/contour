// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/Notifier.hpp>

#include <QtCore/QString>

#include <string>
#include <vector>

namespace contour::test
{

/// A Notifier that raises nothing and records everything, and can play the desktop's own close and
/// activation events back at the session. Deliberately not a mock framework: the recorded vectors
/// ARE the assertions.
///
/// Without this the notification path could only be asserted not to throw: the real backend reaches
/// a D-Bus session that a headless run does not have.
class RecordingNotifier final: public contour::platform::Notifier
{
  public:
    void notify(vtbackend::DesktopNotification const& notification) override
    {
        raised.push_back(notification);
    }

    void close(std::string const& identifier) override { closed.push_back(identifier); }

    /// Plays back what the desktop would report once the user (or a timeout) retired a notification.
    /// @param report Whether the backend observed the close or a timer merely assumed it; the
    ///               portal backend can only ever produce the latter.
    void fireClosed(std::string const& identifier,
                    uint reason,
                    vtbackend::CloseReport report = vtbackend::CloseReport::Observed)
    {
        emit notificationClosed(QString::fromStdString(identifier), reason, report);
    }

    /// Plays back what the desktop would report when the user clicked a notification.
    void fireActivated(std::string const& identifier)
    {
        emit actionInvoked(QString::fromStdString(identifier));
    }

    std::vector<vtbackend::DesktopNotification> raised; ///< What was raised, in order.
    std::vector<std::string> closed;                    ///< What was withdrawn, in order.
};

} // namespace contour::test
