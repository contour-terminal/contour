// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/NotificationRouter.hpp>
#include <contour/platform/UserNotificationNotifier.hpp>

#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <utility>

namespace contour::platform
{

namespace
{
    /// The D-Bus close reason for a notification that expired, which is what an assumed close reports.
    constexpr auto ExpiredReason = uint { 1 };
} // namespace

UserNotificationNotifier::UserNotificationNotifier(IUserNotificationCenter& center,
                                                   std::string prefix,
                                                   std::chrono::milliseconds closeDelay,
                                                   std::unique_ptr<Notifier> fallback,
                                                   QObject* parent):
    Notifier(parent),
    _center { center },
    _prefix { std::move(prefix) },
    _closeDelay { closeDelay },
    _fallback { std::move(fallback) },
    // Through a QPointer: a click may arrive after this notifier is gone, with the session it belonged to.
    _subscription { center.subscribe(
        _prefix,
        [self = QPointer { this }](std::string_view identifier) {
            if (self)
                emit self->actionInvoked(
                    QString::fromUtf8(identifier.data(), static_cast<qsizetype>(identifier.size())));
        },
        [self = QPointer { this }](UserNotificationRequest const& request) {
            if (self)
                self->raiseThroughFallback(request);
        }) }
{
    if (!_fallback)
        return;
    connect(_fallback.get(), &Notifier::actionInvoked, this, &Notifier::actionInvoked);
    connect(_fallback.get(), &Notifier::notificationClosed, this, &Notifier::notificationClosed);
}

UserNotificationNotifier::~UserNotificationNotifier()
{
    for (auto const& identifier: _posted)
        _center.withdraw(_prefix + identifier);
}

void UserNotificationNotifier::raiseThroughFallback(UserNotificationRequest const& request)
{
    auto const identifier = request.requestId.substr(_prefix.size());
    _posted.erase(identifier);
    // The fallback now owns the notice, so it reports the close the application asked for, not this timer.
    auto const closeRequested = _timed.erase(identifier) != 0;
    if (!_fallback)
        return;

    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = identifier;
    notification.title = request.sourceTitle;
    notification.body = request.body;
    notification.sound = request.sound;
    notification.closeEventRequested = closeRequested;
    _fallback->notify(notification);
}

void UserNotificationNotifier::notify(vtbackend::DesktopNotification const& notification)
{
    if (_center.availability() == UserNotificationAvailability::Unavailable)
    {
        if (_fallback)
            _fallback->notify(notification);
        return;
    }

    auto const isOwn = notification.identifier.starts_with(FinishNotificationIdPrefix);
    auto const title = QString::fromStdString(notification.title);

    _center.post(UserNotificationRequest {
        .requestId = _prefix + notification.identifier,
        .title = (isOwn ? title : tr("Application Message: %1").arg(title)).toStdString(),
        .sourceTitle = notification.title,
        .body = notification.body,
        .sound = notification.sound,
        .threadId = _prefix,
    });

    auto const posting = ++_postings;
    _posted.insert(notification.identifier);
    // A new posting retires the timer of the one it replaces, whether or not it arms its own.
    _timed.erase(notification.identifier);

    // The centre cannot say when the user dismisses a notification, so a close the application asked
    // to hear of is assumed after a while, and reported as the assumption it is.
    auto const delay = NotificationRouter::assumedCloseDelay(notification, _closeDelay);
    if (delay == std::chrono::milliseconds { 0 })
        return;
    _timed.insert_or_assign(notification.identifier, posting);

    QTimer::singleShot(delay, this, [this, identifier = notification.identifier, posting] {
        auto const timed = _timed.find(identifier);
        // Withdrawn or replaced since: either retires this posting, which is how the timer cancels.
        if (timed == _timed.end() || timed->second != posting)
            return;
        // Only the timer is spent: the notification may still be on screen, and the id stays for withdrawal.
        _timed.erase(timed);
        emit notificationClosed(
            QString::fromStdString(identifier), ExpiredReason, vtbackend::CloseReport::Untracked);
    });
}

void UserNotificationNotifier::close(std::string const& identifier)
{
    if (_fallback)
        _fallback->close(identifier);
    _timed.erase(identifier);
    if (_posted.erase(identifier) == 0)
        return;
    _center.withdraw(_prefix + identifier);
}

} // namespace contour::platform
