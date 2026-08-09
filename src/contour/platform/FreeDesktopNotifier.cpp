// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
    #include <contour/platform/QtInvoke.hpp>

    #include <QtCore/QStringList>
    #include <QtCore/QVariantMap>

    #include <utility>

namespace contour::platform
{

FreeDesktopNotifier::FreeDesktopNotifier(std::unique_ptr<NotificationTransport> transport, QObject* parent):
    Notifier(parent), _transport { std::move(transport) }
{
    // Installed unconditionally, and without first asking whether a notification daemon is there:
    // subscribing is fire-and-forget, and a daemon that starts later is then simply picked up. The
    // old code probed the service synchronously and, when that probe failed, left the session unable
    // to notify for the rest of the process's life.
    _transport->subscribe(
        [this](NotificationTransport::ServerId serverId, uint32_t reason) {
            auto const oscIdentifier = _router.takeForServerEvent(serverId);
            if (!oscIdentifier.has_value())
                return;

            notifierLog()("Notification closed: dbus_id={} reason={}", serverId, reason);
            emit notificationClosed(QString::fromStdString(*oscIdentifier), reason);
        },
        [this](NotificationTransport::ServerId serverId) {
            auto const oscIdentifier = _router.takeForServerEvent(serverId);
            if (!oscIdentifier.has_value())
                return;

            notifierLog()("Notification activated: dbus_id={}", serverId);
            emit actionInvoked(QString::fromStdString(*oscIdentifier));
        });
}

void FreeDesktopNotifier::notify(vtbackend::DesktopNotification const& notification)
{
    // ALWAYS posts, never sends inline -- the same rule QtAnnouncer::announce follows, for the same
    // call sites. This runs on the TERMINAL thread with the terminal's non-recursive state mutex
    // held, so any wait here stalls that session's PTY drain; and _router must be reached from one
    // thread only, or it races the close/activation events arriving on the GUI thread.
    postToObject(this, [this, notification] { sendNotification(notification); });
}

void FreeDesktopNotifier::close(std::string const& identifier)
{
    postToObject(this, [this, identifier] { sendClose(identifier); });
}

void FreeDesktopNotifier::sendNotification(vtbackend::DesktopNotification const& notification)
{
    auto const appName = notification.applicationName.empty()
                             ? QStringLiteral("contour")
                             : QString::fromStdString(notification.applicationName);

    auto hints = QVariantMap {};
    hints["urgency"] = QVariant::fromValue(NotificationRouter::toFreedesktopUrgency(notification.urgency));

    // The default action is what a click on the popup triggers.
    auto const actions = QStringList { QStringLiteral("default"), QStringLiteral("Activate") };

    // Whether this replaces a notification of ours that is still on screen.
    auto const replacesId = _router.replacementFor(notification.identifier);

    // org.freedesktop.Notifications.Notify, signature susssasa{sv}i:
    // STRING app_name, UINT32 replaces_id, STRING app_icon, STRING summary,
    // STRING body, ARRAY actions, DICT hints, INT32 expire_timeout.
    // The casts are load-bearing: QDBusInterface::call used to deduce the wire types from the C++
    // argument types, and a QVariant built from the wrong integer type marshals as the wrong D-Bus
    // type, which the notification server rejects outright.
    auto arguments = QVariantList {
        appName,
        QVariant::fromValue(static_cast<quint32>(replacesId)),
        QStringLiteral(""), // app_icon (empty)
        QString::fromStdString(notification.title),
        QString::fromStdString(notification.body),
        actions,
        hints,
        QVariant::fromValue(static_cast<int>(notification.timeout)),
    };

    auto const identifier = notification.identifier;
    _transport->notify(std::move(arguments),
                       [this, identifier, replacesId](std::optional<NotificationRouter::ServerId> serverId) {
                           if (!serverId.has_value())
                               return;

                           notifierLog()("Notification sent: id='{}' -> dbus_id={}", identifier, *serverId);
                           _router.onSent(identifier, *serverId, replacesId);
                       });
}

void FreeDesktopNotifier::sendClose(std::string const& identifier)
{
    auto const serverId = _router.takeForClose(identifier);
    if (!serverId.has_value())
        return;

    _transport->close(*serverId);
}

} // namespace contour::platform

#endif // defined(__linux__)
