// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Turns the daemon's `LayoutState` into an ordered plan of model operations that
/// rebuild its tab/pane trees locally. This is the pure, GUI-free decision at the
/// heart of B2: a frontend replays the plan against its own tab/split model
/// (`createTab` / `splitActivePane` / `setActivePane`) — or a thin client ignores
/// it — while the mirror keeps feeding cells. Kept dependency-free and headless so
/// the tree-flattening logic is unit-tested against a real `vtmux::SessionModel`
/// without standing up the Qt GUI.

#include <cstdint>
#include <vector>

#include <muxserver/proto/Pdu.h>

namespace muxserver::client
{

/// One step to rebuild a daemon tab/pane tree. Replaying a tab's steps in order
/// against a fresh model reproduces that tab's split tree — structure,
/// orientations, ratios and per-leaf sessions.
struct ReconstructStep
{
    enum class Kind : uint8_t
    {
        NewTab,   ///< Create a new tab; its single root leaf takes @ref session.
        Split,    ///< Split the ACTIVE pane (@ref orientation, @ref ratio); the new pane takes @ref session.
        Activate, ///< Make the leaf carrying @ref session active (so the next Split targets it).
    };

    Kind kind;
    uint64_t session = 0;    ///< NewTab/Split: the placed leaf's session. Activate: the leaf to activate.
    uint8_t orientation = 0; ///< Split only: vtmux::SplitState (1 horizontal, 2 vertical).
    uint16_t ratio = 5000;   ///< Split only: the split ratio × 10000.

    bool operator==(ReconstructStep const&) const = default;
};

/// Produces the reconstruction plan for @p layout: the steps for tab 0, then tab
/// 1, and so on.
///
/// The plan relies on the split invariant the daemon itself used (see
/// `vtmux::Pane::split`): splitting a leaf keeps the OLD session in the first
/// child and puts the NEW session in the second — which then becomes active. So a
/// subtree's leftmost leaf is always the pane that seeded it, and rebuilding a
/// split means "split the active (leftmost) pane, giving the new pane the right
/// subtree's leftmost session, then build the right subtree (now active), then
/// re-activate the left leaf and build the left subtree".
///
/// @param layout The daemon's whole window layout.
/// @return The ordered steps; empty when the layout has no tabs.
[[nodiscard]] std::vector<ReconstructStep> planReconstruction(proto::LayoutState const& layout);

} // namespace muxserver::client
