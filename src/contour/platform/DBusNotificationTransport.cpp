// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/DBusNotificationTransport.hpp>

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
    constexpr auto NotificationsService = QLatin1StringView("org.freedesktop.Notifications");
    constexpr auto NotificationsPath = QLatin1StringView("/org/freedesktop/Notifications");
    constexpr auto NotificationsInterface = QLatin1StringView("org.freedesktop.Notifications");

    /// How long a call may stay outstanding before D-Bus reports NoReply.
    ///
    /// Qt's default is 25 seconds, which is a long time to hold a watcher open against a desktop
    /// that is not answering. Nothing here needs its reply urgently: losing a Notify reply costs
    /// only the replace-in-place bookkeeping for that one notification, and CloseNotification's
    /// reply is not read at all.
    constexpr auto CallTimeoutMilliseconds = 5000;

    /// A server signal, and the slot it is delivered to.
    struct SignalSubscription
    {
        QLatin1StringView name;
        char const* slot = nullptr;
    };

    /// The signals this transport listens for.
    ///
    /// A table rather than four hand-written calls, because subscribe() and the destructor must pass
    /// D-Bus byte-for-byte identical arguments or the match rule is added in one shape and looked for
    /// in another -- which fails silently and leaks the rule for the life of the process. Walking one
    /// table makes that structural instead of something two code blocks have to agree about.
    ///
    /// A function rather than a constant, because SLOT() expands to a qFlagLocation() call: it is
    /// neither constexpr nor safe to run before main().
    [[nodiscard]] std::array<SignalSubscription, 2> signalSubscriptions()
    {
        return {
            SignalSubscription { QLatin1StringView("NotificationClosed"),
                                 SLOT(onNotificationClosed(uint, uint)) },
            SignalSubscription { QLatin1StringView("ActionInvoked"), SLOT(onActionInvoked(uint, QString)) },
        };
    }

    /// The one method-call shape: same service, same object, same interface, every time.
    [[nodiscard]] QDBusMessage notificationCall(QLatin1StringView method)
    {
        return QDBusMessage::createMethodCall(
            NotificationsService, NotificationsPath, NotificationsInterface, method);
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

    // Mirror image of subscribe(). @see the declaration for why this cannot be defaulted.
    for (auto const& subscription: signalSubscriptions())
        _bus.disconnect(NotificationsService,
                        NotificationsPath,
                        NotificationsInterface,
                        subscription.name,
                        this,
                        subscription.slot);
}

void DBusNotificationTransport::notify(QVariantList arguments, SentHandler onSent)
{
    auto message = notificationCall(QLatin1StringView("Notify"));
    message.setArguments(arguments);

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

    // Registering a match rule does not wait for the bus: Qt hands AddMatch a null error pointer,
    // making it fire-and-forget. Installed unconditionally, with no check that anyone is listening
    // for us to talk to -- the notification daemon may well start after this session did.
    for (auto const& subscription: signalSubscriptions())
        _bus.connect(NotificationsService,
                     NotificationsPath,
                     NotificationsInterface,
                     subscription.name,
                     this,
                     subscription.slot);
}

void DBusNotificationTransport::onNotificationClosed(uint id, uint reason)
{
    if (_onClosed)
        _onClosed(id, reason);
}

void DBusNotificationTransport::onActionInvoked(uint id, QString const& actionKey)
{
    (void) actionKey; // We only register the "default" action.

    if (_onActivated)
        _onActivated(id);
}

} // namespace contour::platform

#endif // defined(__linux__)
