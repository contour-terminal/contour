// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <functional>
#include <memory>

namespace contour::platform
{

/// The factories that build the operating-system appearance sources: the injection seam, so a test
/// or a headless run can substitute its own sources for the native ones.
struct AppearanceServices
{
    /// Builds the accessibility preference source.
    std::function<std::unique_ptr<IAccessibilityPreferenceSource>(AccessibilityPreferencesChanged)>
        accessibility = makeAccessibilityPreferenceSource;
    /// Builds the platform appearance source.
    std::function<std::unique_ptr<IPlatformAppearance>(PlatformAppearanceChanged)> appearance =
        makePlatformAppearance;
    /// Builds the native menu presenter.
    std::function<std::unique_ptr<INativeMenuPresenter>()> menus = makeNativeMenuPresenter;
};

/// @return Factories that build the null sources and the null menu presenter on every platform; no
///         source ever calls back.
[[nodiscard]] inline AppearanceServices nullAppearanceServices()
{
    return AppearanceServices {
        .accessibility =
            [](AccessibilityPreferencesChanged const&) -> std::unique_ptr<IAccessibilityPreferenceSource> {
            return std::make_unique<NullAccessibilityPreferenceSource>();
        },
        .appearance = [](PlatformAppearanceChanged const&) -> std::unique_ptr<IPlatformAppearance> {
            return std::make_unique<NullPlatformAppearance>();
        },
        .menus = []() -> std::unique_ptr<INativeMenuPresenter> {
            return std::make_unique<NullNativeMenuPresenter>();
        },
    };
}

} // namespace contour::platform
