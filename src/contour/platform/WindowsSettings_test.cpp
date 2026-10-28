// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowsSettings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using namespace contour::platform;
using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MaterialSupport;
using contour::config::MotionPreference;
using contour::config::ScrollBarMode;
using contour::config::TransparencyPreference;

namespace
{

/// One build and what it means for the chrome.
struct BuildCase
{
    std::optional<uint32_t> build {};
    MaterialSupport materials = MaterialSupport::Unknown;
    std::optional<int> cornerRadius {};
};

constexpr auto BuildCases = std::array {
    BuildCase { .build = std::nullopt, .materials = MaterialSupport::Unknown, .cornerRadius = std::nullopt },
    BuildCase { .build = 19045, .materials = MaterialSupport::Unsupported, .cornerRadius = 0 },
    BuildCase { .build = 22000, .materials = MaterialSupport::Unsupported, .cornerRadius = 8 },
    BuildCase { .build = 22621, .materials = MaterialSupport::Supported, .cornerRadius = 8 },
    BuildCase { .build = 26100, .materials = MaterialSupport::Supported, .cornerRadius = 8 },
};

} // namespace

TEST_CASE("windowsSettings.accentIsAbgr", "[windowssettings]")
{
    // Windows' default blue, as HKCU\Software\Microsoft\Windows\DWM\AccentColor stores it.
    CHECK(rgbFromAbgr(0xFFD77800) == vtbackend::RGBColor { 0x00, 0x78, 0xD7 });
}

TEST_CASE("windowsSettings.builds", "[windowssettings]")
{
    for (auto const& [build, materials, cornerRadius]: BuildCases)
    {
        CAPTURE(build.value_or(0));
        CHECK(materialSupportFor(build) == materials);
        CHECK(windowCornerRadiusFor(build) == cornerRadius);
    }
}

TEST_CASE("accessibility.defaults", "[windowssettings]")
{
    CHECK(accessibilityFrom(WindowsSettingsReadings {}) == AccessibilityPreferences {});
    CHECK(appearanceFrom(WindowsSettingsReadings {}) == PlatformAppearance {});
}

TEST_CASE("accessibility.animationOff", "[windowssettings]")
{
    auto const preferences =
        accessibilityFrom(WindowsSettingsReadings { .animation = ClientAreaAnimation::Off });
    CHECK(preferences.motion == MotionPreference::Reduced);
    CHECK(preferences.transparency == TransparencyPreference::Allowed);
    CHECK(preferences.contrast == ContrastPreference::Standard);
}

TEST_CASE("accessibility.transparencyOff", "[windowssettings]")
{
    CHECK(accessibilityFrom(WindowsSettingsReadings { .enableTransparency = 0 }).transparency
          == TransparencyPreference::Reduced);
    CHECK(accessibilityFrom(WindowsSettingsReadings { .enableTransparency = 1 }).transparency
          == TransparencyPreference::Allowed);
    CHECK(accessibilityFrom(WindowsSettingsReadings { .enableTransparency = std::nullopt }).transparency
          == TransparencyPreference::Allowed);
}

TEST_CASE("accessibility.highContrast", "[windowssettings]")
{
    // A contrast theme raises the chrome's contrast, drops transparency as Windows itself does, and hands the
    // chrome to the OS palette (R-l).
    auto const readings = WindowsSettingsReadings { .highContrast = HighContrast::On };
    auto const preferences = accessibilityFrom(readings);
    CHECK(preferences.contrast == ContrastPreference::Increased);
    CHECK(preferences.transparency == TransparencyPreference::Reduced);
    CHECK(preferences.motion == MotionPreference::Full);
    CHECK(appearanceFrom(readings).contrastTheme == ContrastTheme::Active);
    CHECK(appearanceFrom(WindowsSettingsReadings {}).contrastTheme == ContrastTheme::None);
}

TEST_CASE("appearance.scrollBars", "[windowssettings]")
{
    // HKCU\Control Panel\Accessibility\DynamicScrollbars: 0 is "always show scroll bars" (F24).
    CHECK(appearanceFrom(WindowsSettingsReadings { .dynamicScrollbars = 0 }).scrollBars
          == ScrollBarMode::Always);
    CHECK(appearanceFrom(WindowsSettingsReadings { .dynamicScrollbars = 1 }).scrollBars
          == ScrollBarMode::Overlay);
    CHECK_FALSE(appearanceFrom(WindowsSettingsReadings {}).scrollBars.has_value());
}

TEST_CASE("topics.table", "[windowssettings]")
{
    for (auto const& row: SettingTopicRows)
    {
        auto const change =
            SettingChange { .message = row.message, .action = row.action.value_or(0), .area = row.area };
        CAPTURE(static_cast<int>(row.message), change.action);
        CHECK(topicOf(change) == row.topic);
    }
    CHECK(topicOf(SettingChange { .message = SettingMessage::SettingChange, .action = 0, .area = u"Policy" })
          == SettingTopic::None);
    CHECK(topicConcerns(SettingTopic::Both, SettingTopic::Accessibility));
    CHECK(topicConcerns(SettingTopic::Appearance, SettingTopic::Appearance));
    CHECK_FALSE(topicConcerns(SettingTopic::Appearance, SettingTopic::Accessibility));
    CHECK_FALSE(topicConcerns(SettingTopic::None, SettingTopic::Appearance));
}
