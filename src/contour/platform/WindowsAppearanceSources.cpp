// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowsAppearanceSources.hpp>

#include <utility>

namespace contour::platform
{

WindowsAccessibilityPreferenceSource::WindowsAccessibilityPreferenceSource(
    std::shared_ptr<IWindowsSettingsReader const> reader, AccessibilityPreferencesChanged onChange):
    _report { std::move(reader), &accessibilityFrom, SettingTopic::Accessibility, std::move(onChange) }
{
}

config::AccessibilityPreferences WindowsAccessibilityPreferenceSource::current() const
{
    return _report.current();
}

void WindowsAccessibilityPreferenceSource::settingsChanged(SettingTopic topic)
{
    _report.settingsChanged(topic);
}

WindowsPlatformAppearance::WindowsPlatformAppearance(std::shared_ptr<IWindowsSettingsReader const> reader,
                                                     PlatformAppearanceChanged onChange):
    _report { std::move(reader), &appearanceFrom, SettingTopic::Appearance, std::move(onChange) }
{
}

PlatformAppearance WindowsPlatformAppearance::current() const
{
    return _report.current();
}

void WindowsPlatformAppearance::settingsChanged(SettingTopic topic)
{
    _report.settingsChanged(topic);
}

} // namespace contour::platform
