// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NotificationRouter.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace contour::platform
{

/// The desktop-notification method-call transport a FreeDesktopNotifier speaks.
///
/// An interface for the reason Announcer gives: the DECISIONS -- which method is sent with which
/// arguments, and how a reply feeds the identifier bookkeeping -- are the part worth testing, and a
/// headless test has no notification daemon to observe the delivery through.
///
/// Two implementations exist and they do NOT speak the same protocol: DBusNotificationTransport
/// talks to org.freedesktop.Notifications on the session bus, PortalNotificationTransport to
/// org.freedesktop.portal.Notification (the only one a Flatpak sandbox can reach). This interface
/// is therefore stated in the DOMAIN type, not in either wire format -- each implementation builds
/// its own arguments, and neither has to read a shape it does not speak. @see issue #2074.
///
/// It also pins down the property this seam exists to protect: **no implementation may wait for a
/// reply**. A desktop that is not answering must cost nothing, because these calls are made while a
/// terminal session is being constructed and again on every bell. @see issue #2051.
class NotificationTransport
{
  public:
    using ServerId = NotificationRouter::ServerId;

    /// Invoked with the notification id assigned to the sent notification, or nullopt if the call
    /// failed. The id is the notification server's where the server assigns one, and the
    /// transport's own where it does not.
    using SentHandler = std::function<void(std::optional<ServerId>)>;

    /// Invoked when a notification was closed. @param reason 1=expired, 2=dismissed, 3=closed on
    /// request, 4=undefined. @param report Whether the close was observed or merely assumed.
    using ClosedHandler = std::function<void(ServerId, uint32_t reason, vtbackend::CloseReport report)>;

    /// Invoked when the user activated a notification.
    using ActivatedHandler = std::function<void(ServerId)>;

    NotificationTransport() = default;
    NotificationTransport(NotificationTransport const&) = delete;
    NotificationTransport& operator=(NotificationTransport const&) = delete;
    NotificationTransport(NotificationTransport&&) = delete;
    NotificationTransport& operator=(NotificationTransport&&) = delete;
    virtual ~NotificationTransport() = default;

    /// Sends the notification, returning WITHOUT waiting for its reply.
    ///
    /// @param notification What to show.
    /// @param replacesId   A live id this notification supersedes, or 0 when it is a fresh one.
    /// @param onSent       Called later, on the caller's thread, with the assigned id.
    virtual void notify(vtbackend::DesktopNotification const& notification,
                        ServerId replacesId,
                        SentHandler onSent) = 0;

    /// Withdraws a notification, returning WITHOUT waiting for its reply.
    /// @param serverId The id this notification was sent under.
    virtual void close(ServerId serverId) = 0;

    /// Subscribes to the backend's close and activation events. Called once, by the notifier's
    /// constructor, and undone when the transport is destroyed.
    virtual void subscribe(ClosedHandler onClosed, ActivatedHandler onActivated) = 0;
};

/// A NotificationTransport that sends nothing and never reports an event.
///
/// The default wherever no session bus exists, so a notifier never has to ask whether notifying is
/// possible -- the same role NullAnnouncer plays for announcements.
class NullNotificationTransport final: public NotificationTransport
{
  public:
    void notify(vtbackend::DesktopNotification const& /*notification*/,
                ServerId /*replacesId*/,
                SentHandler /*onSent*/) override
    {
    }
    void close(ServerId /*serverId*/) override {}
    void subscribe(ClosedHandler /*onClosed*/, ActivatedHandler /*onActivated*/) override {}
};

} // namespace contour::platform
