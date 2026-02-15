// SPDX-License-Identifier: Apache-2.0
#if defined(__linux__)

    #include <contour/FreeDesktopNotifier.h>

    #include <crispy/logstore.h>

    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusReply>

namespace contour
{

namespace
{
    auto const notifierLog = logstore::category("gui.notifier", "Desktop notification backend");

    /// Converts NotificationUrgency to the D-Bus urgency byte value.
    uint8_t toDBusUrgency(vtbackend::NotificationUrgency urgency)
    {
        switch (urgency)
        {
            case vtbackend::NotificationUrgency::Low: return 0;
            case vtbackend::NotificationUrgency::Normal: return 1;
            case vtbackend::NotificationUrgency::Critical: return 2;
        }
        return 1;
    }
} // namespace

FreeDesktopNotifier::FreeDesktopNotifier(QObject* parent): QObject(parent)
{
    _interface = std::make_unique<QDBusInterface>("org.freedesktop.Notifications",
                                                  "/org/freedesktop/Notifications",
                                                  "org.freedesktop.Notifications",
                                                  QDBusConnection::sessionBus(),
                                                  this);

    if (!_interface->isValid())
    {
        notifierLog()("Failed to connect to org.freedesktop.Notifications D-Bus interface: {}",
                      _interface->lastError().message().toStdString());
        return;
    }

    // Connect to the NotificationClosed and ActionInvoked signals from the notification server.
    auto bus = QDBusConnection::sessionBus();
    bus.connect("org.freedesktop.Notifications",
                "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications",
                "NotificationClosed",
                this,
                SLOT(onNotificationClosed(uint, uint)));

    bus.connect("org.freedesktop.Notifications",
                "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications",
                "ActionInvoked",
                this,
                SLOT(onActionInvoked(uint, QString)));
}

void FreeDesktopNotifier::notify(vtbackend::DesktopNotification const& notification)
{
    if (!_interface || !_interface->isValid())
        return;

    auto const appName = notification.applicationName.empty()
                             ? QStringLiteral("contour")
                             : QString::fromStdString(notification.applicationName);
    auto const title = QString::fromStdString(notification.title);
    auto const body = QString::fromStdString(notification.body);

    // Build hints map with urgency level.
    QVariantMap hints;
    hints["urgency"] = QVariant::fromValue(toDBusUrgency(notification.urgency));

    // Check if we're replacing an existing notification.
    uint32_t replacesId = 0;
    if (auto it = _oscToDbus.find(notification.identifier); it != _oscToDbus.end())
        replacesId = it->second;

    // Build actions list. The default action is triggered on click.
    QStringList actions;
    actions << QStringLiteral("default") << QStringLiteral("Activate");

    // org.freedesktop.Notifications.Notify parameters:
    // STRING app_name, UINT32 replaces_id, STRING app_icon, STRING summary,
    // STRING body, ARRAY actions, DICT hints, INT32 expire_timeout
    QDBusReply<uint> reply = _interface->call("Notify",
                                              appName,
                                              replacesId,
                                              QStringLiteral(""), // app_icon (empty)
                                              title,
                                              body,
                                              actions,
                                              hints,
                                              notification.timeout);

    if (reply.isValid())
    {
        auto const dbusId = reply.value();
        notifierLog()("Notification sent: id='{}' -> dbus_id={}", notification.identifier, dbusId);

        // Update the bidirectional ID mapping.
        _dbusToOsc[dbusId] = notification.identifier;
        _oscToDbus[notification.identifier] = dbusId;
    }
    else
    {
        notifierLog()("Failed to send notification: {}", reply.error().message().toStdString());
    }
}

void FreeDesktopNotifier::close(std::string const& identifier)
{
    if (!_interface || !_interface->isValid())
        return;

    auto it = _oscToDbus.find(identifier);
    if (it == _oscToDbus.end())
        return;

    auto const dbusId = it->second;
    _interface->call("CloseNotification", dbusId);

    // Clean up mappings.
    _dbusToOsc.erase(dbusId);
    _oscToDbus.erase(it);
}

void FreeDesktopNotifier::onNotificationClosed(uint id, uint reason)
{
    auto it = _dbusToOsc.find(id);
    if (it == _dbusToOsc.end())
        return;

    auto const identifier = QString::fromStdString(it->second);
    notifierLog()("Notification closed: dbus_id={} reason={}", id, reason);

    // Clean up mappings.
    auto const oscId = it->second;
    _dbusToOsc.erase(it);
    _oscToDbus.erase(oscId);

    emit notificationClosed(identifier, reason);
}

void FreeDesktopNotifier::onActionInvoked(uint id, QString const& actionKey)
{
    (void) actionKey; // We only register "default" action.

    auto it = _dbusToOsc.find(id);
    if (it == _dbusToOsc.end())
        return;

    auto const identifier = QString::fromStdString(it->second);
    notifierLog()("Notification activated: dbus_id={}", id);

    emit actionInvoked(identifier);
}

} // namespace contour

#endif // defined(__linux__)
