// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#import <AppKit/AppKit.h>

using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MotionPreference;
using contour::config::TransparencyPreference;
using contour::platform::PlatformAppearance;

namespace
{

/// The accent the backend should report, resolved the way the backend documents it resolves it.
[[nodiscard]] vtbackend::RGBColor expectedAccent()
{
    __block NSColor* srgb = nil;
    auto const resolve = ^{
      srgb = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    };
    if (NSApp != nil)
        [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:resolve];
    else
        resolve();
    REQUIRE(srgb != nil);
    auto const channel = [](CGFloat value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, CGFloat { 0 }, CGFloat { 1 }) * 255.0));
    };
    return vtbackend::RGBColor { channel(srgb.redComponent),
                                 channel(srgb.greenComponent),
                                 channel(srgb.blueComponent) };
}

} // namespace

TEST_CASE("Cocoa accessibility source reads the NSWorkspace flags", "[appearance-source][cocoa]")
{
    auto* const workspace = NSWorkspace.sharedWorkspace;
    auto const expected = AccessibilityPreferences {
        .motion = workspace.accessibilityDisplayShouldReduceMotion ? MotionPreference::Reduced
                                                                   : MotionPreference::Full,
        .transparency = workspace.accessibilityDisplayShouldReduceTransparency
                            ? TransparencyPreference::Reduced
                            : TransparencyPreference::Allowed,
        .contrast = workspace.accessibilityDisplayShouldIncreaseContrast ? ContrastPreference::Increased
                                                                         : ContrastPreference::Standard,
    };
    CHECK(contour::platform::makeAccessibilityPreferenceSource({})->current() == expected);
}

TEST_CASE("Cocoa accessibility source reports the workspace notification", "[appearance-source][cocoa]")
{
    auto received = std::vector<AccessibilityPreferences> {};
    auto source = contour::platform::makeAccessibilityPreferenceSource(
        [&](AccessibilityPreferences next) { received.push_back(next); });
    CHECK(received.empty());
    [NSWorkspace.sharedWorkspace.notificationCenter
        postNotificationName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                      object:NSWorkspace.sharedWorkspace];
    REQUIRE(received.size() == 1);
    CHECK(received.front() == source->current());

    // The destructor removes the observer: a later notification reaches nobody.
    source.reset();
    [NSWorkspace.sharedWorkspace.notificationCenter
        postNotificationName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                      object:NSWorkspace.sharedWorkspace];
    CHECK(received.size() == 1);
}

TEST_CASE("Cocoa appearance reads the accent and reports system colour changes",
          "[appearance-source][cocoa]")
{
    auto const current = contour::platform::makePlatformAppearance({})->current();
    REQUIRE(current.accent.has_value());
    CHECK(*current.accent == expectedAccent());
    CHECK_FALSE(current.windowCornerRadius.has_value());

    auto received = std::vector<PlatformAppearance> {};
    auto appearance =
        contour::platform::makePlatformAppearance([&](PlatformAppearance next) { received.push_back(next); });
    CHECK(received.empty());
    [NSNotificationCenter.defaultCenter postNotificationName:NSSystemColorsDidChangeNotification object:nil];
    REQUIRE(received.size() == 1);
    CHECK(received.front() == appearance->current());

    appearance.reset();
    [NSNotificationCenter.defaultCenter postNotificationName:NSSystemColorsDidChangeNotification object:nil];
    CHECK(received.size() == 1);
}
