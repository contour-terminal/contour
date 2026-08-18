// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/DBusSignalSubscription.hpp>

    #include <QtCore/QStringList>
    #include <QtCore/QVariantMap>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusPendingCallWatcher>
    #include <QtDBus/QDBusPendingReply>

    #include <array>
    #include <utility>

namespace contour::platform
{

namespace
{
    // The one string trio every call and every match rule is addressed with.
    constexpr auto NotificationsSource = DBusSignalSource {
        .service = QLatin1StringView("org.freedesktop.Notifications"),
        .path = QLatin1StringView("/org/freedesktop/Notifications"),
        .interface = QLatin1StringView("org.freedesktop.Notifications"),
    };

    // Deliberately below Qt's 25-second default: losing a Notify reply costs only the
    // replace-in-place bookkeeping for that one notification, and CloseNotification's reply is not
    // read at all. A desktop that is not answering must not be able to hold anything of ours open.
    constexpr auto CallTimeoutMilliseconds = 5000;

    // A function rather than a constant because SLOT() expands to qFlagLocation(), which is neither
    // constexpr nor safe to run before main().
    [[nodiscard]] std::array<DBusSignalSubscription, 2> signalSubscriptions()
    {
        return {
            DBusSignalSubscription { QLatin1StringView("NotificationClosed"),
                                     SLOT(onNotificationClosed(uint, uint)) },
            DBusSignalSubscription { QLatin1StringView("ActionInvoked"),
                                     SLOT(onActionInvoked(uint, QString)) },
        };
    }

    [[nodiscard]] QDBusMessage notificationCall(QLatin1StringView method)
    {
        return QDBusMessage::createMethodCall(
            NotificationsSource.service, NotificationsSource.path, NotificationsSource.interface, method);
    }
} // namespace

DBusNotificationTransport::DBusNotificationTransport(QObject* parent):
    QObject(parent), _bus { QDBusConnection::sessionBus() }
{
}

DBusNotificationTransport::~DBusNotificationTransport()
{
    if (!_onClosed)
        return;

    unsubscribeFromDBusSignals(_bus, NotificationsSource, this, signalSubscriptions());
}

QVariantList buildFreedesktopNotifyArguments(vtbackend::DesktopNotification const& notification,
                                             NotificationTransport::ServerId replacesId)
{
    auto const appName = notification.applicationName.empty()
                             ? QStringLiteral("contour")
                             : QString::fromStdString(notification.applicationName);

    auto hints = QVariantMap {};
    hints["urgency"] = QVariant::fromValue(NotificationRouter::toFreedesktopUrgency(notification.urgency));

    // The default action is what a click on the popup triggers.
    auto const actions = QStringList { QStringLiteral("default"), QStringLiteral("Activate") };

    // org.freedesktop.Notifications.Notify, signature susssasa{sv}i:
    // STRING app_name, UINT32 replaces_id, STRING app_icon, STRING summary,
    // STRING body, ARRAY actions, DICT hints, INT32 expire_timeout.
    // The casts are load-bearing: QDBusInterface::call used to deduce the wire types from the C++
    // argument types, and a QVariant built from the wrong integer type marshals as the wrong D-Bus
    // type, which the notification server rejects outright.
    return QVariantList {
        appName,
        QVariant::fromValue(static_cast<quint32>(replacesId)),
        QStringLiteral(""), // app_icon (empty)
        QString::fromStdString(notification.title),
        QString::fromStdString(notification.body),
        actions,
        hints,
        QVariant::fromValue(static_cast<int>(notification.timeout)),
    };
}

void DBusNotificationTransport::notify(vtbackend::DesktopNotification const& notification,
                                       ServerId replacesId,
                                       SentHandler onSent)
{
    auto message = notificationCall(QLatin1StringView("Notify"));
    message.setArguments(buildFreedesktopNotifyArguments(notification, replacesId));

    // Parented to this, so a transport destroyed with a call in flight takes its watcher with it and
    // the handler -- which reaches back into the notifier that owns us -- is simply never run.
    auto* const watcher = new QDBusPendingCallWatcher(_bus.asyncCall(message, CallTimeoutMilliseconds), this);

    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, this, [onSent = std::move(onSent)](auto* self) {
            auto const reply = QDBusPendingReply<uint>(*self);
            self->deleteLater();

            if (reply.isError())
            {
                notifierLog()("Failed to send notification: {}", reply.error().message().toStdString());
                onSent(std::nullopt);
                return;
            }

            onSent(reply.value());
        });
}

void DBusNotificationTransport::close(ServerId serverId)
{
    auto message = notificationCall(QLatin1StringView("CloseNotification"));
    message.setArguments({ QVariant::fromValue(static_cast<quint32>(serverId)) });

    // Sent and forgotten: there is nothing in the reply the caller acts on, and waiting for it was
    // what made closing a notification able to stall the terminal thread.
    _bus.asyncCall(message, CallTimeoutMilliseconds);
}

void DBusNotificationTransport::subscribe(ClosedHandler onClosed, ActivatedHandler onActivated)
{
    _onClosed = std::move(onClosed);
    _onActivated = std::move(onActivated);

    subscribeToDBusSignals(_bus, NotificationsSource, this, signalSubscriptions());
}

void DBusNotificationTransport::onNotificationClosed(uint id, uint reason)
{
    // Observed: the notification daemon is telling us this happened, so the close report Contour
    // sends back over the PTY is a fact rather than an assumption.
    if (_onClosed)
        _onClosed(id, reason, vtbackend::CloseReport::Observed);
}

void DBusNotificationTransport::onActionInvoked(uint id, QString const& actionKey)
{
    (void) actionKey; // We only register the "default" action.

    if (_onActivated)
        _onActivated(id);
}

} // namespace contour::platform

#endif // defined(__linux__)
