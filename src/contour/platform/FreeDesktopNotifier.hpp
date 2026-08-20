// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <contour/platform/NotificationRouter.hpp>
    #include <contour/platform/NotificationTransport.hpp>
    #include <contour/platform/Notifier.hpp>

    #include <vtbackend/vt/DesktopNotification.hpp>

    #include <memory>
    #include <string>

namespace contour::platform
{

/// Backend for the Kitty OSC 99 desktop notification protocol on Linux, in the four verbs every
/// freedesktop notification service offers:
/// - Sending a notification, replacing one still on screen where the identifier repeats
/// - Withdrawing a notification
/// - Receiving close events
/// - Receiving activation events
///
/// Holds no transport of its own: what it does is decide, and the sending is a NotificationTransport
/// it is handed. That is what keeps the OSC-identifier bookkeeping testable without a D-Bus session,
/// and what lets the class guarantee it never blocks -- @see NotificationTransport.
///
/// It is deliberately ignorant of WHICH service it is talking to. org.freedesktop.Notifications and
/// org.freedesktop.portal.Notification differ in argument shape, in who assigns the notification id,
/// and in whether a close can be observed at all -- and none of those three reach this class. The
/// last of them arrives as the CloseReport on a close event rather than as a question this class
/// has to ask. @see https://github.com/contour-terminal/contour/issues/2074
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
    /// Resolves what this notification replaces and hands it to the transport, which decides what
    /// that means on the wire. Runs on this object's own thread.
    void sendNotification(vtbackend::DesktopNotification const& notification);

    /// Resolves @p identifier to a live server id and asks the transport to withdraw it, if any.
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
