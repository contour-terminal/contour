// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the daemon and workspace switching actions, and the palette rows built for them, need to know:
/// which local daemons exist, which hosts windows show, and which workspaces a window's daemon has. All of
/// it is answered from a cache, so asking never blocks the GUI thread.

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::session
{

/// Whether a workspace is the one the asking window shows.
enum class WorkspaceRole : uint8_t
{
    Other = 0, ///< Another workspace of the daemon: shown elsewhere, hidden or left.
    Current,   ///< The one the asking window shows.
};

/// One workspace of the daemon a window is on.
struct KnownWorkspace
{
    uint32_t position = 0; ///< 1-based position among the daemon's workspaces: a valid spec.
    std::string name;      ///< Label, else the active tab's title, else "workspace N".
    WorkspaceRole role = WorkspaceRole::Other; ///< Whether the asking window shows it.
};

/// Answers what the switching surfaces list. Implemented by the application, which owns the discovery and
/// the daemon links; session/ only asks.
class SwitchDirectory
{
  public:
    SwitchDirectory() = default;
    SwitchDirectory(SwitchDirectory const&) = delete;
    SwitchDirectory(SwitchDirectory&&) = delete;
    SwitchDirectory& operator=(SwitchDirectory const&) = delete;
    SwitchDirectory& operator=(SwitchDirectory&&) = delete;
    virtual ~SwitchDirectory() = default;

    /// @return The local daemons found by the last discovery, `default` first; empty until one answered.
    [[nodiscard]] virtual std::vector<cli::DiscoveredDaemon> localDaemons() const = 0;

    /// @return The saved hosts some window of this process shows.
    [[nodiscard]] virtual std::vector<std::string> hostsShownHere() const = 0;

    /// @param window The asking window.
    /// @return The workspaces of the daemon @p window is on, in position order; empty when unknown.
    [[nodiscard]] virtual std::vector<KnownWorkspace> workspacesOf(vtworkspace::WorkspaceId window) const = 0;

    /// Starts refreshing what the lists above answer from, without waiting for it. Called when a surface
    /// that lists them opens.
    virtual void refresh() = 0;
};

} // namespace contour::session
