// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/NotificationRouter.hpp>
    #include <contour/platform/NotificationTransport.hpp>
    #include <contour/platform/Notifier.hpp>

    #include <vtbackend/DesktopNotification.hpp>

    #include <memory>
    #include <string>

namespace contour::platform
{

/// Backend for the Kitty OSC 99 desktop notification protocol on Linux, speaking the
/// org.freedesktop.Notifications vocabulary:
/// - Sending notifications (Notify)
/// - Closing notifications (CloseNotification)
/// - Receiving close events (NotificationClosed signal)
/// - Receiving activation events (ActionInvoked signal)
///
/// Holds no transport of its own: what it does is decide, and the sending is a NotificationTransport
/// it is handed. That is what keeps the OSC-identifier bookkeeping testable without a D-Bus session,
/// and what lets the class guarantee it never blocks -- @see NotificationTransport.
class FreeDesktopNotifier final: public Notifier
{
    Q_OBJECT

  public:
    /// @param transport How notifications are sent; required, and owned from here on.
    /// @param parent    The usual QObject parent.
    explicit FreeDesktopNotifier(std::unique_ptr<NotificationTransport> transport, QObject* parent = nullptr);

    void notify(vtbackend::DesktopNotification const& notification) override;
    void close(std::string const& identifier) override;

  private:
    /// Builds and dispatches the freedesktop Notify call. Runs on this object's own thread.
    void sendNotification(vtbackend::DesktopNotification const& notification);

    /// Resolves @p identifier to a live server id and dispatches CloseNotification for it, if any.
    void sendClose(std::string const& identifier);

    /// The transport-independent OSC-id <-> server-id bookkeeping and urgency policy. Only ever
    /// touched on this object's thread, which is the point of routing every entry point through
    /// postToObject: it used to be reached from the terminal thread and the GUI thread at once.
    NotificationRouter _router;

    /// Declared last, so it is destroyed FIRST. A call still in flight holds a handler that reaches
    /// back into _router, and destroying the transport is what cancels it.
    std::unique_ptr<NotificationTransport> _transport;
};

} // namespace contour::platform

#endif // defined(__linux__)
