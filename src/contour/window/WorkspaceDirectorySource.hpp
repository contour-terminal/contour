// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/AttentionJump.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/PaneSeen.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

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

    /// @return Who keeps the attention flags of the panes directory() lists: the daemon, for a source whose
    ///         daemon shares them; the window's own ledger otherwise. GUI thread.
    [[nodiscard]] virtual FlagSource flagSource() const = 0;

    /// Tells the daemon that keeps @p pane's flags (FlagSource::Daemon) that the window showed it and saw it
    /// up to @p seen: the daemon clears what that covers, for every window. Nothing for a source whose panes'
    /// flags the window keeps. GUI thread.
    /// @param pane The pane, as directory() lists it.
    /// @param seen The serials and the status generation the window saw of it.
    virtual void markSeen(PaneKey pane, PaneSeen seen) = 0;

    /// Tells the daemon behind the source whether the window has the user's focus and which of its panes it
    /// shows, in full each time: a pane a focused window shows is watched, so the daemon keeps it seen. The
    /// window's one report of either, made on every focus and visibility change. Nothing for a source with
    /// no daemon. GUI thread.
    /// @param focus The window's focus now.
    /// @param panes The panes the window shows now (platform::SessionVisibility other than Hidden).
    virtual void reportShown(WindowFocusState focus, std::vector<PaneKey> panes) = 0;

    /// @return The marks of every pane whose flags the daemon keeps (FlagSource::Daemon), by PaneKey value:
    ///         what a tab strip reads for those panes instead of the window's ledger. Taken from directory()
    ///         unless overridden, as a source that keeps the flags apart from the layouts does, to spare
    ///         listing every workspace for them. GUI thread.
    [[nodiscard]] virtual std::unordered_map<uint64_t, SessionAttention> daemonMarks() const
    {
        auto marks = std::unordered_map<uint64_t, SessionAttention> {};
        for (auto const& workspace: directory().workspaces)
            for (auto const& tab: workspace.tabs)
                for (auto const& pane: tab.panes)
                    if (pane.flagSource == FlagSource::Daemon)
                        marks.insert_or_assign(pane.key.value, daemonMarksOf(pane));
        return marks;
    }
};

} // namespace contour::window
