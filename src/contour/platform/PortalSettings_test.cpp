// SPDX-License-Identifier: Apache-2.0
//
// The Settings portal's values, decoded: accent, light/dark and the accessibility keys, each a table.
// Qt-free on purpose -- the decoders live in a header with no Qt in it, which is what lets contour_test
// cover them in both frontend configurations, and keeps the D-Bus adapter down to unwrapping variants.

#include <contour/platform/PortalSettings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

using contour::config::AccessibilityPreferences;
using contour::config::ContrastPreference;
using contour::config::MotionPreference;
using contour::config::TransparencyPreference;
using contour::platform::AccentColorKey;
using contour::platform::AppearanceNamespace;
using contour::platform::ColorSchemeKey;
using contour::platform::ContrastKey;
using contour::platform::PortalColor;
using contour::platform::PortalFallbackAccent;
using contour::platform::PortalReading;
using contour::platform::PortalSetting;
using contour::platform::PortalSettings;
using contour::platform::PortalSettingsError;
using contour::platform::PortalSettingValue;
using contour::platform::ReducedMotionKey;
using vtbackend::ColorPreference;
using vtbackend::RGBColor;

namespace
{

/// @return The setting @p key of the appearance namespace, holding @p value.
[[nodiscard]] PortalSetting appearanceSetting(std::string_view key, PortalSettingValue value)
{
    return PortalSetting { .group = std::string { AppearanceNamespace },
                           .key = std::string { key },
                           .value = std::move(value) };
}

/// @return A reading of exactly one appearance setting.
[[nodiscard]] PortalReading readingOf(std::string_view key, PortalSettingValue value)
{
    return PortalSettings { appearanceSetting(key, std::move(value)) };
}

} // namespace

TEST_CASE("portal.accent", "[portal]")
{
    struct Row
    {
        PortalColor color;
        std::optional<RGBColor> expected;
    };
    auto const rows = std::array {
        Row { .color = { .red = 0.2078, .green = 0.5176, .blue = 0.8941 }, .expected = PortalFallbackAccent },
        Row { .color = { .red = 1, .green = 1, .blue = 1 }, .expected = RGBColor { 0xFF, 0xFF, 0xFF } },
        Row { .color = { .red = 0, .green = 0, .blue = 0 }, .expected = RGBColor { 0, 0, 0 } },
        // The portal's "unset" is any channel outside [0, 1].
        Row { .color = { .red = -1, .green = -1, .blue = -1 }, .expected = std::nullopt },
        Row { .color = { .red = 1.5, .green = 0, .blue = 0 }, .expected = std::nullopt },
        Row { .color = { .red = 0, .green = 0, .blue = -0.01 }, .expected = std::nullopt },
    };
    for (auto const& row: rows)
    {
        INFO(row.color.red << ", " << row.color.green << ", " << row.color.blue);
        CHECK(contour::platform::accentOf(row.color) == row.expected);
    }
}

TEST_CASE("portal.appearance", "[portal]")
{
    using contour::platform::appearanceFromPortal;

    SECTION("no portal answered: the fallback accent, nothing else")
    {
        // Review Focus 2 and ruling Q4: #3584E4 only when no portal answers at all.
        for (auto const error: { PortalSettingsError::NoPortal, PortalSettingsError::Failed })
        {
            auto const appearance = appearanceFromPortal(std::unexpected(error), {});
            CHECK(appearance.accent == PortalFallbackAccent);
            CHECK_FALSE(appearance.colorScheme.has_value());
            CHECK_FALSE(appearance.windowCornerRadius.has_value());
        }
    }

    SECTION("a portal that answers without an accent leaves the palette's highlight")
    {
        // Ruling Q4: a portal answered, so the platform theme's own highlight stays.
        auto const appearance = appearanceFromPortal(PortalSettings {}, {});
        CHECK_FALSE(appearance.accent.has_value());
        CHECK_FALSE(appearance.colorScheme.has_value());
        CHECK_FALSE(appearance.windowCornerRadius.has_value());
    }

    SECTION("the portal's accent")
    {
        auto const red = readingOf(AccentColorKey, PortalColor { .red = 1, .green = 0, .blue = 0 });
        CHECK(appearanceFromPortal(red, {}).accent == RGBColor { 0xFF, 0, 0 });
        // An out-of-range accent is the portal's "unset": the portal answered, so no fallback.
        auto const unset = readingOf(AccentColorKey, PortalColor { .red = -1, .green = -1, .blue = -1 });
        CHECK_FALSE(appearanceFromPortal(unset, {}).accent.has_value());
        // A value of the wrong type counts as absent.
        CHECK_FALSE(
            appearanceFromPortal(readingOf(AccentColorKey, std::uint32_t { 1 }), {}).accent.has_value());
    }

    SECTION("color-scheme")
    {
        struct Row
        {
            PortalSettingValue value;
            std::optional<ColorPreference> expected;
        };
        auto const rows = std::array {
            Row { .value = std::uint32_t { 0 }, .expected = std::nullopt },
            Row { .value = std::uint32_t { 1 }, .expected = ColorPreference::Dark },
            Row { .value = std::uint32_t { 2 }, .expected = ColorPreference::Light },
            Row { .value = std::uint32_t { 7 }, .expected = std::nullopt },
            Row { .value = std::string { "1" }, .expected = std::nullopt },
            Row { .value = std::monostate {}, .expected = std::nullopt },
        };
        for (auto const& row: rows)
        {
            INFO("variant index " << row.value.index());
            CHECK(appearanceFromPortal(readingOf(ColorSchemeKey, row.value), {}).colorScheme == row.expected);
        }
    }

    SECTION("the button layout")
    {
        using contour::config::HostPlatform;
        using contour::config::KWinButtonKeys;
        using contour::config::WindowButtonLayout;
        using contour::config::WindowControlKind;
        using contour::platform::DesktopLayoutInputs;
        using contour::platform::GnomeWmNamespace;

        auto const leftHanded = WindowButtonLayout {
            .leading = { WindowControlKind::Close, WindowControlKind::Minimize, WindowControlKind::Maximize },
            .trailing = {}
        };
        auto const gnome =
            PortalSettings { PortalSetting { .group = std::string { GnomeWmNamespace },
                                             .key = "button-layout",
                                             .value = std::string { "close,minimize,maximize:" } } };
        auto const kwinFiles =
            contour::config::ButtonLayoutSources { .gnomeButtonLayout = std::nullopt,
                                                   .kwin = KWinButtonKeys { .left = std::string { "X" },
                                                                            .right = std::string { "" } },
                                                   .gtkDecorationLayout = std::nullopt };
        auto const gtkFiles = contour::config::ButtonLayoutSources { .gnomeButtonLayout = std::nullopt,
                                                                     .kwin = std::nullopt,
                                                                     .gtkDecorationLayout = "close:" };

        // No portal, no files: the fallback (Review Focus 2).
        CHECK(appearanceFromPortal(std::unexpected(PortalSettingsError::NoPortal), {}).buttonLayout
              == contour::config::fallbackButtonLayout());
        CHECK(appearanceFromPortal(PortalSettings {}, {}).buttonLayout
              == contour::config::fallbackButtonLayout());
        // The portal's GNOME key (Review Focus 1).
        CHECK(appearanceFromPortal(gnome, {}).buttonLayout == leftHanded);
        // ... which outranks KWin's keys outside a KDE session ...
        CHECK(appearanceFromPortal(gnome,
                                   DesktopLayoutInputs { .files = kwinFiles, .desktop = HostPlatform::Other })
                  .buttonLayout
              == leftHanded);
        // ... and not on one (ruling Q3).
        CHECK(appearanceFromPortal(
                  gnome, DesktopLayoutInputs { .files = kwinFiles, .desktop = HostPlatform::KdePlasma })
                  .buttonLayout
              == WindowButtonLayout { .leading = { WindowControlKind::Close }, .trailing = {} });
        // No portal key: GTK's settings.ini, with or without a portal.
        auto const gtkOnly = WindowButtonLayout { .leading = { WindowControlKind::Close }, .trailing = {} };
        CHECK(appearanceFromPortal(PortalSettings {},
                                   DesktopLayoutInputs { .files = gtkFiles, .desktop = HostPlatform::Other })
                  .buttonLayout
              == gtkOnly);
        CHECK(appearanceFromPortal(std::unexpected(PortalSettingsError::Failed),
                                   DesktopLayoutInputs { .files = gtkFiles, .desktop = HostPlatform::Other })
                  .buttonLayout
              == gtkOnly);
        // A key of the wrong type counts as absent.
        auto const wrongType = PortalSettings { PortalSetting { .group = std::string { GnomeWmNamespace },
                                                                .key = "button-layout",
                                                                .value = std::uint32_t { 1 } } };
        CHECK(appearanceFromPortal(wrongType, {}).buttonLayout == contour::config::fallbackButtonLayout());
    }

    SECTION("a key of another namespace is not read")
    {
        auto const settings = PortalSettings { PortalSetting { .group = "org.example.other",
                                                               .key = std::string { ColorSchemeKey },
                                                               .value = std::uint32_t { 1 } } };
        CHECK_FALSE(appearanceFromPortal(settings, {}).colorScheme.has_value());
    }
}

TEST_CASE("portal.accessibility", "[portal]")
{
    using contour::platform::accessibilityFromPortal;

    struct Row
    {
        PortalSettings settings;
        AccessibilityPreferences expected;
    };
    auto const defaults = AccessibilityPreferences {};
    auto const rows = std::array {
        Row { .settings = {}, .expected = defaults },
        Row { .settings = { appearanceSetting(ContrastKey, std::uint32_t { 1 }) },
              .expected = { .motion = MotionPreference::Full,
                            .transparency = TransparencyPreference::Allowed,
                            .contrast = ContrastPreference::Increased } },
        Row { .settings = { appearanceSetting(ReducedMotionKey, std::uint32_t { 1 }) },
              .expected = { .motion = MotionPreference::Reduced,
                            .transparency = TransparencyPreference::Allowed,
                            .contrast = ContrastPreference::Standard } },
        Row { .settings = { appearanceSetting(ContrastKey, std::uint32_t { 1 }),
                            appearanceSetting(ReducedMotionKey, std::uint32_t { 1 }) },
              .expected = { .motion = MotionPreference::Reduced,
                            .transparency = TransparencyPreference::Allowed,
                            .contrast = ContrastPreference::Increased } },
        Row { .settings = { appearanceSetting(ContrastKey, std::uint32_t { 0 }),
                            appearanceSetting(ReducedMotionKey, std::uint32_t { 0 }) },
              .expected = defaults },
        // A value of the wrong type counts as absent.
        Row { .settings = { appearanceSetting(ContrastKey, std::string { "1" }) }, .expected = defaults },
    };
    for (auto const& row: rows)
    {
        INFO("settings " << row.settings.size());
        CHECK(accessibilityFromPortal(row.settings) == row.expected);
    }
}

TEST_CASE("portal.merge", "[portal]")
{
    using contour::platform::mergePortalSetting;

    auto const blue = appearanceSetting(AccentColorKey, PortalColor { .red = 0, .green = 0, .blue = 1 });
    auto const red = appearanceSetting(AccentColorKey, PortalColor { .red = 1, .green = 0, .blue = 0 });
    auto const dark = appearanceSetting(ColorSchemeKey, std::uint32_t { 1 });
    auto const settings = PortalSettings { blue, dark };

    SECTION("a new value of a known key replaces it in place")
    {
        auto const merged = mergePortalSetting(settings, red);
        CHECK(merged == PortalSettings { red, dark });
    }

    SECTION("a new key appends")
    {
        auto const contrast = appearanceSetting(ContrastKey, std::uint32_t { 1 });
        CHECK(mergePortalSetting(settings, contrast) == PortalSettings { blue, dark, contrast });
    }

    SECTION("the same key in another namespace is another setting")
    {
        auto other = red;
        other.group = "org.example.other";
        CHECK(mergePortalSetting(settings, other) == PortalSettings { blue, dark, other });
    }

    SECTION("an equal value gives an equal vector")
    {
        // The sources' "no callback for a repeated value" relies on it.
        CHECK(mergePortalSetting(settings, blue) == settings);
    }
}
