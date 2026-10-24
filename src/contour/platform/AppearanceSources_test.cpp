// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/test/AppearanceFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MotionPreference;
using contour::config::TransparencyPreference;
using contour::platform::PlatformAppearance;

TEST_CASE("The null sources report the defaults", "[appearance-source]")
{
    CHECK(contour::platform::NullAccessibilityPreferenceSource {}.current() == AccessibilityPreferences {});
    CHECK(contour::platform::NullPlatformAppearance {}.current() == PlatformAppearance {});
}

TEST_CASE("nullAppearanceServices builds silent default sources", "[appearance-source]")
{
    auto const services = contour::platform::nullAppearanceServices();
    auto accessibilityCalls = 0;
    auto appearanceCalls = 0;
    auto const accessibility =
        services.accessibility([&](AccessibilityPreferences) { ++accessibilityCalls; });
    auto const appearance = services.appearance([&](PlatformAppearance) { ++appearanceCalls; });
    REQUIRE(accessibility != nullptr);
    REQUIRE(appearance != nullptr);
    CHECK(accessibility->current() == AccessibilityPreferences {});
    CHECK(appearance->current() == PlatformAppearance {});
    CHECK(accessibilityCalls == 0);
    CHECK(appearanceCalls == 0);
}

TEST_CASE("appearanceServicesWith drives sources from the switches", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    auto appearanceSwitch = contour::test::AppearanceSwitch {};
    auto const services = contour::test::appearanceServicesWith(accessibilitySwitch, appearanceSwitch);

    auto received = std::vector<AccessibilityPreferences> {};
    auto const source =
        services.accessibility([&](AccessibilityPreferences next) { received.push_back(next); });
    REQUIRE(source != nullptr);
    CHECK(source->current() == AccessibilityPreferences {});
    CHECK(received.empty());

    auto const reduced = AccessibilityPreferences { .motion = MotionPreference::Reduced,
                                                    .transparency = TransparencyPreference::Allowed,
                                                    .contrast = ContrastPreference::Standard };
    accessibilitySwitch.set(reduced);
    CHECK(source->current().motion == MotionPreference::Reduced);
    REQUIRE(received.size() == 1);
    CHECK(received.front() == reduced);

    auto appearances = std::vector<PlatformAppearance> {};
    auto const appearance =
        services.appearance([&](PlatformAppearance next) { appearances.push_back(next); });
    REQUIRE(appearance != nullptr);
    auto const accented =
        PlatformAppearance { .accent = vtbackend::RGBColor { 0x0A, 0x84, 0xFF }, .windowCornerRadius = 10 };
    appearanceSwitch.set(accented);
    CHECK(appearance->current() == accented);
    REQUIRE(appearances.size() == 1);
    CHECK(appearances.front() == accented);
}

TEST_CASE("A switch with no source attached only stores", "[appearance-source]")
{
    auto accessibilitySwitch = contour::test::AccessibilitySwitch {};
    accessibilitySwitch.set({ .motion = MotionPreference::Full,
                              .transparency = TransparencyPreference::Reduced,
                              .contrast = ContrastPreference::Increased });
    CHECK(accessibilitySwitch.preferences.contrast == ContrastPreference::Increased);
}
