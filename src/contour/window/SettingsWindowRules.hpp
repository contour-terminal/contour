// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>

#include <QtCore/QMargins>
#include <QtCore/QRect>
#include <QtCore/QSize>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace contour::window
{

/// Which title-bar buttons the Settings window offers besides Close.
enum class SettingsWindowButtons : uint8_t
{
    All = 0,   //!< Minimize, maximize and close: the OS's default window.
    CloseOnly, //!< Close alone; macOS dims minimize and zoom, as the HIG asks of a settings window.
};

/// How the Settings window presents itself on one host.
struct SettingsWindowConventions
{
    config::HostPlatform host = config::HostPlatform::Other;    ///< The host this row describes.
    std::string_view title {};                                  ///< The window title.
    SettingsWindowButtons buttons = SettingsWindowButtons::All; ///< Its title-bar buttons.
};

/// The conventions per host, in config::HostPlatform enumerator order.
inline constexpr auto SettingsWindowConventionRows = std::array {
    SettingsWindowConventions { .host = config::HostPlatform::Other,
                                .title = "Settings",
                                .buttons = SettingsWindowButtons::CloseOnly },
    SettingsWindowConventions { .host = config::HostPlatform::Windows,
                                .title = "Contour Settings",
                                .buttons = SettingsWindowButtons::All },
    SettingsWindowConventions { .host = config::HostPlatform::MacOS,
                                .title = "Settings",
                                .buttons = SettingsWindowButtons::CloseOnly },
    SettingsWindowConventions { .host = config::HostPlatform::KdePlasma,
                                .title = "Settings",
                                .buttons = SettingsWindowButtons::CloseOnly },
};

static_assert(SettingsWindowConventionRows.size() == config::HostPlatformCount,
              "every HostPlatform needs a Settings window convention row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SettingsWindowConventionRows.size()),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(SettingsWindowConventionRows[i].host)
                                             == i;
                                  }),
              "SettingsWindowConventionRows must list one row per HostPlatform, in enumerator order");

/// The Settings window's conventions on a host.
/// @param host The host.
/// @return Its row of SettingsWindowConventionRows.
[[nodiscard]] constexpr SettingsWindowConventions const& settingsWindowConventionsFor(
    config::HostPlatform host) noexcept
{
    return SettingsWindowConventionRows[static_cast<std::size_t>(host)];
}

/// What deciding where the Settings window goes needs to know, in logical pixels.
struct SettingsWindowPlacement
{
    QSize preferred {};               ///< The row's default size.
    QSize minimum {};                 ///< The row's minimum size.
    std::optional<QRect> previous {}; ///< Where it was last shown; none before its first show.
    QRect anchor {};                  ///< The available area of the screen to open on.
    std::vector<QRect> screens {};    ///< The available area of every screen.
    QMargins frame {};                ///< The native frame around the client area.
};

/// Where the Settings window goes (spec resolution R-c).
/// @param placement What the decision needs to know.
/// @return The client geometry: the previous one kept reachable on the screen it overlaps most, else a
///         centred default clamped to the anchor screen.
[[nodiscard]] QRect settingsWindowGeometry(SettingsWindowPlacement const& placement);

/// What a reopen does with the hidden Settings window's geometry.
enum class SettingsWindowReopen : uint8_t
{
    Place = 0,    ///< Placed by settingsWindowGeometry() before it is shown.
    KeepGeometry, ///< Left alone: its geometry is the maximised or full-screen frame.
};

/// Decides whether a reopen places the Settings window. A maximised or full-screen window reports its
/// maximised frame, and a setGeometry() on it is what a window manager stores as the restore rectangle, so
/// un-maximising after a reopen would give a screen-sized window.
/// @param states The hidden window's states.
/// @return KeepGeometry when @p states holds Maximized or FullScreen, else Place.
[[nodiscard]] inline SettingsWindowReopen settingsWindowReopenFor(Qt::WindowStates states) noexcept
{
    return states.testAnyFlags(Qt::WindowMaximized | Qt::WindowFullScreen)
               ? SettingsWindowReopen::KeepGeometry
               : SettingsWindowReopen::Place;
}

} // namespace contour::window
