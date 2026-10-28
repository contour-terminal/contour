// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowsAppearanceSources.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace contour::platform;
using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MaterialSupport;

namespace
{

/// Answers what a case sets, and counts the reads.
class FakeWindowsSettingsReader final: public IWindowsSettingsReader
{
  public:
    WindowsSettingsReadings readings {}; ///< What read() answers.
    mutable int reads = 0;               ///< How often read() was asked.

    [[nodiscard]] WindowsSettingsReadings read() const override
    {
        ++reads;
        return readings;
    }
};

} // namespace

TEST_CASE("a source reports a change once", "[appearance-source][windowssettings]")
{
    auto const reader = std::make_shared<FakeWindowsSettingsReader>();
    auto received = std::vector<AccessibilityPreferences> {};
    auto source = WindowsAccessibilityPreferenceSource { reader, [&](AccessibilityPreferences next) {
                                                            received.push_back(next);
                                                        } };
    CHECK(source.current() == AccessibilityPreferences {});

    reader->readings.highContrast = HighContrast::On;
    source.settingsChanged(SettingTopic::Both);
    REQUIRE(received.size() == 1);
    CHECK(received.front().contrast == ContrastPreference::Increased);
    CHECK(source.current().contrast == ContrastPreference::Increased);

    source.settingsChanged(SettingTopic::Both);
    CHECK(received.size() == 1);

    // A broadcast about the appearance alone is not the accessibility source's to re-read.
    reader->readings.highContrast = HighContrast::Off;
    auto const readsBefore = reader->reads;
    source.settingsChanged(SettingTopic::Appearance);
    CHECK(received.size() == 1);
    CHECK(reader->reads == readsBefore);
}

TEST_CASE("the appearance source follows the accent and the build", "[appearance-source][windowssettings]")
{
    auto const reader = std::make_shared<FakeWindowsSettingsReader>();
    reader->readings.accentColor = 0xFF2B1CC4;
    reader->readings.osBuild = 19045;
    auto received = std::vector<PlatformAppearance> {};
    auto source =
        WindowsPlatformAppearance { reader, [&](PlatformAppearance next) { received.push_back(next); } };

    auto const expected = PlatformAppearance { .accent = vtbackend::RGBColor { 0xC4, 0x1C, 0x2B },
                                               .windowCornerRadius = 0,
                                               .scrollBars = std::nullopt,
                                               .materials = MaterialSupport::Unsupported,
                                               .contrastTheme = ContrastTheme::None };
    CHECK(source.current() == expected);

    reader->readings.osBuild = 26100;
    source.settingsChanged(SettingTopic::Accessibility);
    CHECK(received.empty());
    source.settingsChanged(SettingTopic::Appearance);
    REQUIRE(received.size() == 1);
    CHECK(received.front().materials == MaterialSupport::Supported);
}

TEST_CASE("a source without a callback only re-reads", "[appearance-source][windowssettings]")
{
    auto const reader = std::make_shared<FakeWindowsSettingsReader>();
    auto source = WindowsPlatformAppearance { reader, PlatformAppearanceChanged {} };
    reader->readings.osBuild = 22621;
    source.settingsChanged(SettingTopic::Both);
    CHECK(source.current().materials == MaterialSupport::Supported);
}
