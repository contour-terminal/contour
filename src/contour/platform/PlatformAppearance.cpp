// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/PlatformAppearance.hpp>

#include <utility>

// Apple's factory lives in CocoaPlatformAppearance.mm, Windows' in Win32Settings.cpp, Linux's in
// PortalSourceFactories.cpp.
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)
namespace contour::platform
{

std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange)
{
    // The null source never changes, so it never needs the callback: release it here.
    [[maybe_unused]] auto const released = std::move(onChange);
    return std::make_unique<NullPlatformAppearance>();
}

} // namespace contour::platform
#endif
