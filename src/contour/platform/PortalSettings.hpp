// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The XDG Settings portal's values (`org.freedesktop.portal.Settings`), and what the chrome makes of
/// them. Qt-free: the D-Bus adapter only unwraps a reply into these types, and every decision about
/// what a key means is a table here, so a new portal key is a row.

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace contour::platform
{

/// The namespace that carries the desktop's appearance: accent, light/dark, contrast, motion.
inline constexpr std::string_view AppearanceNamespace = "org.freedesktop.appearance";
/// GNOME's window-manager namespace, which carries the window button layout.
inline constexpr std::string_view GnomeWmNamespace = "org.gnome.desktop.wm.preferences";

inline constexpr std::string_view ColorSchemeKey = "color-scheme";     ///< `u`: 0 none, 1 dark, 2 light.
inline constexpr std::string_view AccentColorKey = "accent-color";     ///< `(ddd)`: sRGB, each in [0, 1].
inline constexpr std::string_view ContrastKey = "contrast";            ///< `u`: 0 normal, 1 high.
inline constexpr std::string_view ReducedMotionKey = "reduced-motion"; ///< `u`: 0 none, 1 reduced.
inline constexpr std::string_view ButtonLayoutKey = "button-layout";   ///< `s`: GTK's layout string.

/// The accent colour when no Settings portal answers at all (§2.4, ruling Q4).
inline constexpr vtbackend::RGBColor PortalFallbackAccent { 0x35, 0x84, 0xE4 };

/// A colour as the portal sends it: three sRGB channels, each in [0, 1] when set.
struct PortalColor
{
    double red = 0;   ///< The red channel.
    double green = 0; ///< The green channel.
    double blue = 0;  ///< The blue channel.

    [[nodiscard]] bool operator==(PortalColor const&) const = default;
};

/// One setting's value: the types the portal keys this file reads use, or nothing for any other.
using PortalSettingValue = std::variant<std::monostate, std::uint32_t, std::string, PortalColor>;

/// One setting, addressed by its namespace and key.
struct PortalSetting
{
    std::string group {};        ///< The namespace, e.g. AppearanceNamespace.
    std::string key {};          ///< The key within it, e.g. AccentColorKey.
    PortalSettingValue value {}; ///< Its decoded value.

    [[nodiscard]] bool operator==(PortalSetting const&) const = default;
};

/// Every setting a portal reported, in the order it reported them.
using PortalSettings = std::vector<PortalSetting>;

/// Why the portal gave no settings.
enum class PortalSettingsError : uint8_t
{
    NoPortal = 0, ///< No Settings portal on the bus: an unknown service, object, interface or method.
    Failed,       ///< Any other failure, such as a timeout.
};

/// What asking the portal gave: its settings, or why there are none.
using PortalReading = std::expected<PortalSettings, PortalSettingsError>;

/// Records a reported change.
/// @param settings The settings so far.
/// @param change The changed setting.
/// @return @p settings with the `(group, key)` entry of @p change replaced, or @p change appended.
[[nodiscard]] inline PortalSettings mergePortalSetting(PortalSettings settings, PortalSetting change)
{
    auto const existing = std::ranges::find_if(settings, [&](PortalSetting const& setting) {
        return setting.group == change.group && setting.key == change.key;
    });
    if (existing != settings.end())
        existing->value = std::move(change.value);
    else
        settings.push_back(std::move(change));
    return settings;
}

/// The value of one setting, when it is present and of type @p T.
/// @param settings The settings to search.
/// @param group The namespace.
/// @param key The key.
/// @return The value, or null when the key is absent or holds another type.
template <typename T>
[[nodiscard]] T const* portalValueOf(PortalSettings const& settings,
                                     std::string_view group,
                                     std::string_view key)
{
    auto const found = std::ranges::find_if(
        settings, [&](PortalSetting const& setting) { return setting.group == group && setting.key == key; });
    return found != settings.end() ? std::get_if<T>(&found->value) : nullptr;
}

/// The accent colour a portal colour stands for.
/// @param color The portal's value.
/// @return Each channel as `lround(channel × 255)`, or nothing when any channel is outside [0, 1],
///         which is how the portal says "no accent".
[[nodiscard]] inline std::optional<vtbackend::RGBColor> accentOf(PortalColor color)
{
    auto const channels = std::array { color.red, color.green, color.blue };
    if (!std::ranges::all_of(channels, [](double channel) { return channel >= 0.0 && channel <= 1.0; }))
        return std::nullopt;
    auto const toByte = [](double channel) {
        return static_cast<std::uint8_t>(std::lround(channel * 255.0));
    };
    return vtbackend::RGBColor { toByte(color.red), toByte(color.green), toByte(color.blue) };
}

/// One `color-scheme` value and the light/dark preference it stands for.
struct ColorSchemeValue
{
    std::uint32_t value;               ///< The portal's value.
    vtbackend::ColorPreference scheme; ///< What it means.
};

/// The `color-scheme` values that name a preference; any other (0, "no preference") names none.
inline constexpr std::array ColorSchemeValues {
    ColorSchemeValue { .value = 1, .scheme = vtbackend::ColorPreference::Dark },
    ColorSchemeValue { .value = 2, .scheme = vtbackend::ColorPreference::Light },
};

/// One accessibility key: the value that turns it on, and what it then changes.
struct PortalAccessibilityRow
{
    std::string_view key;                             ///< The key in AppearanceNamespace.
    std::uint32_t enabled;                            ///< The value that means "on".
    void (*apply)(config::AccessibilityPreferences&); ///< What "on" changes.
};

/// The accessibility keys the portal serves (§2.3). Transparency has no portal key and stays Allowed.
inline constexpr std::array PortalAccessibilityRows {
    PortalAccessibilityRow { .key = ContrastKey,
                             .enabled = 1,
                             .apply =
                                 [](config::AccessibilityPreferences& preferences) {
                                     preferences.contrast = config::ContrastPreference::Increased;
                                 } },
    PortalAccessibilityRow { .key = ReducedMotionKey,
                             .enabled = 1,
                             .apply =
                                 [](config::AccessibilityPreferences& preferences) {
                                     preferences.motion = config::MotionPreference::Reduced;
                                 } },
};

/// The light/dark preference the portal publishes.
/// @param settings The portal's settings.
/// @return The preference, or nothing for "no preference", an unknown value or a value of the wrong type.
[[nodiscard]] inline std::optional<vtbackend::ColorPreference> colorSchemeOf(PortalSettings const& settings)
{
    auto const* value = portalValueOf<std::uint32_t>(settings, AppearanceNamespace, ColorSchemeKey);
    if (value == nullptr)
        return std::nullopt;
    auto const row = std::ranges::find(ColorSchemeValues, *value, &ColorSchemeValue::value);
    return row != ColorSchemeValues.end() ? std::optional { row->scheme } : std::nullopt;
}

/// What the button layout is resolved from besides the portal: the desktop's files and the session.
struct DesktopLayoutInputs
{
    config::ButtonLayoutSources files {};                       ///< KWin's keys and GTK's layout, if read.
    config::HostPlatform desktop = config::HostPlatform::Other; ///< The session, which orders the sources.
};

/// The window button layout the portal and the desktop's files state.
/// @param settings The portal's settings; its GNOME `button-layout` joins @p desktop's file sources.
/// @param desktop The file sources and the session.
/// @return config::resolveDesktopButtonLayout() of them all.
[[nodiscard]] inline config::WindowButtonLayout buttonLayoutOf(PortalSettings const& settings,
                                                               DesktopLayoutInputs const& desktop)
{
    auto sources = desktop.files;
    if (auto const* layout = portalValueOf<std::string>(settings, GnomeWmNamespace, ButtonLayoutKey))
        sources.gnomeButtonLayout = *layout;
    return config::resolveDesktopButtonLayout(sources, desktop.desktop);
}

/// The appearance the portal's answer stands for.
/// @param reading The portal's settings, or why there are none.
/// @param desktop What the button layout is resolved from besides the portal.
/// @return With no portal answer, the accent PortalFallbackAccent and the button layout of @p desktop's
///         files. With an answer, the portal's accent (none when absent, "unset" or of the wrong type, so
///         the platform theme's highlight stays), its light/dark preference, and the button layout of
///         its GNOME key and @p desktop's files. The window corner radius is never set.
[[nodiscard]] inline PlatformAppearance appearanceFromPortal(PortalReading const& reading,
                                                             DesktopLayoutInputs const& desktop)
{
    if (!reading)
        return PlatformAppearance { .accent = PortalFallbackAccent,
                                    .windowCornerRadius = std::nullopt,
                                    .scrollBars = std::nullopt,
                                    .materials = config::MaterialSupport::Unknown,
                                    .contrastTheme = ContrastTheme::None,
                                    .colorScheme = std::nullopt,
                                    .buttonLayout = buttonLayoutOf({}, desktop) };
    auto const& settings = *reading;
    auto const* accent = portalValueOf<PortalColor>(settings, AppearanceNamespace, AccentColorKey);
    return PlatformAppearance { .accent = accent != nullptr ? accentOf(*accent) : std::nullopt,
                                .windowCornerRadius = std::nullopt,
                                .scrollBars = std::nullopt,
                                .materials = config::MaterialSupport::Unknown,
                                .contrastTheme = ContrastTheme::None,
                                .colorScheme = colorSchemeOf(settings),
                                .buttonLayout = buttonLayoutOf(settings, desktop) };
}

/// The accessibility preferences the portal publishes.
/// @param settings The portal's settings.
/// @return The defaults, changed by each PortalAccessibilityRows key set to its "on" value.
[[nodiscard]] inline config::AccessibilityPreferences accessibilityFromPortal(PortalSettings const& settings)
{
    auto preferences = config::AccessibilityPreferences {};
    for (auto const& row: PortalAccessibilityRows)
    {
        auto const* value = portalValueOf<std::uint32_t>(settings, AppearanceNamespace, row.key);
        if (value != nullptr && *value == row.enabled)
            row.apply(preferences);
    }
    return preferences;
}

} // namespace contour::platform
