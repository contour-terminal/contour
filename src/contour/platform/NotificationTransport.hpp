// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NotificationRouter.hpp>

#include <QtCore/QVariantList>

#include <cstdint>
#include <functional>
#include <optional>

namespace contour::platform
{

/// The org.freedesktop.Notifications method-call transport a FreeDesktopNotifier speaks.
///
/// An interface for the reason Announcer gives: the DECISIONS -- which method is sent with which
/// arguments, and how a reply feeds the identifier bookkeeping -- are the part worth testing, and a
/// headless test has no notification daemon to observe the delivery through.
///
/// It also pins down the property this seam exists to protect: **no implementation may wait for a
/// reply**. A desktop that is not answering must cost nothing, because these calls are made while a
/// terminal session is being constructed and again on every bell. @see issue #2051.
class NotificationTransport
{
  public:
    using ServerId = NotificationRouter::ServerId;

    /// Invoked with the server-assigned notification id, or nullopt if the call failed.
    using SentHandler = std::function<void(std::optional<ServerId>)>;

    /// Invoked when the desktop closed a notification. @param reason 1=expired, 2=dismissed,
    /// 3=closed on request, 4=undefined.
    using ClosedHandler = std::function<void(ServerId, uint32_t reason)>;

    /// Invoked when the user activated a notification.
    using ActivatedHandler = std::function<void(ServerId)>;

    NotificationTransport() = default;
    NotificationTransport(NotificationTransport const&) = delete;
    NotificationTransport& operator=(NotificationTransport const&) = delete;
    NotificationTransport(NotificationTransport&&) = delete;
    NotificationTransport& operator=(NotificationTransport&&) = delete;
    virtual ~NotificationTransport() = default;

    /// Sends `Notify`, returning WITHOUT waiting for its reply.
    ///
    /// @param arguments The freedesktop Notify arguments, in wire order.
    /// @param onSent    Called later, on the caller's thread, with the assigned id.
    virtual void notify(QVariantList arguments, SentHandler onSent) = 0;

    /// Sends `CloseNotification`, returning WITHOUT waiting for its reply.
    /// @param serverId The id the notification server assigned.
    virtual void close(ServerId serverId) = 0;

    /// Subscribes to the server's NotificationClosed and ActionInvoked signals. Called once, by the
    /// notifier's constructor, and undone when the transport is destroyed.
    virtual void subscribe(ClosedHandler onClosed, ActivatedHandler onActivated) = 0;
};

/// A NotificationTransport that sends nothing and never reports an event.
///
/// The default wherever no session bus exists, so a notifier never has to ask whether notifying is
/// possible -- the same role NullAnnouncer plays for announcements.
class NullNotificationTransport final: public NotificationTransport
{
  public:
    void notify(QVariantList /*arguments*/, SentHandler /*onSent*/) override {}
    void close(ServerId /*serverId*/) override {}
    void subscribe(ClosedHandler /*onClosed*/, ActivatedHandler /*onActivated*/) override {}
};

} // namespace contour::platform
