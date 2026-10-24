// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AccessibilityPreferenceSource.hpp>

#include <utility>

// Apple's factory lives in CocoaAccessibilityPreferenceSource.mm.
#ifndef __APPLE__
namespace contour::platform
{

std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(
    AccessibilityPreferencesChanged onChange)
{
    // The null source never changes, so it never needs the callback: release it here.
    [[maybe_unused]] auto const released = std::move(onChange);
    return std::make_unique<NullAccessibilityPreferenceSource>();
}

} // namespace contour::platform
#endif
