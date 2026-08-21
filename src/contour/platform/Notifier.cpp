// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>

#include <format>

#ifdef __linux__
    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
    #include <contour/platform/PortalNotificationTransport.hpp>

    #include <vtpty/Process.hpp>
#endif

namespace contour::platform
{

#ifdef __linux__
namespace
{
    /// Where this process is running.
    ///
    /// vtpty::Process::isFlatpak() answers with a bool -- a signature this layer does not own -- so
    /// the conversion into the named state happens here, at the boundary, and nothing below deals
    /// in the anonymous form.
    [[nodiscard]] SandboxState currentSandboxState() noexcept
    {
        return vtpty::Process::isFlatpak() ? SandboxState::Flatpak : SandboxState::Host;
    }

} // namespace
#endif

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
    auto const backend = selectNotificationBackend(currentSandboxState());
    return std::make_unique<FreeDesktopNotifier>(makeNotificationTransport(backend, closeDelay));
#else
    return std::make_unique<NullNotifier>();
#endif
}

} // namespace contour::platform
