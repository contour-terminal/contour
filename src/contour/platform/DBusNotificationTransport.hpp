// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/DBusSignalSubscription.hpp>
    #include <contour/platform/NotificationTransport.hpp>

    #include <QtCore/QObject>
    #include <QtCore/QVariantList>
    #include <QtDBus/QDBusConnection>

namespace contour::platform
{

/// Builds the positional argument tuple for org.freedesktop.Notifications.Notify.
///
/// A free function rather than a member, because it is the one part of this transport a headless
/// test can check head-on: no bus, no daemon, no waiting -- just the notification in and the wire
/// tuple out.
///
/// @param notification The notification to send.
/// @param replacesId A live server id this supersedes, or 0 for a fresh notification.
/// @return The eight Notify arguments, in wire order.
[[nodiscard]] QVariantList buildFreedesktopNotifyArguments(vtbackend::DesktopNotification const& notification,
                                                           NotificationTransport::ServerId replacesId);

/// Speaks org.freedesktop.Notifications over the session bus, asynchronously.
///
/// Deliberately NOT a QDBusInterface. That class introspects the remote object from its
/// CONSTRUCTOR, synchronously, at D-Bus's default 25-second reply timeout -- and a desktop whose
/// notification daemon is installed but not running is precisely the case that pays that timeout in
/// full, because the bus attempts to activate the service and nothing ever answers. Contour builds
/// one notifier per terminal session, so the cost landed on the startup path, once per tab, before
/// any window was mapped. @see https://github.com/contour-terminal/contour/issues/2051
///
/// Nothing here needs what the introspection buys: every call names its method as a string, which
/// QDBusAbstractInterface forwards verbatim. So the messages are built directly and sent with
/// QDBusConnection::asyncCall, and a watcher collects whatever reply eventually arrives.
class DBusNotificationTransport final: public QObject, public NotificationTransport
{
    Q_OBJECT

  public:
    explicit DBusNotificationTransport(QObject* parent = nullptr);

    /// Removes the session-bus signal subscriptions that subscribe() installed.
    ///
    /// Not defaulted: QDBusConnection::connect() registers a match rule on the process-wide session
    /// bus, and there is one transport per terminal session. Without the matching disconnect the
    /// rules accumulate for the life of the process, and the bus keeps delivering those signals
    /// towards an object that is being destroyed -- the bus runs its own thread, so that is a
    /// teardown race and not merely a leak.
    ~DBusNotificationTransport() override;

    void notify(vtbackend::DesktopNotification const& notification,
                ServerId replacesId,
                SentHandler onSent) override;
    void close(ServerId serverId) override;
    void subscribe(ClosedHandler onClosed, ActivatedHandler onActivated) override;

  private slots:
    /// Handles the NotificationClosed D-Bus signal.
    void onNotificationClosed(uint id, uint reason);

    /// Handles the ActionInvoked D-Bus signal.
    void onActionInvoked(uint id, QString const& actionKey);

  private:
    /// The session bus. Held rather than fetched per call so the connection name is resolved once;
    /// obtaining it costs a Hello round-trip to the bus daemon, which always answers at once.
    QDBusConnection _bus;

    /// Whether subscribe() has installed the match rules, so the destructor knows to remove them.
    DBusSubscriptionState _subscription = DBusSubscriptionState::NotSubscribed;

    /// The subscription handlers.
    ClosedHandler _onClosed;
    ActivatedHandler _onActivated;
};

} // namespace contour::platform

#endif // defined(__linux__)
