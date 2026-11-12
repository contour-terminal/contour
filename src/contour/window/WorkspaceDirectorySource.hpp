// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/WorkspaceDirectory.hpp>

#include <cstdint>
#include <functional>

namespace contour::window
{

/// Told, on the GUI thread, that what a source's directory() lists changed. An implementation detail of the
/// sources that serve one window (InProcessDirectory), which is given it once, as it is constructed.
using DirectoryChanged = std::function<void()>;

/// Where a window gets the workspaces it can list from: a daemon's, or its own process's. Both kinds of
/// window list the same WorkspaceDirectory, so a sidebar reads one interface and never asks which it has.
///
/// How a source announces a change is not part of the interface, and a window's user need not know it:
/// whichever source a window has, the window raises WindowController::workspaceDirectoryChanged when what
/// directory() lists changes, and when the source is replaced.
class IWorkspaceDirectorySource
{
  public:
    IWorkspaceDirectorySource() = default;
    IWorkspaceDirectorySource(IWorkspaceDirectorySource const&) = delete;
    IWorkspaceDirectorySource& operator=(IWorkspaceDirectorySource const&) = delete;
    IWorkspaceDirectorySource(IWorkspaceDirectorySource&&) = delete;
    IWorkspaceDirectorySource& operator=(IWorkspaceDirectorySource&&) = delete;
    virtual ~IWorkspaceDirectorySource() = default;

    /// @return Every workspace the window can list, with its tabs, panes and what is known of them. GUI
    ///         thread.
    [[nodiscard]] virtual WorkspaceDirectory directory() const = 0;

    /// Has the window show @p workspace. Nothing for a workspace the source does not list, or one already
    /// shown. GUI thread.
    /// @param workspace The workspace's id, as directory() lists it (DirectoryWorkspace::id).
    virtual void select(uint64_t workspace) = 0;
};

} // namespace contour::window
