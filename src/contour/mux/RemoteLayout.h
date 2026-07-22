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

#include <optional>

#include <vtmux/Primitives.h>

namespace contour
{

class AttachController;
class TerminalSessionManager;

/// Realizes @p controller's current daemon layout into @p window of @p manager,
/// binding each realized pane to its remote session (via the controller's
/// beforeLeafSeed → `setNextBindSession` seam). A no-op when no layout has arrived.
/// Call once per authoritative layout — it CREATES tabs; it does not reconcile an
/// already-built tree.
/// @param manager The GUI's session/tab/pane manager (its factory is @p controller).
/// @param window The window to build the daemon's tabs into.
/// @param controller The attach controller holding the captured layout + bindings.
/// @param pageSize The size each realized pane's grid/pty is born at (the live
///        window size), or nullopt for the profile default.
void applyRemoteLayout(TerminalSessionManager& manager,
                       vtmux::WindowId window,
                       AttachController& controller,
                       std::optional<vtbackend::PageSize> pageSize = std::nullopt);

} // namespace contour
