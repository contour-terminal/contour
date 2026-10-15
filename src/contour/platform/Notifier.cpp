// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>

#include <QtCore/QPointer>

#include <format>
#include <utility>

#ifdef __linux__
    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
    #include <contour/platform/PortalNotificationTransport.hpp>

    #include <vtpty/SandboxInfo.hpp>
#endif

namespace contour::platform
{

void TrayNotifier::notify(vtbackend::DesktopNotification const& notification)
{
    auto const title = QString::fromStdString(notification.title);
    auto const isOwn = notification.identifier.starts_with(FinishNotificationIdPrefix);
    // A click is the notification's activation, as a desktop's would be -- unless the notifier is gone
    // by then, with the session it belonged to.
    auto onClicked = [notifier = QPointer { this },
                      identifier = QString::fromStdString(notification.identifier)] {
        if (notifier)
            emit notifier->actionInvoked(identifier);
    };
    _showMessage(isOwn ? title : tr("Application Message: %1").arg(title),
                 QString::fromStdString(notification.body),
                 std::move(onClicked));
}

std::unique_ptr<NotificationTransport> makeNotificationTransport(
    [[maybe_unused]] NotificationBackend backend, [[maybe_unused]] std::chrono::milliseconds closeDelay)
{
#ifdef __linux__
    switch (backend)
    {
        case NotificationBackend::FreeDesktop: return std::make_unique<DBusNotificationTransport>();
        case NotificationBackend::Portal:
            return std::make_unique<PortalNotificationTransport>(closeDelay,
                                                                 makePortalIdPrefix(),
                                                                 qtDelayScheduler(),
                                                                 qtPortalCaller(NotificationPortalInterface),
                                                                 sessionBusSubscriber());
    }
#endif
    return std::make_unique<NullNotificationTransport>();
}

std::unique_ptr<Notifier> makeDesktopNotifier([[maybe_unused]] std::chrono::milliseconds closeDelay)
{
#ifdef __linux__
    auto const backend = selectNotificationBackend(vtpty::currentSandbox().state);
    return std::make_unique<FreeDesktopNotifier>(makeNotificationTransport(backend, closeDelay));
#else
    return std::make_unique<NullNotifier>();
#endif
}

std::unique_ptr<Notifier> makeDesktopNotifier(
    [[maybe_unused]] std::chrono::milliseconds closeDelay,
    [[maybe_unused]] TrayNotifier::ShowMessage const& showTrayMessage)
{
#ifdef __linux__
    return makeDesktopNotifier(closeDelay);
#else
    return std::make_unique<TrayNotifier>(showTrayMessage);
#endif
}

} // namespace contour::platform
