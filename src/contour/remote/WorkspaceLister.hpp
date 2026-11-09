// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Listing a daemon's workspaces -- every one of them, hidden and left ones included -- without blocking the
/// GUI thread. The window switcher resolves a target with it, and the switch directory caches what it says.

#include <contour/remote/SwitchPlan.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vthost/Transport.hpp>

namespace contour::remote
{

/// Which daemon to list the workspaces of, and how to reach it.
struct ListTarget
{
    DaemonAddress address;                                ///< The daemon as a switch names it.
    std::shared_ptr<vthost::ITransportFactory> transport; ///< How to reach it.
};

/// Told once, on the GUI thread, what a daemon's `workspace.list` answered, or why it did not.
using WorkspacesListed =
    std::function<void(std::expected<std::vector<vtcontrol::WorkspaceInfo>, std::string>)>;

/// Lists a daemon's workspaces without blocking the GUI thread: a daemon over ssh may take seconds.
class IWorkspaceLister
{
  public:
    IWorkspaceLister() = default;
    IWorkspaceLister(IWorkspaceLister const&) = delete;
    IWorkspaceLister(IWorkspaceLister&&) = delete;
    IWorkspaceLister& operator=(IWorkspaceLister const&) = delete;
    IWorkspaceLister& operator=(IWorkspaceLister&&) = delete;
    virtual ~IWorkspaceLister() = default;

    /// Lists @p target's workspaces; @p done is told later, on the GUI thread, unless the lister is gone.
    /// @param target The daemon.
    /// @param done Told the workspaces, or why there are none.
    virtual void list(ListTarget target, WorkspacesListed done) = 0;
};

/// The production lister: `workspace.list` over the control API, one call per listing, on a thread of its
/// own; the answer is posted to the GUI thread.
/// @param timeout How long one listing may take.
/// @return The lister.
[[nodiscard]] std::unique_ptr<IWorkspaceLister> makeControlWorkspaceLister(std::chrono::milliseconds timeout);

} // namespace contour::remote
