// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Converts the daemon's `LayoutState` into a `vtmux::Layout` that the shared
/// realizer (`vtmux::realizeLayoutTab`) rebuilds locally — the GUI-free half of
/// B2. Reusing `realizeLayoutTab` (the same primitive the daemon and the config
/// loader use) keeps ONE tree-reconstruction path in the codebase; a frontend
/// only supplies a seeder that binds each realized pane to its remote session,
/// while the mirror keeps feeding cells. Kept dependency-free so it is unit-tested
/// headless against a real `vtmux::SessionModel`.

#include <cstdint>
#include <unordered_map>

#include <muxserver/proto/Pdu.h>
#include <vtmux/LayoutTree.h>

namespace muxserver::client
{

/// A daemon layout ready for `vtmux::realizeLayoutTab`, plus the map from each
/// realized LEAF (keyed by its address inside @ref layout) to the remote session
/// that backs it. The realizer's seeder receives each leaf by const reference, so
/// it recovers the session with `leafSession.at(&leaf)`.
///
/// The addresses are stable for the whole object's lifetime — the map is built
/// only after the tree is complete, and moving the struct preserves the pane
/// addresses (a moved `std::vector` steals its buffer). Do not mutate @ref layout
/// after conversion.
struct WireLayout
{
    vtmux::Layout layout;
    std::unordered_map<vtmux::LayoutPane const*, uint64_t> leafSession;
};

/// Converts a single daemon wire pane subtree into a `vtmux::LayoutPane`: a split
/// keeps its orientation and gives the FIRST child the wire ratio (the second
/// takes the rest, matching `vtmux::ratioForFirst`); a leaf becomes an empty,
/// command-less pane (a remote session backs it — the seeder binds it). The
/// per-leaf remote session is not recorded here; use @ref wireToLayout for that.
[[nodiscard]] vtmux::LayoutPane wireToLayoutPane(proto::WirePane const& pane);

/// Converts the daemon's whole layout into a realizable `vtmux::Layout` (one
/// `LayoutTab` per wire tab) and records each leaf's remote session.
/// @param state The daemon's window layout.
/// @return The converted layout plus its leaf → remote-session map.
[[nodiscard]] WireLayout wireToLayout(proto::LayoutState const& state);

} // namespace muxserver::client
