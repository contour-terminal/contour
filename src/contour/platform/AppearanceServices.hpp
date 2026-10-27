// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeCaptionButtons.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <functional>
#include <memory>
#include <optional>

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
    /// Builds the integration that hands the menu bar's Window and Help menus to the OS.
    std::function<std::unique_ptr<IMenuBarIntegration>()> menuBar = makeMenuBarIntegration;
    /// Builds the material the OS draws behind a client-drawn title band.
    std::function<std::unique_ptr<ITitleBandMaterial>()> titleBand = makeTitleBandMaterial;
    /// Builds the OS's secure keyboard entry.
    std::function<std::unique_ptr<ISecureInput>()> secureInput = makeSecureInput;
    /// Builds the adapter that hands a client-drawn window's maximise button to the OS (Snap Layouts).
    std::function<std::unique_ptr<INativeCaptionButtons>()> caption = makeNativeCaptionButtons;
    /// The platform whose frame policy the windows follow (framePolicyFor): the build's own by default, or
    /// another, so a test drives a Windows host's frame decisions on any OS.
    FramePlatform framePlatform = currentFramePlatform();
    /// The host whose chrome row and window controls are drawn; empty detects it from the environment
    /// (config::detectHostPlatform). Set, a test draws a Windows host's chrome on any OS.
    std::optional<config::HostPlatform> hostPlatform = std::nullopt;
};

/// @return Factories that build the null sources, the null menu presenter, the null menu bar
///         integration, the null title-band material, the null secure input and the null caption adapter on
///         every platform, with the build's own frame platform and the detected host; no source ever calls
///         back.
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
        .menuBar = []() -> std::unique_ptr<IMenuBarIntegration> {
            return std::make_unique<NullMenuBarIntegration>();
        },
        .titleBand = []() -> std::unique_ptr<ITitleBandMaterial> {
            return std::make_unique<NullTitleBandMaterial>();
        },
        .secureInput = []() -> std::unique_ptr<ISecureInput> { return std::make_unique<NullSecureInput>(); },
        .caption = []() -> std::unique_ptr<INativeCaptionButtons> {
            return std::make_unique<NullNativeCaptionButtons>();
        },
        .framePlatform = currentFramePlatform(),
        .hostPlatform = std::nullopt,
    };
}

} // namespace contour::platform
