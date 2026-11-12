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

/// Whether a source took a request to show a workspace.
enum class SelectResult : uint8_t
{
    Refused = 0, ///< Nothing was asked.
    Requested,   ///< The window will show the workspace, or already does.
};

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

    /// Has the window show @p workspace. GUI thread.
    /// @param workspace The workspace's id, as directory() lists it (DirectoryWorkspace::id).
    /// @return Requested when the window will show it (at once in-process, once the daemon states it for a
    ///         daemon); Refused for a workspace the source does not list, or a connection that cannot ask.
    ///         A daemon may still refuse later; the window then keeps what it shows.
    virtual SelectResult select(uint64_t workspace) = 0;
};

} // namespace contour::window
