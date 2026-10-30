// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/UserNotificationCenter.hpp>

// macOS defines these in CocoaUserNotificationCenter.mm.
#ifndef __APPLE__

namespace contour::platform
{

AppBundle currentAppBundle()
{
    return AppBundle::Absent;
}

std::unique_ptr<IUserNotificationCenter> makeUserNotificationCenter()
{
    return std::make_unique<NullUserNotificationCenter>();
}

} // namespace contour::platform

#endif
