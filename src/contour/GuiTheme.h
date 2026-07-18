// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <QtCore/Qt>
#include <QtGui/QColor>
#include <QtGui/QPalette>

#include <array>
#include <optional>

namespace contour
{

/// Maps the configured GUI theme to the Qt color-scheme override that should be applied to the
/// application's chrome (title bar, tab strip, command palette, settings pages, dialogs).
///
/// This is a pure decision, deliberately free of any @c QGuiApplication / display dependency so it
/// can be unit-tested headlessly; the caller applies the result through
/// @c QStyleHints::setColorScheme / @c unsetColorScheme.
///
/// @param theme The configured GUI theme.
/// @return The Qt color scheme to force, or @c std::nullopt for @c GuiTheme::System (defer to the
///         operating system's color scheme, i.e. @c unsetColorScheme).
[[nodiscard]] constexpr std::optional<Qt::ColorScheme> qtColorSchemeFor(config::GuiTheme theme) noexcept
{
    switch (theme)
    {
        case config::GuiTheme::System: return std::nullopt;
        case config::GuiTheme::Dark: return Qt::ColorScheme::Dark;
        case config::GuiTheme::Light: return Qt::ColorScheme::Light;
    }
    return std::nullopt;
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

} // namespace contour
