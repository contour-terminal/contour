// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the platform launchers offer beside the application's icon: the macOS Dock menu and the Windows jump
/// list, each a short list of ways to open a window on a chosen target. Qt-free and a table: the adapters
/// (platform/DockMenu, platform/Win32JumpList) only render what is decided here, so the decisions test
/// without the OS.

#include <contour/config/LaunchTarget.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace contour::command
{

/// The launcher surfaces that list targets.
enum class LauncherSurface : uint8_t
{
    DockMenu = 0, ///< macOS: the menu on the Dock icon.
    JumpList,     ///< Windows: the Tasks of the taskbar button's jump list.
};

/// How a surface words and limits its entries. Adding a surface is adding a row.
struct LauncherSurfaceRow
{
    LauncherSurface surface;      ///< The surface.
    std::string_view pickerTitle; ///< The entry that opens the Connect picker.
    std::size_t maxHosts;         ///< The most saved hosts it lists.
};

/// One row per surface, in enumerator order.
inline constexpr auto LauncherSurfaceRows = std::array {
    LauncherSurfaceRow {
        .surface = LauncherSurface::DockMenu, .pickerTitle = "New Window to…", .maxHosts = 12 },
    LauncherSurfaceRow { .surface = LauncherSurface::JumpList, .pickerTitle = "Connect…", .maxHosts = 10 },
};

static_assert(LauncherSurfaceRows.size() == 2, "one row per LauncherSurface");

/// One entry a launcher shows: what it reads, and the command line it runs.
struct LauncherEntry
{
    std::string title;                  ///< What the entry reads.
    std::string detail;                 ///< A second line (a host's address), where the surface has room.
    std::vector<std::string> arguments; ///< The arguments after the program (`connect starfive`).

    [[nodiscard]] bool operator==(LauncherEntry const&) const = default;
};

/// The entries a surface shows: the one that opens the Connect picker, then the saved hosts by name.
/// @param surface Which launcher.
/// @param hosts The saved hosts.
/// @param configPath The configuration file the launched processes load; empty for the default one.
/// @return The entries, picker first; at most the surface's host limit of hosts follow.
[[nodiscard]] std::vector<LauncherEntry> launcherEntries(
    LauncherSurface surface,
    std::map<std::string, config::HostEntry> const& hosts,
    std::string_view configPath);

} // namespace contour::command
