// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__linux__)

    #include <contour/NotificationRouter.h>

    #include <vtbackend/DesktopNotification.h>

    #include <QtCore/QObject>
    #include <QtDBus/QDBusInterface>

    #include <cstdint>
    #include <string>

namespace contour
{

/// D-Bus backend for the Kitty OSC 99 desktop notification protocol on Linux.
///
/// Uses the org.freedesktop.Notifications interface for:
/// - Sending notifications (Notify)
/// - Closing notifications (CloseNotification)
/// - Receiving close events (NotificationClosed signal)
/// - Receiving activation events (ActionInvoked signal)
class FreeDesktopNotifier: public QObject
{
    Q_OBJECT

  public:
    explicit FreeDesktopNotifier(QObject* parent = nullptr);
    ~FreeDesktopNotifier() override = default;

    /// Sends a desktop notification via D-Bus.
    ///
    /// @param notification the parsed OSC 99 notification data.
    void notify(vtbackend::DesktopNotification const& notification);

    /// Requests the desktop to close a notification.
    ///
    /// @param identifier the OSC 99 notification identifier.
    void close(std::string const& identifier);

  signals:
    /// Emitted when a notification is closed by the desktop environment.
    ///
    /// @param identifier the OSC 99 identifier of the closed notification.
    /// @param reason the D-Bus close reason code (1=expired, 2=dismissed, 3=closed, 4=undefined).
    void notificationClosed(QString identifier, uint reason);

    /// Emitted when the user interacts with a notification.
    ///
    /// @param identifier the OSC 99 identifier of the activated notification.
    void actionInvoked(QString identifier);

  private slots:
    /// Handles the NotificationClosed D-Bus signal.
    void onNotificationClosed(uint id, uint reason);

    /// Handles the ActionInvoked D-Bus signal.
    void onActionInvoked(uint id, QString const& actionKey);

  private:
    /// The transport-independent OSC-id ⇄ server-id bookkeeping and urgency policy.
    NotificationRouter _router;

    /// The D-Bus interface proxy for org.freedesktop.Notifications.
    std::unique_ptr<QDBusInterface> _interface;
};

} // namespace contour

#endif // defined(__linux__)
