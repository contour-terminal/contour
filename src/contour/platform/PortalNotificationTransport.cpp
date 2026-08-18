// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/DBusSignalSubscription.hpp>
    #include <contour/platform/PortalNotificationTransport.hpp>

    #include <QtCore/QTimer>
    #include <QtDBus/QDBusMessage>
    #include <QtDBus/QDBusPendingCallWatcher>
    #include <QtDBus/QDBusPendingReply>

    #include <array>
    #include <format>
    #include <utility>

namespace contour::platform
{

namespace
{
    // The portal is always at this one name, path and interface -- it is the sandbox's single
    // entry point, so there is nothing to discover.
    constexpr auto PortalSource = DBusSignalSource {
        .service = QLatin1StringView("org.freedesktop.portal.Desktop"),
        .path = QLatin1StringView("/org/freedesktop/portal/desktop"),
        .interface = QLatin1StringView("org.freedesktop.portal.Notification"),
    };

    // Below Qt's 25-second default, for the reason DBusNotificationTransport gives: losing an
    // AddNotification reply costs only this notification's bookkeeping.
    constexpr auto CallTimeoutMilliseconds = 5000;

    // The freedesktop close reason reported for an assumed close. There is no "we guessed" reason
    // in that enumeration, and "expired" is the closest true statement: the time we waited is up.
    // What actually distinguishes it is the CloseReport carried alongside.
    constexpr auto ExpiredReason = uint32_t { 1 };

    // The action name registered as the notification's default -- what a click on the popup
    // triggers. The absence of an "app." prefix is the whole mechanism and NOT an oversight: the
    // portal treats an "app."-prefixed action as exported and D-Bus-activates it via
    // org.freedesktop.Application.ActivateAction, delivering nothing to us, while any other name
    // takes the branch that emits ActionInvoked. @see xdg-desktop-portal-gtk's activate_action().
    constexpr auto DefaultActionName = "default";

    // A function rather than a constant because SLOT() expands to qFlagLocation(), which is neither
    // constexpr nor safe to run before main().
    [[nodiscard]] std::array<DBusSignalSubscription, 1> signalSubscriptions()
    {
        return {
            DBusSignalSubscription { QLatin1StringView("ActionInvoked"),
                                     SLOT(onActionInvoked(QString, QString, QVariantList)) },
        };
    }

} // namespace

QVariantMap buildPortalNotificationOptions(vtbackend::DesktopNotification const& notification)
{
    auto const priority = NotificationRouter::toPortalPriority(notification.urgency);

    auto options = QVariantMap {};

    options["title"] = QString::fromStdString(notification.title);
    options["body"] = QString::fromStdString(notification.body);
    options["priority"] = QString::fromLatin1(priority.data(), static_cast<qsizetype>(priority.size()));
    options["default-action"] = QString::fromLatin1(DefaultActionName);

    return options;
}

DelayScheduler qtDelayScheduler()
{
    return [](QObject* context, std::chrono::milliseconds delay, std::function<void()> action) {
        // Bound to the context object, so a transport destroyed with a timer pending takes the
        // timer with it and the action -- which reaches back into that transport -- never runs.
        QTimer::singleShot(delay, context, std::move(action));
    };
}

PortalCaller qtPortalCaller()
{
    return [bus = QDBusConnection::sessionBus()](QObject* context,
                                                 QLatin1StringView method,
                                                 QVariantList arguments,
                                                 std::function<void(CallOutcome)> onReply) {
        auto message = QDBusMessage::createMethodCall(
            PortalSource.service, PortalSource.path, PortalSource.interface, method);
        message.setArguments(std::move(arguments));

        auto pending = bus.asyncCall(message, CallTimeoutMilliseconds);
        if (!onReply)
            return; // Sent and forgotten: nothing in this reply is acted on.

        // Parented to the context, so a transport destroyed with a call in flight takes its watcher
        // with it and the handler -- which reaches back into that transport -- is never run.
        auto* const watcher = new QDBusPendingCallWatcher(pending, context);
        QObject::connect(
            watcher, &QDBusPendingCallWatcher::finished, context, [onReply = std::move(onReply)](auto* self) {
                // AddNotification returns nothing, so there is no value to read -- only whether it
                // arrived at all.
                auto const reply = QDBusPendingReply<>(*self);
                self->deleteLater();

                if (reply.isError())
                    notifierLog()("Portal notification call failed: {}",
                                  reply.error().message().toStdString());

                onReply(reply.isError() ? CallOutcome::Failed : CallOutcome::Accepted);
            });
    };
}

PortalNotificationTransport::PortalNotificationTransport(std::chrono::milliseconds closeDelay,
                                                         std::string idPrefix,
                                                         DelayScheduler scheduler,
                                                         PortalCaller caller,
                                                         QObject* parent):
    QObject(parent),
    _bus { QDBusConnection::sessionBus() },
    _closeDelay { closeDelay },
    _idPrefix { std::move(idPrefix) },
    _schedule { std::move(scheduler) },
    _call { std::move(caller) }
{
}

PortalNotificationTransport::~PortalNotificationTransport()
{
    if (_subscription == DBusSubscriptionState::NotSubscribed)
        return;

    unsubscribeFromDBusSignals(_bus, PortalSource, this, signalSubscriptions());
}

std::string PortalNotificationTransport::portalIdFor(std::string const& identifier) const
{
    return std::format("{}/{}", _idPrefix, identifier);
}

void PortalNotificationTransport::notify(vtbackend::DesktopNotification const& notification,
                                         ServerId replacesId,
                                         SentHandler onSent)
{
    auto const portalId = portalIdFor(notification.identifier);
    auto const serverId = ++_lastServerId;
    auto const closeDelay = NotificationRouter::resolveCloseDelay(notification.timeout, _closeDelay);

    _call(
        this,
        QLatin1StringView("AddNotification"),
        { QString::fromStdString(portalId), buildPortalNotificationOptions(notification) },
        [this, portalId, serverId, replacesId, closeDelay, onSent = std::move(onSent)](CallOutcome outcome) {
            if (outcome != CallOutcome::Accepted)
            {
                onSent(std::nullopt);
                return;
            }

            // Recorded only now, and only on success -- exactly when NotificationRouter records
            // its own mapping, so a failed send leaves the previous notification still live and
            // still replaceable.
            if (replacesId != 0)
                retire(replacesId);
            _portalIds[serverId] = portalId;
            _serverIds[portalId] = serverId;

            notifierLog()("Notification sent: portal_id='{}' -> id={}", portalId, serverId);
            onSent(serverId);

            if (closeDelay == std::chrono::milliseconds { 0 })
                return;

            _schedule(this, closeDelay, [this, serverId] {
                // The notification may have been withdrawn, superseded or activated while the
                // timer ran; all three retire it, so finding it gone is how the timer cancels.
                if (!_portalIds.contains(serverId))
                    return;

                retire(serverId);
                if (_onClosed)
                    _onClosed(serverId, ExpiredReason, vtbackend::CloseReport::Untracked);
            });
        });
}

void PortalNotificationTransport::close(ServerId serverId)
{
    auto const it = _portalIds.find(serverId);
    if (it == _portalIds.end())
        return; // Never sent, or already retired: nothing to withdraw and no traffic to make.

    auto const portalId = it->second;
    retire(serverId);

    // No reply handler: nothing in RemoveNotification's reply is acted on.
    _call(this, QLatin1StringView("RemoveNotification"), { QString::fromStdString(portalId) }, {});
}

void PortalNotificationTransport::subscribe(ClosedHandler onClosed, ActivatedHandler onActivated)
{
    _onClosed = std::move(onClosed);
    _onActivated = std::move(onActivated);

    // Only the activation signal exists to subscribe to; the portal has no close signal at all, so
    // _onClosed is driven by the timer instead.
    subscribeToDBusSignals(_bus, PortalSource, this, signalSubscriptions());
    _subscription = DBusSubscriptionState::Subscribed;
}

void PortalNotificationTransport::onActionInvoked(QString const& identifier,
                                                  QString const& action,
                                                  QVariantList const& parameters)
{
    (void) action;     // We only register the "default" action.
    (void) parameters; // Carries the action target and the activation token; neither is used.

    auto const it = _serverIds.find(identifier.toStdString());
    if (it == _serverIds.end())
        return; // Another application's notification: this signal is broadcast.

    auto const serverId = it->second;
    retire(serverId);

    if (_onActivated)
        _onActivated(serverId);
}

void PortalNotificationTransport::retire(ServerId serverId)
{
    auto const it = _portalIds.find(serverId);
    if (it == _portalIds.end())
        return;

    _serverIds.erase(it->second);
    _portalIds.erase(it);
}

} // namespace contour::platform

#endif // defined(__linux__)
