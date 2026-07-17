// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <QtCore/Qt>

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

} // namespace contour
