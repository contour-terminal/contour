// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
    #include <contour/platform/QtInvoke.hpp>

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
        [this](NotificationTransport::ServerId serverId, uint32_t reason, vtbackend::CloseReport report) {
            auto const oscIdentifier = _router.takeForServerEvent(serverId);
            if (!oscIdentifier.has_value())
                return;

            notifierLog()("Notification closed: id={} reason={} report={}",
                          serverId,
                          reason,
                          report == vtbackend::CloseReport::Observed ? "observed" : "untracked");
            emit notificationClosed(QString::fromStdString(*oscIdentifier), reason, report);
        },
        [this](NotificationTransport::ServerId serverId) {
            auto const oscIdentifier = _router.takeForServerEvent(serverId);
            if (!oscIdentifier.has_value())
                return;

            notifierLog()("Notification activated: id={}", serverId);
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
    // Whether this replaces a notification of ours that is still on screen. What that means on the
    // wire is the transport's business: freedesktop's Notify carries it as replaces_id, while the
    // portal gets replacement for free from reusing the notification's id.
    auto const replacesId = _router.replacementFor(notification.identifier);

    auto const identifier = notification.identifier;
    _transport->notify(notification,
                       replacesId,
                       [this, identifier, replacesId](std::optional<NotificationRouter::ServerId> serverId) {
                           if (!serverId.has_value())
                               return;

                           notifierLog()("Notification sent: id='{}' -> {}", identifier, *serverId);
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
