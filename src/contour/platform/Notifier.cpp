// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NotificationTransport.hpp>
#include <contour/platform/Notifier.hpp>

#include <QtCore/QUuid>

#include <atomic>
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

std::string makePortalIdPrefix()
{
    // A random per-process component rather than the process id, because the process id is exactly
    // what a sandbox takes away: Flatpak runs every instance in its own PID namespace, so two
    // Contour instances -- the only case where a collision between processes is possible at all --
    // both report a small, identical pid. A UUID is unique wherever a pid is, and stays unique
    // where a pid stops being.
    static auto const processNamespace = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();

    // A plain counter rather than anything meaningful: the only requirement is that no two
    // transports in this process share a namespace. Atomic because sessions are created from more
    // than one thread.
    static auto nextNamespace = std::atomic<uint64_t> { 0 };

    return std::format("contour-{}-{}", processNamespace, ++nextNamespace);
}

std::unique_ptr<NotificationTransport> makeNotificationTransport(
    [[maybe_unused]] NotificationBackend backend, [[maybe_unused]] std::chrono::milliseconds closeDelay)
{
#ifdef __linux__
    switch (backend)
    {
        case NotificationBackend::FreeDesktop: return std::make_unique<DBusNotificationTransport>();
        case NotificationBackend::Portal:
            return std::make_unique<PortalNotificationTransport>(
                closeDelay, makePortalIdPrefix(), qtDelayScheduler(), qtPortalCaller());
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
