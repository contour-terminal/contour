// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>

#include <format>

#ifdef __linux__
    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
    #include <contour/platform/PortalNotificationTransport.hpp>

    #include <vtpty/SandboxInfo.hpp>
#endif

namespace contour::platform
{

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
                                                                 qtPortalCaller(NotificationPortalInterface));
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

} // namespace contour::platform
