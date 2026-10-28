// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the Settings window asks the operating system to draw behind its own title bar (R-m). It names
/// Qt::ColorScheme through the request, so it is not one of the Qt-free `window/` headers.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <algorithm>
#include <array>
#include <optional>

namespace contour::window
{

/// What the Settings window's backdrop is decided from, read afresh whenever the window is dressed.
struct SettingsBackdropInputs
{
    config::ChromeStyle row = config::ChromeStyle::ContourLinux; ///< The resolved chrome row.
    config::Material material = config::Material::None;          ///< The row's resolved title-band material.
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;           ///< The chrome's applied light or dark.

    [[nodiscard]] bool operator==(SettingsBackdropInputs const&) const = default;
};

/// One chrome row and material under which the Settings window's title bar shows the OS's material.
struct SettingsBackdropRow
{
    config::ChromeStyle row {};   ///< The chrome row.
    config::Material material {}; ///< Its resolved title-band material.
};

/// The pairs that dress the Settings window's title bar: Mica on the Fluent row. macOS keeps its Settings
/// window as it is (1d R-h), and every other row draws no material.
inline constexpr auto SettingsBackdropRows = std::array {
    SettingsBackdropRow { .row = config::ChromeStyle::Fluent, .material = config::Material::Mica },
};

/// Decides what the Settings window asks of the title-band material.
/// @param row      The resolved chrome row.
/// @param material The row's resolved title-band material: Solid once Reduced transparency, a contrast
///                 theme or an OS without the material took Mica away.
/// @param scheme   The chrome's applied light or dark, which the material follows.
/// @return The native material for a Main window in @p scheme for a @c SettingsBackdropRows pair; nothing
///         for every other pair.
[[nodiscard]] constexpr std::optional<platform::TitleBandRequest> settingsBackdropFor(
    config::ChromeStyle row, config::Material material, Qt::ColorScheme scheme) noexcept
{
    auto const dressed = std::ranges::any_of(SettingsBackdropRows, [&](SettingsBackdropRow const& candidate) {
        return candidate.row == row && candidate.material == material;
    });
    if (!dressed)
        return std::nullopt;
    return platform::TitleBandRequest { .backdrop = config::BandBackdrop::NativeMaterial,
                                        .kind = config::BackdropKind::Main,
                                        .growth = config::TitleBarGrowth::None,
                                        .scheme = scheme };
}

/// @copydoc settingsBackdropFor(config::ChromeStyle, config::Material, Qt::ColorScheme)
/// @param inputs The row, its material and the scheme.
[[nodiscard]] constexpr std::optional<platform::TitleBandRequest> settingsBackdropFor(
    SettingsBackdropInputs const& inputs) noexcept
{
    return settingsBackdropFor(inputs.row, inputs.material, inputs.scheme);
}

} // namespace contour::window
