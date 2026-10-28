// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/MaterialSupport.hpp>
#include <contour/config/ScrollBarMode.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// What Windows' accessibility and appearance settings mean for the chrome, Qt- and Windows-free: the Win32
// reader (Win32Settings.cpp) fills in the raw readings, and everything decided from them is here, tested on
// every OS.
namespace contour::platform
{

/// SPI_GETHIGHCONTRAST's HCF_HIGHCONTRASTON: whether a contrast theme is on.
enum class HighContrast : uint8_t
{
    Off = 0, ///< No contrast theme.
    On,      ///< A contrast theme is on.
};

/// SPI_GETCLIENTAREAANIMATION: Settings' "Animation effects".
enum class ClientAreaAnimation : uint8_t
{
    On = 0, ///< Animations run (Windows' default).
    Off,    ///< The user turned animation effects off.
};

/// What Windows reports, raw; each optional is absent when the value could not be read.
struct WindowsSettingsReadings
{
    /// HKCU\Software\Microsoft\Windows\DWM\AccentColor, 0xAABBGGRR.
    std::optional<uint32_t> accentColor {};
    /// HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\EnableTransparency; 0 is off.
    std::optional<uint32_t> enableTransparency {};
    /// HKCU\Control Panel\Accessibility\DynamicScrollbars; 0 is "Always show scrollbars".
    std::optional<uint32_t> dynamicScrollbars {};
    HighContrast highContrast = HighContrast::Off;           ///< Whether a contrast theme is on.
    ClientAreaAnimation animation = ClientAreaAnimation::On; ///< Whether animation effects are on.
    std::optional<uint32_t> osBuild {};                      ///< RtlGetVersion's dwBuildNumber.

    [[nodiscard]] bool operator==(WindowsSettingsReadings const&) const = default;
};

/// Reads Windows' settings. Behind an interface so the sources decide from a fake in tests and never from
/// the developer's or the runner's real settings.
class IWindowsSettingsReader
{
  public:
    virtual ~IWindowsSettingsReader() = default;

    /// @pre GUI thread.
    /// @return The settings as Windows reports them now.
    [[nodiscard]] virtual WindowsSettingsReadings read() const = 0;
};

/// The first Windows 11 build.
inline constexpr uint32_t Windows11Build = 22000;
/// The first build with DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2).
inline constexpr uint32_t SystemBackdropBuild = 22621;
/// The radius DWM rounds a Windows 11 window's corners by, in device-independent pixels.
inline constexpr int Windows11CornerRadius = 8;

/// @param abgr A colour as the registry stores it, 0xAABBGGRR.
/// @return Its red, green and blue.
[[nodiscard]] constexpr vtbackend::RGBColor rgbFromAbgr(uint32_t abgr) noexcept
{
    return vtbackend::RGBColor { static_cast<uint8_t>(abgr & 0xFF),
                                 static_cast<uint8_t>((abgr >> 8) & 0xFF),
                                 static_cast<uint8_t>((abgr >> 16) & 0xFF) };
}

/// @param build Windows' build number, if read.
/// @return Unknown when unread; Supported from Windows 11 22H2 on; Unsupported before.
[[nodiscard]] constexpr config::MaterialSupport materialSupportFor(std::optional<uint32_t> build) noexcept
{
    if (!build)
        return config::MaterialSupport::Unknown;
    return *build >= SystemBackdropBuild ? config::MaterialSupport::Supported
                                         : config::MaterialSupport::Unsupported;
}

/// @param build Windows' build number, if read.
/// @return Nothing when unread; Windows 11's 8 from build 22000 on; Windows 10's square 0 before.
[[nodiscard]] constexpr std::optional<int> windowCornerRadiusFor(std::optional<uint32_t> build) noexcept
{
    if (!build)
        return std::nullopt;
    return *build >= Windows11Build ? Windows11CornerRadius : 0;
}

/// @param readings What Windows reports.
/// @return The chrome's accessibility preferences. A contrast theme is Increased contrast and Reduced
///         transparency both, since Windows drops its own transparency effects under one (R-l).
[[nodiscard]] constexpr config::AccessibilityPreferences accessibilityFrom(
    WindowsSettingsReadings const& readings) noexcept
{
    auto const highContrast = readings.highContrast == HighContrast::On;
    auto const transparencyOff = readings.enableTransparency == uint32_t { 0 };
    return config::AccessibilityPreferences {
        .motion = readings.animation == ClientAreaAnimation::Off ? config::MotionPreference::Reduced
                                                                 : config::MotionPreference::Full,
        .transparency = transparencyOff || highContrast ? config::TransparencyPreference::Reduced
                                                        : config::TransparencyPreference::Allowed,
        .contrast =
            highContrast ? config::ContrastPreference::Increased : config::ContrastPreference::Standard,
    };
}

/// @param dynamicScrollbars The DynamicScrollbars reading.
/// @return Always when scroll bars always show, Overlay when they hide, nothing when unread.
[[nodiscard]] constexpr std::optional<config::ScrollBarMode> scrollBarsFrom(
    std::optional<uint32_t> dynamicScrollbars) noexcept
{
    if (!dynamicScrollbars)
        return std::nullopt;
    return *dynamicScrollbars == 0 ? config::ScrollBarMode::Always : config::ScrollBarMode::Overlay;
}

/// @param readings What Windows reports.
/// @return The appearance the chrome follows.
[[nodiscard]] constexpr PlatformAppearance appearanceFrom(WindowsSettingsReadings const& readings) noexcept
{
    return PlatformAppearance {
        .accent = readings.accentColor ? std::optional { rgbFromAbgr(*readings.accentColor) } : std::nullopt,
        .windowCornerRadius = windowCornerRadiusFor(readings.osBuild),
        .scrollBars = scrollBarsFrom(readings.dynamicScrollbars),
        .materials = materialSupportFor(readings.osBuild),
        .contrastTheme =
            readings.highContrast == HighContrast::On ? ContrastTheme::Active : ContrastTheme::None,
    };
}

/// Which source a settings broadcast concerns.
enum class SettingTopic : uint8_t
{
    None = 0,      ///< Neither: the broadcast is ignored.
    Accessibility, ///< The accessibility preferences.
    Appearance,    ///< The platform appearance.
    Both,          ///< Both sources.
};

/// @param topic What a broadcast concerns.
/// @param source The source asking, Accessibility or Appearance.
/// @return Whether @p source re-reads on @p topic.
[[nodiscard]] constexpr bool topicConcerns(SettingTopic topic, SettingTopic source) noexcept
{
    return topic != SettingTopic::None && (topic == source || topic == SettingTopic::Both);
}

/// The broadcast, as the adapter decoded it.
enum class SettingMessage : uint8_t
{
    SettingChange = 0,   ///< WM_SETTINGCHANGE.
    ColorizationChanged, ///< WM_DWMCOLORIZATIONCOLORCHANGED: the accent.
    ThemeChanged,        ///< WM_THEMECHANGED.
};

/// One decoded broadcast.
struct SettingChange
{
    SettingMessage message = SettingMessage::SettingChange; ///< Which message.
    uint32_t action = 0;         ///< WM_SETTINGCHANGE's wParam, the SPI_SET* action.
    std::u16string_view area {}; ///< WM_SETTINGCHANGE's lParam, the area's name.
};

/// SPI_SETHIGHCONTRAST, restated because this header is Windows-free.
inline constexpr uint32_t SpiSetHighContrast = 0x0043;
/// SPI_SETCLIENTAREAANIMATION, restated because this header is Windows-free.
inline constexpr uint32_t SpiSetClientAreaAnimation = 0x1043;

/// One row of the broadcast table: a message, the action and area it must carry (any when absent or
/// empty), and what it concerns.
struct SettingTopicRow
{
    SettingMessage message = SettingMessage::SettingChange; ///< Which message.
    std::optional<uint32_t> action {};                      ///< The action it must carry, if any.
    std::u16string_view area {};                            ///< The area it must name, if any.
    SettingTopic topic = SettingTopic::None;                ///< What it concerns.
};

/// What each broadcast concerns. "ImmersiveColorSet" is what Settings sends for the accent and the
/// transparency switch alike.
inline constexpr auto SettingTopicRows = std::array {
    SettingTopicRow { .message = SettingMessage::SettingChange,
                      .action = SpiSetHighContrast,
                      .area = {},
                      .topic = SettingTopic::Both },
    SettingTopicRow { .message = SettingMessage::SettingChange,
                      .action = SpiSetClientAreaAnimation,
                      .area = {},
                      .topic = SettingTopic::Accessibility },
    SettingTopicRow { .message = SettingMessage::SettingChange,
                      .action = std::nullopt,
                      .area = u"ImmersiveColorSet",
                      .topic = SettingTopic::Both },
    SettingTopicRow { .message = SettingMessage::ColorizationChanged,
                      .action = std::nullopt,
                      .area = {},
                      .topic = SettingTopic::Appearance },
    SettingTopicRow { .message = SettingMessage::ThemeChanged,
                      .action = std::nullopt,
                      .area = {},
                      .topic = SettingTopic::Both },
};

/// @param change A decoded broadcast.
/// @return What it concerns: the first matching @c SettingTopicRows row's topic, else None.
[[nodiscard]] constexpr SettingTopic topicOf(SettingChange const& change) noexcept
{
    auto const row = std::ranges::find_if(SettingTopicRows, [&](SettingTopicRow const& candidate) {
        return candidate.message == change.message
               && (!candidate.action || *candidate.action == change.action)
               && (candidate.area.empty() || candidate.area == change.area);
    });
    return row != SettingTopicRows.end() ? row->topic : SettingTopic::None;
}

} // namespace contour::platform
