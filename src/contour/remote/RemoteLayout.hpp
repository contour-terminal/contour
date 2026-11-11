// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The attach-mode layout executor (B2): realizes the daemon's captured tab/pane
/// tree into the GUI's own `TerminalSessionManager`, binding each realized pane to
/// its remote session. It composes the tested primitives — `NativeController::
/// wireLayout()` (the daemon layout converted for the shared `realizeLayoutTab`)
/// and `TerminalSessionManager::applyLayoutToWindow(..., beforeLeafSeed)` (which
/// hands each leaf to the seeder just before its backing session is born) — so the
/// GUI reproduces the daemon's split tree instead of flattening one tab per
/// session. Kept a free function so it is driven directly from tests.

#include <vtbackend/core/Primitives.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include <vtworkspace/Primitives.hpp>

namespace contour::session
{
class TerminalSessionManager;
}

namespace vthost::proto
{
struct LayoutState;
}

namespace contour::remote
{

class NativeController;

/// The daemon's workspaces' layouts by workspace id, ascending: the primary (lowest-id) workspace first. Each
/// layout is shared and never changed once held -- a new push replaces it -- so a snapshot is a copy of
/// pointers.
using LayoutSnapshot = std::map<uint64_t, std::shared_ptr<vthost::proto::LayoutState const>>;

/// Which of a workspace's tabs and panes a window activates as it applies the daemon's layout.
enum class TabSelection : uint8_t
{
    /// The window's own: the daemon's active tab and pane are whichever window activated one last, and do
    /// not move this window's.
    Keep = 0,
    /// The daemon's: the window shows the workspace for the first time.
    Seed,
};

/// Reconciles one daemon workspace's tab/pane tree into @p window of @p manager, binding each realized pane
/// to its remote session (via the controller's beforeLeafSeed → `setNextBindSession` seam). Incremental:
/// realizes tabs not yet shown, catches up intra-tab splits, and closes panes whose remote session
/// vanished. A no-op when the target workspace's layout has not arrived.
///
/// Against a daemon that moves panes for this client (NativeController::moveSupport), @p layouts is one
/// whole push of every workspace, so the local tabs are brought to it: each daemon tab is shown by the local
/// tab paired with it, the panes it holds are moved into that tab -- the same TerminalSessions, from
/// whichever tab or window shows them -- and laid out as the daemon's tree, and the tabs are put in the
/// daemon's order. Only a pane no workspace's layout names is closed. Against any other daemon, a pane this
/// workspace's layout drops is closed.
/// @param manager The GUI's session/tab/pane manager (its factory is @p controller).
/// @param window The OS window to build the daemon workspace's tabs into.
/// @param controller The attach controller holding the bindings.
/// @param layouts The layouts to reconcile against: one snapshot, shared by every window of a reconcile.
/// @param daemonWorkspace Which daemon workspace to reconcile; nullopt selects the primary (lowest-id)
/// workspace --
///        the single-workspace path.
/// @param pageSize The size each realized pane's grid/pty is born at (the live window size), or nullopt
///        for the profile default.
/// @param selection Whether the daemon's active tab and pane become the window's (Seed), or the window keeps
///        its own (Keep).
void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WorkspaceId window,
                       NativeController& controller,
                       LayoutSnapshot const& layouts,
                       std::optional<uint64_t> daemonWorkspace = std::nullopt,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt,
                       TabSelection selection = TabSelection::Seed);

/// Reconciles as above, against the controller's settled layouts as of now
/// (NativeController::settledLayouts): for a window reconciled on its own.
void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WorkspaceId window,
                       NativeController& controller,
                       std::optional<uint64_t> daemonWorkspace = std::nullopt,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt);

/// Forgets the pairing of every local tab that no longer exists (NativeController::forgetClosedTabs): once
/// per reconcile, after every window of it.
/// @param manager The GUI's session/tab/pane manager.
/// @param controller The attach controller holding the pairings.
void forgetClosedTabPairings(contour::session::TerminalSessionManager const& manager,
                             NativeController& controller);

} // namespace contour::remote
