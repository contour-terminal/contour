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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

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

/// The daemon's windows' layouts by window id, ascending: the primary (lowest-id) window first. Each layout
/// is shared and never changed once held -- a new push replaces it -- so a snapshot is a copy of pointers.
using LayoutSnapshot = std::map<uint64_t, std::shared_ptr<vthost::proto::LayoutState const>>;

/// Reconciles one daemon window's tab/pane tree into @p window of @p manager, binding each realized pane to
/// its remote session (via the controller's beforeLeafSeed → `setNextBindSession` seam). Incremental:
/// realizes tabs not yet shown, catches up intra-tab splits, and closes panes whose remote session
/// vanished. A no-op when the target window's layout has not arrived.
///
/// Against a daemon that moves panes for this client (NativeController::moveSupport), @p layouts is one
/// whole push of every window, so the local tabs are brought to it: each daemon tab is shown by the local
/// tab paired with it, the panes it holds are moved into that tab -- the same TerminalSessions, from
/// whichever tab or window shows them -- and laid out as the daemon's tree, and the tabs are put in the
/// daemon's order. Only a pane no window's layout names is closed. Against any other daemon, a pane this
/// window's layout drops is closed.
/// @param manager The GUI's session/tab/pane manager (its factory is @p controller).
/// @param window The OS window to build the daemon window's tabs into.
/// @param controller The attach controller holding the bindings.
/// @param layouts The layouts to reconcile against: one snapshot, shared by every window of a reconcile.
/// @param daemonWindow Which daemon window to reconcile; nullopt selects the primary (lowest-id) window --
///        the single-window path.
/// @param pageSize The size each realized pane's grid/pty is born at (the live window size), or nullopt
///        for the profile default.
void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       NativeController& controller,
                       LayoutSnapshot const& layouts,
                       std::optional<uint64_t> daemonWindow = std::nullopt,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt);

/// Reconciles as above, against the controller's settled layouts as of now
/// (NativeController::settledLayouts): for a window reconciled on its own.
void applyRemoteLayout(contour::session::TerminalSessionManager& manager,
                       vtworkspace::WindowId window,
                       NativeController& controller,
                       std::optional<uint64_t> daemonWindow = std::nullopt,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt);

/// Forgets the pairing of every local tab that no longer exists (NativeController::forgetClosedTabs): once
/// per reconcile, after every window of it.
/// @param manager The GUI's session/tab/pane manager.
/// @param controller The attach controller holding the pairings.
void forgetClosedTabPairings(contour::session::TerminalSessionManager const& manager,
                             NativeController& controller);

/// Whether a daemon window holds anything to show.
enum class DaemonWindowTabs : uint8_t
{
    None = 0, ///< No tab: a window a reconnect found emptied, given an OS window once it holds one.
    Some,     ///< At least one tab.
};

/// A daemon window the GUI maps onto an OS window.
struct ReconciledWindow
{
    uint64_t daemonWindow = 0;                      ///< The daemon's id of the window.
    DaemonWindowTabs tabs = DaemonWindowTabs::None; ///< Whether it holds a tab.
    std::vector<uint64_t> sessions;                 ///< The daemon sessions its panes show, tab by tab.
};

/// The daemon windows the GUI maps onto OS windows: what applyRemoteLayout() realizes into them.
/// @param layouts The layouts of the reconcile.
/// @return The windows, ascending by id: the primary (lowest-id) window first.
[[nodiscard]] std::vector<ReconciledWindow> windowsToReconcile(LayoutSnapshot const& layouts);

/// The OS window showing each daemon window, by the daemon's id of it.
using AttachWindowMap = std::unordered_map<uint64_t, vtworkspace::WindowId>;

/// Brings every OS window of @p windows to one snapshot of the daemon's layouts: each daemon window an OS
/// window shows is reconciled into it (applyRemoteLayout()), ascending, and every other one is handed to
/// @p openWindow, which adopts or spawns an OS window for it, or leaves it for later.
///
/// No window a move empties closes before every window had its pass, since a later pass may fill it again;
/// then only one whose daemon window holds no tab closes. A mapping whose OS window the reconcile closed is
/// dropped, so a daemon window that holds tabs again later gets an OS window of its own; so is one whose OS
/// window was closed before (by its user) once its daemon window holds no tab or is gone. While that daemon
/// window still holds tabs the mapping stays, so the window its user closed does not open again.
/// @param manager The GUI's session/tab/pane manager (its factory is @p controller).
/// @param controller The attach controller holding the bindings.
/// @param layouts One snapshot of every daemon window's layout.
/// @param windows The OS window showing each daemon window; @p openWindow may add to it.
/// @param openWindow Places a daemon window no OS window shows yet.
void reconcileAttachWindows(contour::session::TerminalSessionManager& manager,
                            NativeController& controller,
                            LayoutSnapshot const& layouts,
                            AttachWindowMap& windows,
                            std::function<void(ReconciledWindow const&)> const& openWindow);

/// The daemon windows as of the end of the last push (NativeController::settledLayouts): a window the first
/// frame of a push still arriving adds is not among them until that push has ended, so it never gets an OS
/// window that would stay blank meanwhile.
/// @param controller The attach controller holding the layouts.
/// @return The windows, ascending by id: the primary (lowest-id) window first.
[[nodiscard]] std::vector<ReconciledWindow> windowsToReconcile(NativeController const& controller);

} // namespace contour::remote
