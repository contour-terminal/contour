// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What serves a window's sessions: this process, a daemon, or a host that is no daemon. The one answer the
/// location chip and the switcher both ask, so neither derives it from a name or a binding of its own.

#include <contour/window/EnumFromInt.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace contour::window
{

/// What serves a window's sessions, which decides what its chrome names and what its switcher can list.
enum class DaemonHosting : uint8_t
{
    InProcess = 0, ///< No binding: the window hosts its own sessions and shows the workspaces it owns.
    Daemon,        ///< The sessions live in a daemon: its chip names it, and its switcher lists every daemon.
    OtherHost,     ///< A host that is no daemon (a tmux attach): bound, but with no daemon to name or list.
};

/// Number of @c DaemonHosting enumerators, derived from the last one.
inline constexpr std::size_t DaemonHostingCount = std::to_underlying(DaemonHosting::OtherHost) + 1;

/// Whether the location chip names a daemon.
enum class ChipNaming : uint8_t
{
    WorkspaceOnly = 0, ///< The chip is the workspace alone; there is no daemon to name.
    NamesDaemon,       ///< The chip names the daemon: its segment, its dot, and the sidebar's head control.
};

/// How far the switcher the window opens may reach.
enum class SwitcherReach : uint8_t
{
    AsAsked = 0,      ///< Whatever scope the action asked for.
    OwnWorkspacesOnly ///< The window's own workspaces alone, whatever was asked.
};

/// One row of the hosting-policy table: what a kind of hosting means for the chrome that names it and the
/// switcher it opens.
struct DaemonHostingPolicy
{
    DaemonHosting hosting; ///< The hosting this row describes.
    ChipNaming naming;     ///< Whether the chip names a daemon, and so where it may show (see below).
    SwitcherReach reach;   ///< How far the switcher the window opens reaches.
};

/// The policy of every hosting, indexed by @c DaemonHosting. The one place the chrome and the switcher read
/// what a kind of hosting entitles a window to: a chip that names a daemon follows the band's centre and may
/// head the sidebar, one that does not shows only in the centre that holds the workspace chip; an in-process
/// window lists its own workspaces alone, one attached to a host that is no daemon (tmux) keeps what was
/// asked, as it always has.
inline constexpr std::array<DaemonHostingPolicy, DaemonHostingCount> DaemonHostingPolicies {
    DaemonHostingPolicy { .hosting = DaemonHosting::InProcess,
                          .naming = ChipNaming::WorkspaceOnly,
                          .reach = SwitcherReach::OwnWorkspacesOnly },
    DaemonHostingPolicy { .hosting = DaemonHosting::Daemon,
                          .naming = ChipNaming::NamesDaemon,
                          .reach = SwitcherReach::AsAsked },
    DaemonHostingPolicy { .hosting = DaemonHosting::OtherHost,
                          .naming = ChipNaming::WorkspaceOnly,
                          .reach = SwitcherReach::AsAsked },
};

static_assert(enumValuesAreDense(DaemonHostingPolicies, &DaemonHostingPolicy::hosting),
              "DaemonHostingPolicies must list the hostings in enumerator order");

/// @param hosting What serves a window.
/// @return Its row in @c DaemonHostingPolicies.
[[nodiscard]] constexpr DaemonHostingPolicy const& daemonHostingPolicy(DaemonHosting hosting) noexcept
{
    return DaemonHostingPolicies[std::to_underlying(hosting)];
}

} // namespace contour::window
