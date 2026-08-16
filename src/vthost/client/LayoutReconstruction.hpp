// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Converts the daemon's `LayoutState` into a `vtworkspace::Layout` that the shared
/// realizer (`vtworkspace::realizeLayoutTab`) rebuilds locally — the GUI-free half of
/// B2. Reusing `realizeLayoutTab` (the same primitive the daemon and the config
/// loader use) keeps ONE tree-reconstruction path in the codebase; a frontend
/// only supplies a seeder that binds each realized pane to its remote session,
/// while the mirror keeps feeding cells. Kept dependency-free so it is unit-tested
/// headless against a real `vtworkspace::SessionModel`.

#include <vtpty/PageSize.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/LayoutTree.hpp>

namespace vthost::client
{

/// A daemon layout ready for `vtworkspace::realizeLayoutTab`, plus the map from each
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
    vtworkspace::Layout layout;
    std::unordered_map<vtworkspace::LayoutPane const*, uint64_t> leafSession;
};

/// Converts a single daemon wire pane subtree into a `vtworkspace::LayoutPane`: a split
/// keeps its orientation and gives the FIRST child the wire ratio (the second
/// takes the rest, matching `vtworkspace::ratioForFirst`); a leaf becomes an empty,
/// command-less pane (a remote session backs it — the seeder binds it). The
/// per-leaf remote session is not recorded here; use @ref wireToLayout for that.
[[nodiscard]] vtworkspace::LayoutPane wireToLayoutPane(proto::WirePane const& pane);

/// Converts the daemon's whole layout into a realizable `vtworkspace::Layout` (one
/// `LayoutTab` per wire tab) and records each leaf's remote session.
/// @param state The daemon's window layout.
/// @return The converted layout plus its leaf → remote-session map.
[[nodiscard]] WireLayout wireToLayout(proto::LayoutState const& state);

/// @return Whether @p session backs any leaf of @p root. Shares this file's leaf/split predicate,
///         so a malformed split (which the converters collapse to a leaf) is judged the same way
///         here as it is when the tree is realized.
[[nodiscard]] bool paneTreeHosts(proto::WirePane const& root, uint64_t session) noexcept;

/// Answers the cell extent one leaf's session is currently rendered at, or nullopt when the client
/// has no pane for it (a leaf the layout carries but the client has not realized yet).
using LeafCellSize = std::function<std::optional<vtpty::PageSize>(uint64_t session)>;

/// Composes the CLIENT-AREA extent @p root occupies from the extents its leaves are rendered at —
/// the inverse of `vtworkspace::layoutInCells`: along a split's axis both children plus one divider
/// cell (`vtworkspace::CellDividerThickness`); across it, the larger of the two.
///
/// This is how a client that lays panes out in PIXELS answers "how big is my content area, in
/// cells?", a question it cannot answer directly: pane widths are the only cell-space quantity it
/// knows. Composing (rather than solving upward from the pane that just resized) makes the answer a
/// function of the WHOLE tree, so every pane's resize event computes the same number — solving from
/// one leaf yields a different area per leaf and oscillates between them.
///
/// @param root The tab's wire pane tree.
/// @param leafSize Resolves one leaf's rendered extent.
/// @return The composed area, or nullopt if any leaf is unresolved (nothing meaningful to report).
[[nodiscard]] std::optional<vtpty::PageSize> composeClientArea(proto::WirePane const& root,
                                                               LeafCellSize const& leafSize);

/// Picks WHICH tab @ref composeClientArea is applied to, across every window a client mirrors.
///
/// The wire carries one client area per CLIENT (`proto::ResizeRequest`), while a client may mirror
/// several daemon windows — so one tab has to speak for all of them. The anchor names the session
/// whose pane just resized, and the tab hosting it is the one whose extent actually changed; any
/// other tab would report an area no one asked about. When the anchor resolves to nothing — no pane
/// has reported yet, or the anchoring pane has since been unbound — the first tab that composes
/// stands in, which is the right answer for the single-window case (every tab of a window fills the
/// same content area) and a stable guess otherwise.
///
/// @param roots     Every mirrored tab's wire pane tree, in a deterministic order.
/// @param anchor    The session whose pane last reported a grid, if any.
/// @param leafSize  Resolves one leaf's rendered extent, @see LeafCellSize.
/// @return The composed area, or nullopt when no tab composes.
template <std::ranges::input_range Roots>
    requires std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<Roots>>, proto::WirePane>
[[nodiscard]] std::optional<vtpty::PageSize> composeAnchoredClientArea(Roots&& roots,
                                                                       std::optional<uint64_t> anchor,
                                                                       LeafCellSize const& leafSize)
{
    // The anchor test is a plain tree walk, so it gates the (more expensive) composition rather
    // than the other way round; only the first otherwise-usable tab is composed as a fallback.
    auto fallback = std::optional<vtpty::PageSize> {};
    // Forwarded rather than iterated directly: `Roots` is a forwarding reference, and the call site
    // passes a range adaptor pipeline whose const-iterability is not guaranteed in general -- a
    // `views::filter` inserted into that pipeline later would have no const `begin()`. Forwarding
    // preserves the caller's value category, which is what makes such a view iterable here.
    for (proto::WirePane const& root: std::forward<Roots>(roots))
    {
        if (anchor.has_value() && paneTreeHosts(root, *anchor))
        {
            if (auto const composed = composeClientArea(root, leafSize))
                return composed;
        }
        else if (!fallback.has_value())
            fallback = composeClientArea(root, leafSize);
    }
    return fallback;
}

} // namespace vthost::client
