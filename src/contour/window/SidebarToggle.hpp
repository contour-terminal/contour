// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenuState.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

// Where the button that hides and shows the workspace sidebar stands, and what it says. Qt-free: Main.qml
// asks UiStyleProvider::sidebarToggle(), which answers from here, and places the one button where it says.
//
// The rule the table keeps: the button stays where the pointer left it. It leads the band, after the window's
// own controls (beside the traffic lights on macOS, at the header bar's start on ContourLinux), whether the
// sidebar is shown or not -- except where a shown sidebar stands beside the band and takes its leading end
// (the macOS source list), or where the band does not rest on screen: there the sidebar's own title row
// holds it, at the same spot.
namespace contour::window
{

/// Where the sidebar's toggle stands.
enum class SidebarToggleHost : uint8_t
{
    None = 0, //!< Nowhere on screen: the band is hidden and so is the sidebar; the key and palette remain.
    Band,     //!< The band's leading end, after the window's own controls.
    SidebarTitleRow, //!< The sidebar's title row, at its leading end, after the OS's controls over it.
};

/// Number of @c SidebarToggleHost enumerators, derived from the last one.
inline constexpr size_t SidebarToggleHostCount = static_cast<size_t>(SidebarToggleHost::SidebarTitleRow) + 1;

/// What the sidebar's toggle depends on.
struct SidebarToggleInputs
{
    config::SidebarSpan span = config::SidebarSpan::UnderBand; ///< Where the row's sidebar stands.
    config::SidebarVisibility visibility = config::SidebarVisibility::Hidden; ///< Whether it is shown.
    BarPresence band = BarPresence::Shown;                                    ///< Whether the band is.
    BarEdge bandEdge = BarEdge::Top;                                          ///< Where the band is.
};

/// The sidebar's toggle.
struct SidebarToggle
{
    SidebarToggleHost host = SidebarToggleHost::None; ///< Where it stands.
    std::string_view label;                           ///< Its accessible name and its tooltip's label.

    [[nodiscard]] bool operator==(SidebarToggle const&) const = default;
};

/// What the toggle says, by config::SidebarVisibility: what clicking it does.
inline constexpr auto SidebarToggleLabels = std::array<std::string_view, config::SidebarVisibilityCount> {
    "Show sidebar", // Hidden
    "Hide sidebar", // Shown
};

/// @param host Where the toggle stands.
/// @return The name QML reads it by: "none", "band" or "sidebar".
[[nodiscard]] constexpr std::string_view sidebarToggleHostName(SidebarToggleHost host) noexcept
{
    switch (host)
    {
        case SidebarToggleHost::None: return "none";
        case SidebarToggleHost::Band: return "band";
        case SidebarToggleHost::SidebarTitleRow: return "sidebar";
    }
    return "none";
}

/// @param inputs The row's sidebar, whether it is shown, and the band's presence and edge.
/// @return Where the toggle stands and what it says.
[[nodiscard]] constexpr SidebarToggle sidebarToggleOf(SidebarToggleInputs const& inputs) noexcept
{
    auto const shown = inputs.visibility == config::SidebarVisibility::Shown;
    // A sidebar beside a band at the top takes the band's leading end; one that does not rest on screen
    // (hidden, or revealed only at the screen's edge) leaves the sidebar's own title row as the steady place.
    auto const besideBand = inputs.span == config::SidebarSpan::BesideBand && inputs.bandEdge == BarEdge::Top;
    auto const bandRests = inputs.band == BarPresence::Shown || inputs.band == BarPresence::Pinned;
    auto const host = [&] {
        if (shown && (besideBand || !bandRests))
            return SidebarToggleHost::SidebarTitleRow;
        return inputs.band != BarPresence::Hidden ? SidebarToggleHost::Band : SidebarToggleHost::None;
    }();
    return SidebarToggle { .host = host,
                           .label = SidebarToggleLabels[static_cast<size_t>(inputs.visibility)] };
}

/// @param name A host as sidebarToggleHostName() names it.
/// @return The host; None for a name no host has.
[[nodiscard]] constexpr SidebarToggleHost sidebarToggleHostNamed(std::string_view name) noexcept
{
    for (auto const index: std::views::iota(size_t { 0 }, SidebarToggleHostCount))
        if (auto const host = static_cast<SidebarToggleHost>(index); sidebarToggleHostName(host) == name)
            return host;
    return SidebarToggleHost::None;
}

/// Whether the pane's context menu must offer the sidebar back: whatever hides it can be undone by pointer,
/// and with no toggle on screen (no band to hold one) the pane's menu is the pointer's way back.
/// @param visibility Whether the sidebar is shown.
/// @param host       Where its toggle stands.
/// @return Offered while the sidebar is hidden and its toggle stands nowhere.
[[nodiscard]] constexpr command::SidebarRecovery sidebarRecoveryOf(config::SidebarVisibility visibility,
                                                                   SidebarToggleHost host) noexcept
{
    return visibility == config::SidebarVisibility::Hidden && host == SidebarToggleHost::None
               ? command::SidebarRecovery::Offered
               : command::SidebarRecovery::None;
}

} // namespace contour::window
