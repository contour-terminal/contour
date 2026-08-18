// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/NotificationTransport.hpp>

    #include <QtCore/QLatin1StringView>
    #include <QtCore/QObject>
    #include <QtCore/QVariantList>
    #include <QtCore/QVariantMap>
    #include <QtDBus/QDBusConnection>

    #include <chrono>
    #include <cstdint>
    #include <functional>
    #include <optional>
    #include <string>
    #include <unordered_map>

namespace contour::platform
{

/// Builds the options dictionary for org.freedesktop.portal.Notification.AddNotification.
///
/// A free function rather than a member, for the same reason buildFreedesktopNotifyArguments() is:
/// it is the part a headless test can check head-on, without a bus or a portal.
///
/// Only interface version 1 keys are produced (title, body, priority, default-action), so no
/// runtime version probe is needed -- the portal shipped on current desktops still advertises
/// version 1. The portal has no counterpart for freedesktop's app_name, app_icon or
/// expire_timeout: it derives the application from the sandbox's own app id, and it never expires
/// a notification on the caller's instruction.
///
/// @param notification The notification to send.
/// @return The AddNotification options dictionary.
[[nodiscard]] QVariantMap buildPortalNotificationOptions(vtbackend::DesktopNotification const& notification);

/// Runs @p action after @p delay, keeping it bound to the lifetime of @p context.
///
/// Injected rather than called directly so a test can drive the timer by hand instead of waiting:
/// the recording scheduler stores the action and the test decides when it fires, which is the same
/// "stored, not invoked" shape RecordingNotificationTransport uses for replies.
using DelayScheduler =
    std::function<void(QObject* context, std::chrono::milliseconds delay, std::function<void()> action)>;

/// The production DelayScheduler: a single-shot QTimer owned by the context object, so a transport
/// destroyed with a timer pending takes the timer with it.
[[nodiscard]] DelayScheduler qtDelayScheduler();

/// Whether a portal method call was accepted.
///
/// An enum rather than a bool because at the call site `true` says nothing about which way round the
/// question was asked, and the two states have real names. The reason for a failure is not carried:
/// nothing acts on it beyond the log line the caller already writes.
enum class CallOutcome : uint8_t
{
    Failed = 0,
    Accepted = 1,
};

/// Issues one org.freedesktop.portal.Notification method call, returning WITHOUT waiting for it.
///
/// Injected because the D-Bus call is the one ambient resource this class is built out of. Behind a
/// seam, a headless test drives the whole transport -- the id bookkeeping, replace-in-place, the
/// assumed close -- with no portal to send to; without one, the only way to reach the code that
/// runs when a call is ACCEPTED would be to send real notifications at whoever runs the suite.
///
/// @param context Whose lifetime the pending call is bound to.
/// @param method The interface method to call.
/// @param arguments Its arguments, in wire order.
/// @param onReply Called if and when a reply arrives, with whether the call was accepted. Empty
///                where the caller does not read the reply at all.
using PortalCaller = std::function<void(QObject* context,
                                        QLatin1StringView method,
                                        QVariantList arguments,
                                        std::function<void(CallOutcome)> onReply)>;

/// The production PortalCaller: an asynchronous session-bus call to the portal, whose reply watcher
/// is owned by the context object.
[[nodiscard]] PortalCaller qtPortalCaller();

/// Speaks org.freedesktop.portal.Notification, asynchronously.
///
/// The transport a sandboxed Contour must use. Inside a Flatpak sandbox the session-bus name
/// org.freedesktop.Notifications is unreachable without --talk-name, and Flathub declines to grant
/// it ("Your app should not need direct access to org.freedesktop.Notifications. There's a
/// notifications portal."); the portal needs no static permission at all.
/// @see https://github.com/contour-terminal/contour/issues/2074
///
/// Three ways it is NOT a drop-in for org.freedesktop.Notifications, and what is done about each:
///
///  1. **It assigns no id.** AddNotification takes an application-chosen string and returns
///     nothing, so the ServerId every layer above deals in is minted HERE, as a counter, and mapped
///     to the portal's string. That keeps NotificationRouter and the notifier unchanged.
///  2. **It has no NotificationClosed signal.** Nothing can report that the desktop retired a
///     popup, so a close is ASSUMED once the resolved delay elapses and reported as
///     CloseReport::Untracked -- never as an observation. A delay of zero reports nothing at all.
///  3. **Its ActionInvoked is broadcast**, and carries only (id, action, parameter) -- the app id
///     the portal matched on is dropped before the signal reaches us. A bare "1" would therefore
///     collide with another sandboxed application's notification, so portal ids are namespaced with
///     a per-process prefix.
class PortalNotificationTransport final: public QObject, public NotificationTransport
{
    Q_OBJECT

  public:
    /// @param closeDelay How long to wait before assuming a notification closed, where the
    ///                   notification itself does not say (OSC 99 `w=`). Zero never assumes.
    /// @param idPrefix   Namespace for this process's portal ids. @see the class doc, point 3.
    /// @param scheduler  How a delayed action is run; required, so a test can drive it by hand.
    /// @param caller     How a portal method call is issued; required, for the same reason.
    /// @param parent     Qt ownership, or nullptr.
    explicit PortalNotificationTransport(std::chrono::milliseconds closeDelay,
                                         std::string idPrefix,
                                         DelayScheduler scheduler,
                                         PortalCaller caller,
                                         QObject* parent = nullptr);

    /// Removes the session-bus signal subscription that subscribe() installed. Not defaulted, for
    /// the reason DBusNotificationTransport's destructor gives: one match rule per terminal session
    /// on a process-wide bus that runs its own thread.
    ~PortalNotificationTransport() override;

    void notify(vtbackend::DesktopNotification const& notification,
                ServerId replacesId,
                SentHandler onSent) override;
    void close(ServerId serverId) override;
    void subscribe(ClosedHandler onClosed, ActivatedHandler onActivated) override;

    /// The portal id a notification is sent under: the prefix, then the OSC identifier.
    ///
    /// Deriving it from the OSC identifier rather than from the minted ServerId is what gives
    /// replace-in-place for free: re-sending the same OSC identifier reuses the same portal id, and
    /// the portal's own rule is that reusing an id updates the notification already showing.
    /// @param identifier The OSC 99 notification identifier.
    /// @return The portal id.
    [[nodiscard]] std::string portalIdFor(std::string const& identifier) const;

  private slots:
    /// Handles the portal's ActionInvoked signal.
    ///
    /// All three parameters are declared even though only the id is used, because QtDBus matches a
    /// hook to an incoming signal by comparing the signature it reconstructs from the SLOT's
    /// parameters against the message's own. A slot taking fewer arguments does not fail loudly --
    /// it is simply never called.
    void onActionInvoked(QString const& identifier, QString const& action, QVariantList const& parameters);

  private:
    /// Forgets a notification, in both directions.
    /// @param serverId The id to forget.
    void retire(ServerId serverId);

    /// The session bus, held rather than fetched per call. @see DBusNotificationTransport.
    QDBusConnection _bus;

    /// How long before a close is assumed, where the notification itself does not say.
    std::chrono::milliseconds _closeDelay;

    /// This process's portal id namespace.
    std::string _idPrefix;

    /// How a delayed close is scheduled.
    DelayScheduler _schedule;

    /// How a portal method call is issued.
    PortalCaller _call;

    /// The last minted ServerId. Zero is never handed out: it is freedesktop's "this is a new
    /// notification" sentinel, and NotificationRouter::replacementFor() returns it to mean exactly
    /// that -- so a real notification must never wear it.
    ServerId _lastServerId = 0;

    /// The minted-id ⇄ portal-id mapping, kept as exact mutual inverses.
    std::unordered_map<ServerId, std::string> _portalIds;
    std::unordered_map<std::string, ServerId> _serverIds;

    /// The subscription handlers; their emptiness IS the "not subscribed yet" state.
    ClosedHandler _onClosed;
    ActivatedHandler _onActivated;
};

} // namespace contour::platform

#endif // defined(__linux__)
