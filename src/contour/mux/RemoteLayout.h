// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The attach-mode layout executor (B2): realizes the daemon's captured tab/pane
/// tree into the GUI's own `TerminalSessionManager`, binding each realized pane to
/// its remote session. It composes the tested primitives — `AttachController::
/// wireLayout()` (the daemon layout converted for the shared `realizeLayoutTab`)
/// and `TerminalSessionManager::applyLayoutToWindow(..., beforeLeafSeed)` (which
/// hands each leaf to the seeder just before its backing session is born) — so the
/// GUI reproduces the daemon's split tree instead of flattening one tab per
/// session. Kept a free function so it is driven directly from tests.

#include <vtbackend/primitives.h>

#include <cstdint>
#include <optional>

#include <vtmux/Primitives.h>

namespace contour
{

class AttachController;
class TerminalSessionManager;

/// Reconciles one daemon window's tab/pane tree into @p window of @p manager,
/// binding each realized pane to its remote session (via the controller's
/// beforeLeafSeed → `setNextBindSession` seam). Incremental: realizes tabs not yet
/// shown, catches up intra-tab splits, and closes panes whose remote session
/// vanished. A no-op when the target window's layout has not arrived.
/// @param manager The GUI's session/tab/pane manager (its factory is @p controller).
/// @param window The OS window to build the daemon window's tabs into.
/// @param controller The attach controller holding the captured layouts + bindings.
/// @param daemonWindow Which daemon window to reconcile; nullopt selects the primary
///        (lowest-id) window — the single-window path.
/// @param pageSize The size each realized pane's grid/pty is born at (the live
///        window size), or nullopt for the profile default.
void applyRemoteLayout(TerminalSessionManager& manager,
                       vtmux::WindowId window,
                       AttachController& controller,
                       std::optional<uint64_t> daemonWindow = std::nullopt,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt);

} // namespace contour
