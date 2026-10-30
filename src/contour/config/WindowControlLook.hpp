// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <array>
#include <concepts>
#include <span>
#include <string_view>

// How the window controls look on a chrome row: which font and glyphs draw them, which shape they take, and
// which status hue inks each dot. Apart from WindowControlStyle.hpp because it reads the chrome row's
// vocabulary (UiStyle.hpp), which itself includes that header.
namespace contour::config
{

/// The font and glyphs the window controls are drawn with.
struct ControlGlyphs
{
    std::string_view family {};                                 ///< The icon font; empty for the chrome font.
    std::array<std::string_view, WindowControlCount> glyphs {}; ///< By WindowControlKind.
    std::string_view restore {};                                ///< The maximise control's restore face.

    [[nodiscard]] bool operator==(ControlGlyphs const&) const = default;
};

/// The glyphs @p tokens' controls are drawn with: the first installed icon family with its icon glyphs, else
/// the Unicode glyphs in the chrome font. An empty family entry is none, and is never asked about.
/// @param tokens      The window-control style's row.
/// @param isInstalled Whether a font family is installed: QFontDatabase::hasFamily, or a test's fake.
/// @return The glyphs.
template <std::predicate<std::string_view> IsInstalled>
[[nodiscard]] constexpr ControlGlyphs controlGlyphsFor(WindowControlTokens const& tokens,
                                                       IsInstalled const& isInstalled)
{
    auto const named = [&isInstalled](std::string_view family) {
        return !family.empty() && isInstalled(family);
    };
    if (auto const family =
            firstAvailableFamily(std::span<std::string_view const> { tokens.iconFamilies }, named))
        return ControlGlyphs { .family = *family,
                               .glyphs = tokens.iconGlyphs,
                               .restore = tokens.iconRestoreGlyph };
    return ControlGlyphs { .family = {}, .glyphs = tokens.glyphs, .restore = tokens.restoreGlyph };
}

/// The shape @p tokens' controls take on @p row: the Terminal row draws every style's controls as dot cells,
/// on its own grid and inked from the scheme (DotRoles); every other row draws the style's own shape.
/// @param tokens The window-control style's row.
/// @param row    The chrome row.
/// @return The presentation.
[[nodiscard]] constexpr WindowControlPresentation presentationFor(WindowControlTokens const& tokens,
                                                                  ChromeStyle row) noexcept
{
    return row == ChromeStyle::Terminal ? WindowControlPresentation::TrafficLight : tokens.presentation;
}

/// The status hue each control's dot is inked in on the Terminal row, by WindowControlKind: close is the
/// danger, minimise the warning, maximise the success.
inline constexpr std::array<StatusColorRole, WindowControlCount> DotRoles {
    StatusColorRole::Danger,
    StatusColorRole::Warning,
    StatusColorRole::Success,
};

} // namespace contour::config
