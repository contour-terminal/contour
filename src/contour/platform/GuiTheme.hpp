// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <QtCore/Qt>
#include <QtGui/QColor>
#include <QtGui/QPalette>

#include <array>
#include <optional>
#include <variant>

namespace contour::platform
{

/// The luminance below which a terminal background reads as dark (WCAG relative luminance, 0 to 1).
inline constexpr double DarkBackgroundLuminance = 0.18;

/// Whether a terminal background of the given color calls for a dark or a light chrome.
/// @param background The terminal's default background color.
/// @return @c Dark when its relative luminance is below @c DarkBackgroundLuminance, else @c Light.
[[nodiscard]] inline Qt::ColorScheme colorSchemeOfBackground(vtbackend::RGBColor background) noexcept
{
    return vtbackend::relativeLuminance(background) < DarkBackgroundLuminance ? Qt::ColorScheme::Dark
                                                                              : Qt::ColorScheme::Light;
}

/// The GUI theme in effect while the operating system does or does not run a contrast theme.
///
/// A contrast theme owns the palette (R-l): forcing a scheme, the terminal's included, would paint over
/// the very colours the user chose for legibility, so under one the chrome defers to the system palette.
/// @param configured The configured GUI theme.
/// @param contrast Whether the operating system runs a contrast theme.
/// @return @c GuiTheme::System under @c ContrastTheme::Active, else @p configured.
[[nodiscard]] constexpr config::GuiTheme effectiveGuiTheme(config::GuiTheme configured,
                                                           ContrastTheme contrast) noexcept
{
    switch (contrast)
    {
        case ContrastTheme::None: return configured;
        case ContrastTheme::Active: return config::GuiTheme::System;
    }
    return configured;
}

/// Maps the configured GUI theme to the Qt color-scheme override that should be applied to the
/// application's chrome (title bar, tab strip, command palette, settings pages, dialogs).
///
/// This is a pure decision, deliberately free of any @c QGuiApplication / display dependency so it
/// can be unit-tested headlessly; the caller applies the result through
/// @c QStyleHints::setColorScheme / @c unsetColorScheme.
///
/// @param theme The configured GUI theme.
/// @param colors The default profile's color configuration, consulted for @c GuiTheme::Terminal.
/// @param liveBackground The focused session's default background as a program set it at runtime
///        (OSC 11), if known. Over a simple color configuration it outranks the configured background;
///        over a dual one it is ignored, since the chrome then follows the OS.
/// @return The Qt color scheme to force, or @c std::nullopt to defer to the operating system's color
///         scheme (i.e. @c unsetColorScheme): for @c GuiTheme::System, and for @c GuiTheme::Terminal
///         over a dual (dark/light) color configuration, which already follows the OS.
[[nodiscard]] inline std::optional<Qt::ColorScheme> qtColorSchemeFor(
    config::GuiTheme theme,
    config::ColorConfig const& colors,
    std::optional<vtbackend::RGBColor> liveBackground) noexcept
{
    switch (theme)
    {
        case config::GuiTheme::System: return std::nullopt;
        case config::GuiTheme::Dark: return Qt::ColorScheme::Dark;
        case config::GuiTheme::Light: return Qt::ColorScheme::Light;
        case config::GuiTheme::Terminal:
            if (auto const* simple = std::get_if<config::SimpleColorConfig>(&colors))
                return colorSchemeOfBackground(liveBackground.value_or(simple->colors.defaultBackground));
            return std::nullopt;
    }
    return std::nullopt;
}

/// The color scheme for @p theme over the configured background alone, with no live background.
/// @param theme The configured GUI theme.
/// @param colors The default profile's color configuration.
/// @return As the three-argument form with @c std::nullopt for the live background.
[[nodiscard]] inline std::optional<Qt::ColorScheme> qtColorSchemeFor(
    config::GuiTheme theme, config::ColorConfig const& colors) noexcept
{
    return qtColorSchemeFor(theme, colors, std::nullopt);
}

/// The terminal's light/dark preference from what the operating system says.
/// @param published The preference the platform appearance source publishes, if any (the XDG Settings
///        portal's @c color-scheme on Linux).
/// @param qtScheme Qt's own reading of the OS colour scheme (@c QStyleHints::colorScheme).
/// @return @p published when set; otherwise @c Dark for @c Qt::ColorScheme::Dark and @c Light for anything
///         else.
[[nodiscard]] inline vtbackend::ColorPreference osColorPreference(
    std::optional<vtbackend::ColorPreference> published, Qt::ColorScheme qtScheme) noexcept
{
    if (published)
        return *published;
    return qtScheme == Qt::ColorScheme::Dark ? vtbackend::ColorPreference::Dark
                                             : vtbackend::ColorPreference::Light;
}

/// The terminal's light/dark preference once the GUI theme is taken into account.
///
/// A chrome pinned to dark or light carries the terminal with it: a dark chrome around a light grid (or the
/// reverse) is the one combination nobody picks on purpose. Every other theme defers to the OS, `Terminal`
/// included, as that theme derives the chrome from the terminal and not the other way round.
/// @param effectiveTheme The GUI theme in effect (see @ref effectiveGuiTheme).
/// @param os What the OS says (see @ref osColorPreference).
/// @return @c Dark or @c Light for the matching pinned theme, else @p os.
[[nodiscard]] constexpr vtbackend::ColorPreference terminalColorPreference(
    config::GuiTheme effectiveTheme, vtbackend::ColorPreference os) noexcept
{
    switch (effectiveTheme)
    {
        case config::GuiTheme::Dark: return vtbackend::ColorPreference::Dark;
        case config::GuiTheme::Light: return vtbackend::ColorPreference::Light;
        case config::GuiTheme::System:
        case config::GuiTheme::Terminal: return os;
    }
    return os;
}

namespace detail
{
    /// One row of the forced-theme palette table: a Qt palette role paired with its light and dark
    /// colors, each packed as @c 0xRRGGBB. Kept as plain integers so the whole table stays
    /// @c constexpr (@c QColor's constructors are not @c constexpr).
    struct ThemePaletteRow
    {
        QPalette::ColorRole role; ///< The palette role this row colors.
        unsigned light;           ///< Color for @c Qt::ColorScheme::Light, packed @c 0xRRGGBB.
        unsigned dark;            ///< Color for @c Qt::ColorScheme::Dark, packed @c 0xRRGGBB.
    };

    /// Core palette roles applied to every color group. Values follow Qt's canonical Fusion
    /// light/dark palettes so the pinned Fusion style and every QML @c SystemPalette recolor
    /// consistently across platforms. Adding a role themed by the forced theme is a new row here.
    constexpr std::array ThemePaletteRows {
        // clang-format off
        ThemePaletteRow { QPalette::Window,          0xefefef, 0x353535 },
        ThemePaletteRow { QPalette::WindowText,      0x000000, 0xffffff },
        ThemePaletteRow { QPalette::Base,            0xffffff, 0x191919 },
        ThemePaletteRow { QPalette::AlternateBase,   0xf7f7f7, 0x353535 },
        ThemePaletteRow { QPalette::ToolTipBase,     0xffffff, 0x353535 },
        ThemePaletteRow { QPalette::ToolTipText,     0x000000, 0xffffff },
        ThemePaletteRow { QPalette::PlaceholderText, 0x7f7f7f, 0x7f7f7f },
        ThemePaletteRow { QPalette::Text,            0x000000, 0xffffff },
        ThemePaletteRow { QPalette::Button,          0xefefef, 0x353535 },
        ThemePaletteRow { QPalette::ButtonText,      0x000000, 0xffffff },
        ThemePaletteRow { QPalette::BrightText,      0xff0000, 0xff0000 },
        ThemePaletteRow { QPalette::Link,            0x2a82da, 0x2a82da },
        ThemePaletteRow { QPalette::Highlight,       0x2a82da, 0x2a82da },
        ThemePaletteRow { QPalette::HighlightedText, 0xffffff, 0x000000 },
        // clang-format on
    };

    /// Overrides applied to the @c QPalette::Disabled group only, so greyed-out chrome stays legible
    /// (and visibly disabled) in both themes.
    constexpr std::array ThemeDisabledRows {
        detail::ThemePaletteRow { QPalette::WindowText, 0x7f7f7f, 0x7f7f7f },
        detail::ThemePaletteRow { QPalette::Text, 0x7f7f7f, 0x7f7f7f },
        detail::ThemePaletteRow { QPalette::ButtonText, 0x7f7f7f, 0x7f7f7f },
    };
} // namespace detail

/// Builds the explicit application @c QPalette that forces @p scheme onto the GUI chrome.
///
/// This is the load-bearing half of the theme seam: on platforms whose platform-theme plugin owns
/// the application palette (KDE Plasma, GNOME, …), @c QStyleHints::setColorScheme does @b not
/// regenerate @c QGuiApplication::palette(), so the chrome never recolors. Setting this palette
/// explicitly via @c QGuiApplication::setPalette does, and every QML @c SystemPalette follows it.
///
/// The colors come from @c detail::ThemePaletteRows / @c ThemeDisabledRows (data-driven), so this
/// function is a pure interpreter of that table and is unit-tested headlessly.
///
/// @param scheme The color scheme to force (@c Qt::ColorScheme::Dark or @c Light).
/// @return A fully populated palette for @p scheme.
[[nodiscard]] inline QPalette buildThemePalette(Qt::ColorScheme scheme)
{
    auto const isDark = scheme == Qt::ColorScheme::Dark;
    auto const pick = [isDark](detail::ThemePaletteRow const& row) {
        return QColor::fromRgb(isDark ? row.dark : row.light);
    };

    auto palette = QPalette {};
    for (auto const& row: detail::ThemePaletteRows)
        palette.setColor(row.role, pick(row));
    for (auto const& row: detail::ThemeDisabledRows)
        palette.setColor(QPalette::Disabled, row.role, pick(row));
    return palette;
}

} // namespace contour::platform
