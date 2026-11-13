// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the application's switching actions and its switcher popup reach: switches, and the listings a
/// switch decides on.

#include <contour/remote/SwitchPlan.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::remote
{

/// Told once, on the GUI thread, a daemon's workspaces as a switch would decide on them, or why it could not
/// list them.
using DaemonWorkspacesListed = std::function<void(std::expected<std::vector<WorkspaceRow>, std::string>)>;

/// Switches windows (session::WindowSwitching), and lists a daemon's workspaces for the switcher popup as a
/// switch of the window would see them. WindowSwitcher is the one the application builds; a test injects a
/// double.
class SwitchingService: public session::WindowSwitching
{
  public:
    /// Lists @p daemon's workspaces for the popup over @p window; never blocks.
    /// @param window The window the popup is over.
    /// @param daemon The daemon; its workspace spec is not read.
    /// @param done Told the rows, in the daemon's order, or why there are none; on the GUI thread, unless the
    ///        service is gone first.
    virtual void listWorkspaces(vtworkspace::WorkspaceId window,
                                session::SwitchTarget const& daemon,
                                DaemonWorkspacesListed done) = 0;

    /// Creates a workspace labelled @p label on @p daemon (`workspace.create`), then switches @p window to
    /// it, or opens it in a new window; never blocks.
    /// @param window The window asking.
    /// @param daemon The daemon; its workspace spec is not read.
    /// @param label The new workspace's label.
    /// @param mode Switch the window, or open a new one.
    /// @param done Told how the switch went, or why no workspace was made.
    virtual void createWorkspace(vtworkspace::WorkspaceId window,
                                 session::SwitchTarget const& daemon,
                                 std::string label,
                                 session::SwitchMode mode,
                                 session::SwitchCompletion done) = 0;

    /// @param window A window.
    /// @return Its own daemon's workspaces as a switch decides on them, from the directory its connection
    ///         holds (nothing is listed); empty for a window no daemon serves.
    [[nodiscard]] virtual std::vector<WorkspaceRow> windowWorkspaceRows(vtworkspace::WorkspaceId window) = 0;
};

} // namespace contour::remote
