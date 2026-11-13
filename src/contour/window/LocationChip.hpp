// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the location chip -- the one chip that names a window's daemon and workspace -- decides: the daemon's
/// name, whether its status dot shows, how the chip gives way as its room shrinks, and what assistive
/// technology and the tooltip say. Pure, so the chip, the switcher and the tests read the same rows.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/SidebarTree.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>

#include <vthost/SocketPath.hpp>

namespace contour::window
{

/// @param address   Where the window's daemon was reached.
/// @param hostLabel The window's host label; empty for a local window.
/// @return What the chip calls the daemon: the host label when there is one, else "local" for the default
///         socket label, the label itself, or "daemon" when the address names none.
[[nodiscard]] inline std::string locationDaemonName(session::SwitchTarget const& address,
                                                    std::string_view hostLabel)
{
    if (!hostLabel.empty())
        return std::string(hostLabel);
    if (address.kind == vthost::proto::SwitchDaemonKind::Host && !address.daemon.empty())
        return address.daemon;
    if (address.kind == vthost::proto::SwitchDaemonKind::Label && !address.daemon.empty())
        return address.daemon == vthost::DefaultSocketLabel ? std::string("local") : address.daemon;
    return "daemon";
}

/// Whether the chip's status dot shows.
enum class DaemonDot : uint8_t
{
    Hidden = 0, ///< A local daemon that is up: nothing to say.
    Shown,      ///< A remote daemon, or a connection that is not up.
};

/// @param locality Where the daemon runs.
/// @param state    What the connection looks like.
/// @return Shown for a remote daemon or any state but Connected.
[[nodiscard]] constexpr DaemonDot daemonDotOf(DaemonLocality locality, HostChipState state) noexcept
{
    return locality == DaemonLocality::Remote || state != HostChipState::Connected ? DaemonDot::Shown
                                                                                   : DaemonDot::Hidden;
}

/// How much of the chip fits, from everything down to the workspace name alone.
enum class LocationChipFit : uint8_t
{
    Full = 0,        ///< Daemon, dot, state text and workspace.
    NoStateText,     ///< The state text is dropped.
    DaemonElided,    ///< The daemon name is elided too.
    DotOnly,         ///< The daemon shrinks to its dot.
    WorkspaceElided, ///< Not even that fits: the workspace name is elided.
};

/// Number of @c LocationChipFit enumerators, derived from the last one.
inline constexpr size_t LocationChipFitCount = static_cast<size_t>(LocationChipFit::WorkspaceElided) + 1;

/// The width each fit step needs, in the units of the available room.
struct LocationChipWidths
{
    double full;         ///< For @c LocationChipFit::Full.
    double noStateText;  ///< For @c LocationChipFit::NoStateText.
    double daemonElided; ///< For @c LocationChipFit::DaemonElided.
    double dotOnly;      ///< For @c LocationChipFit::DotOnly.
};

/// The least workspace characters (cells) the chip keeps before the `…`. The caller reserves that width, so
/// the chip never goes narrower.
inline constexpr std::size_t LocationChipWorkspaceFloor = 6;

/// @param widths    What each step needs.
/// @param available The room the chip has.
/// @return The first step whose width fits, @c WorkspaceElided when none does.
[[nodiscard]] constexpr LocationChipFit locationChipFit(LocationChipWidths const& widths,
                                                        double available) noexcept
{
    if (widths.full <= available)
        return LocationChipFit::Full;
    if (widths.noStateText <= available)
        return LocationChipFit::NoStateText;
    if (widths.daemonElided <= available)
        return LocationChipFit::DaemonElided;
    if (widths.dotOnly <= available)
        return LocationChipFit::DotOnly;
    return LocationChipFit::WorkspaceElided;
}

/// What a screen reader says of the chip: the workspace, the daemon it is on and, while the workspace marker
/// shows, what the other workspaces ask (`Workspace billing on starfive, another workspace needs you`).
/// @param daemon  The daemon's name; empty for an in-process window, which names no daemon.
/// @param summary The summary the chip shows.
/// @return The name.
[[nodiscard]] inline std::string locationChipAccessibleName(std::string_view daemon,
                                                            SidebarSummary const& summary)
{
    auto name = "Workspace " + summary.currentWorkspace;
    if (!daemon.empty())
        name += " on " + std::string(daemon);
    if (chipMarkerOf(summary.otherWorkspaces) == ChipMarker::Shown)
        name += ", another workspace " + std::string(urgencyText(summary.otherWorkspaces));
    return name;
}

/// What the chip says of one connection state beyond its dot.
struct LocationChipStateRow
{
    HostChipState state;            ///< The state this row describes.
    std::string_view tooltipBefore; ///< The tooltip's words before the daemon's name; empty for Connected.
    std::string_view tooltipAfter;  ///< The tooltip's words after the daemon's name.
    std::string_view glyph;         ///< The terminal-style stand-in for the dot.
};

/// The rows, indexed by @c HostChipState.
inline constexpr std::array<LocationChipStateRow, HostChipStateCount> LocationChipStateRows {
    LocationChipStateRow { HostChipState::Connected, "", "", "●" },
    LocationChipStateRow { HostChipState::Reconnecting, "Reconnecting to ", "", "◑" },
    LocationChipStateRow { HostChipState::Disconnected, "Disconnected from ", "", "✕" },
    LocationChipStateRow { HostChipState::NeedsAttention, "Reconnecting to ", " needs your attention", "!" },
};

namespace detail
{
    [[nodiscard]] constexpr bool locationChipStateRowsInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, LocationChipStateRows.size()))
            if (LocationChipStateRows[i].state != HostChipState(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::locationChipStateRowsInOrder(),
              "LocationChipStateRows must list the states in enumerator order");

/// @param state What the connection looks like.
/// @return The glyph that stands for the state where a dot cannot be drawn.
[[nodiscard]] constexpr std::string_view daemonStateGlyph(HostChipState state) noexcept
{
    return LocationChipStateRows[static_cast<size_t>(state)].glyph;
}

/// @param state    What the connection looks like.
/// @param expanded The disclosure word ("collapsed" or "expanded").
/// @return The chip's accessible description: the state's word, capitalised, when it is not Connected, then
///         the disclosure.
[[nodiscard]] inline std::string locationChipAccessibleDescription(HostChipState state,
                                                                   std::string_view expanded)
{
    if (state == HostChipState::Connected)
        return std::string(expanded);
    auto word = std::string(hostChipLook(state).accessibleState);
    if (!word.empty())
        word.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(word.front())));
    return word + ", " + std::string(expanded);
}

/// @param state  What the connection looks like.
/// @param fit    How much of the chip fits; the state text is in the tooltip once it is elided.
/// @param daemon The daemon's name; empty for an in-process window.
/// @return The chip's tooltip.
[[nodiscard]] inline std::string locationChipToolTip(HostChipState state,
                                                     LocationChipFit fit,
                                                     std::string_view daemon)
{
    auto const action = std::string(daemon.empty() ? "Switch workspace" : "Switch workspace or daemon");
    auto const& row = LocationChipStateRows[static_cast<size_t>(state)];
    if (fit == LocationChipFit::Full || state == HostChipState::Connected || daemon.empty())
        return action;
    return std::string(row.tooltipBefore) + std::string(daemon) + std::string(row.tooltipAfter) + " · "
           + action;
}

} // namespace contour::window
