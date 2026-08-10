// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Notifier.hpp>

#ifdef __linux__
    #include <contour/platform/DBusNotificationTransport.hpp>
    #include <contour/platform/FreeDesktopNotifier.hpp>
#endif

namespace contour::platform
{

std::unique_ptr<Notifier> makeDesktopNotifier()
{
#ifdef __linux__
    return std::make_unique<FreeDesktopNotifier>(std::make_unique<DBusNotificationTransport>());
#else
    return std::make_unique<NullNotifier>();
#endif
}

} // namespace contour::platform
