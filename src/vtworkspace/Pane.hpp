// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace vtworkspace
{

struct PaneParts;

/// A node in a tab's binary split tree.
///
/// The model is the one used by Windows Terminal: a pane is **either** a leaf **or** an internal
/// split node, never both.
///
/// - A **leaf** has SplitState::None, carries a SessionId, and has no children. It is what the user
///   sees as a single terminal.
/// - A **split node** has SplitState::Horizontal or ::Vertical, has exactly two non-null children,
///   a split ratio, and carries no session.
///
/// The two structure-mutating operations preserve that invariant and keep the tree minimal:
///
/// - split(): the focused leaf promotes *itself* into a split node. Its session moves into a newly
///   created first child; the incoming leaf becomes the second child. No allocation of a separate
///   parent node is needed, and — crucially — the surviving session keeps living in a leaf whose
///   identity (its PaneId) is preserved, so a host rendering that leaf need not tear it down.
/// - closeChild(): when one child of a split closes, the parent absorbs the surviving sibling,
///   becoming whatever the sibling was (leaf or split). No internal node is ever left with a single
///   child.
class Pane
{
  public:
    /// Constructs a leaf pane holding @p session, identified by @p id.
    Pane(PaneId id, SessionId session) noexcept: _id { id }, _session { session } {}

    Pane(Pane const&) = delete;
    Pane& operator=(Pane const&) = delete;
    Pane(Pane&&) = delete;
    Pane& operator=(Pane&&) = delete;
    ~Pane() = default;

    [[nodiscard]] PaneId id() const noexcept { return _id; }
    [[nodiscard]] SplitState splitState() const noexcept { return _splitState; }

    /// A pane is a leaf iff it has no split.
    [[nodiscard]] bool isLeaf() const noexcept { return _splitState == SplitState::None; }

    /// The session carried by a leaf. Only valid when isLeaf().
    [[nodiscard]] SessionId session() const noexcept { return _session; }

    [[nodiscard]] Pane* parent() const noexcept { return _parent; }
    [[nodiscard]] Pane* first() const noexcept { return _first.get(); }
    [[nodiscard]] Pane* second() const noexcept { return _second.get(); }

    /// The split position as a fraction in (0, 1): the share of space given to the first child.
    /// Only meaningful on a split node.
    [[nodiscard]] double ratio() const noexcept { return _ratio; }

    /// Sets the split ratio, clamped to the open interval (0, 1) so neither child is ever given zero
    /// space. A ratio of exactly 0 or 1 (or out of range) would collapse one child to zero width/height
    /// in the renderer, leaving a pane with no grab area that the user cannot recover; clamping here —
    /// the single low-level mutator — protects every caller (the QML drag, a layout/Snapshot restore,
    /// a future daemon), not just the one that already clamps before calling.
    /// @param ratio The desired first-child space share; clamped into (0, 1).
    void setRatio(double ratio) noexcept { _ratio = clampRatio(ratio); }

    /// @return @p ratio as setRatio() stores it: clamped so that each child keeps a visible share.
    [[nodiscard]] static constexpr double clampRatio(double ratio) noexcept
    {
        // A minimum visible share for each child; keeps the divider grabbable at the extremes.
        constexpr double MinimumRatio = 0.05;
        return std::clamp(ratio, MinimumRatio, 1.0 - MinimumRatio);
    }

    /// Promotes this leaf into a split node along @p direction.
    ///
    /// The original session is moved into a freshly created first child (keeping this pane's old id
    /// on that child so the surviving terminal's identity is preserved); a new leaf carrying
    /// @p newLeafId / @p newSession becomes the second child; this node takes a brand new id
    /// @p splitNodeId and becomes the split.
    ///
    /// @param direction   Horizontal or Vertical (must not be None).
    /// @param splitNodeId The id this node adopts as a split (the old id migrates to the first
    ///                    child).
    /// @param newLeafId   The id of the new leaf (second child).
    /// @param newSession  The session for the new leaf.
    /// @param ratio       The first child's space share, in (0, 1).
    /// @return A pair {firstChild, secondChild}. firstChild is the original session's new home;
    ///         secondChild is the new leaf.
    std::pair<Pane*, Pane*> split(
        SplitState direction, PaneId splitNodeId, PaneId newLeafId, SessionId newSession, double ratio = 0.5);

    /// Grafts @p subtree beside this leaf, which becomes a split node.
    ///
    /// The leaf's own session moves into a freshly created child that keeps this pane's old id (as
    /// split() does); @p subtree, with every id and session it already carries, becomes the other
    /// child; this node takes the new id @p splitNodeId.
    ///
    /// @pre isLeaf(), @p subtree is not null, and @p state is not None.
    /// @param subtree     The tree to graft, adopted whole.
    /// @param splitNodeId The id this node adopts as a split.
    /// @param state       Horizontal or Vertical.
    /// @param order       Whether @p subtree becomes the first or the second child.
    /// @param ratio       The first child's space share, in (0, 1).
    /// @return The root of the grafted subtree (now a child of this node).
    Pane* graft(std::unique_ptr<Pane> subtree,
                PaneId splitNodeId,
                SplitState state,
                GraftOrder order,
                double ratio = 0.5);

    /// Absorbs the surviving sibling when @p child (which must be a direct child of this split) is
    /// being closed. After this call, this node *becomes* the surviving sibling (taking over its
    /// leaf session or its split children and id).
    ///
    /// @return the SessionId of the (now removed) closed child, so the caller can tear down that
    ///         session.
    SessionId closeChild(Pane* child);

    /// Like closeChild(), but hands the removed child (with its whole subtree, ids and sessions intact)
    /// back instead of destroying it, so the caller can graft it elsewhere.
    /// @param child A direct child of this split node.
    /// @return The detached child, now a root (its parent is null).
    [[nodiscard]] std::unique_ptr<Pane> detachChild(Pane* child);

    /// Flips this split node's axis Horizontal<->Vertical, leaving its children and ratio untouched.
    ///
    /// The layout solver re-derives all geometry from the new axis, so a host need only re-read the
    /// orientation (no child rebind). No-op semantics are the caller's job; this asserts it is a split.
    /// @pre !isLeaf().
    void toggleOrientation() noexcept;

    /// Sets this split node's axis, leaving its children and ratio untouched.
    /// @param axis Horizontal or Vertical.
    /// @pre !isLeaf() and @p axis is not SplitState::None.
    void setOrientation(SplitState axis) noexcept;

    /// Swaps only the session carried by this leaf with @p other's, keeping both PaneIds.
    ///
    /// Swapping the *sessions* (not the ids) is what a pane swap needs: PaneId is the stable identity
    /// that hosts (Qt proxies, a future daemon) address a node by, so it must not move; only the
    /// terminal living in each slot trades places. Both nodes must be leaves.
    /// @param other The other leaf to trade sessions with (must be a leaf, and not this node).
    void swapLeafPayload(Pane* other) noexcept;

    /// Puts @p session into this leaf in place of the one it carries, keeping the PaneId.
    ///
    /// What a restart needs: the pane (the slot hosts address by its id) stays, and a new terminal
    /// takes the place of one that ended. @pre isLeaf().
    /// @param session The session the leaf carries from now on.
    /// @return The session it carried before.
    SessionId replaceSession(SessionId session) noexcept;

    /// Swaps the two children of this split node (their whole subtrees, ids and sessions intact).
    ///
    /// Flips which child occupies the first vs. second slot without renaming anything — used to place
    /// a freshly re-parented pane on the near side of a split. @pre !isLeaf().
    void swapChildren() noexcept;

    /// Depth-first pre-order visit. @p f is called on this node, then the first subtree, then the
    /// second.
    ///
    /// If @p f returns a value convertible to bool, traversal short-circuits: the first node for
    /// which @p f returns true is returned, otherwise nullptr. If @p f returns void, every node is
    /// visited and nullptr is returned.
    template <typename F>
    Pane* walkTree(F&& f)
    {
        // @p f is invoked once per visited node, so it is forwarded once into a reference binding
        // (which consumes nothing) and called through that. Forwarding at each use would move from it
        // on the first node and use it again on every following one.
        auto&& visit = std::forward<F>(f);
        if constexpr (std::convertible_to<std::invoke_result_t<F&, Pane&>, bool>)
        {
            if (visit(*this))
                return this;
            if (_first)
                if (auto* found = _first->walkTree(visit))
                    return found;
            if (_second)
                if (auto* found = _second->walkTree(visit))
                    return found;
            return nullptr;
        }
        else
        {
            visit(*this);
            if (_first)
                _first->walkTree(visit);
            if (_second)
                _second->walkTree(visit);
            return nullptr;
        }
    }

    /// Whether @p node lies in this subtree, counting this node as its own ancestor.
    ///
    /// Walks up from @p node rather than down from here, so it costs the node's depth, not the
    /// subtree's size. Answers "is this pane on screen?" for a host that renders only one subtree of
    /// the tree (see Tab::layoutRoot).
    /// @param node The pane to test; nullptr is not contained.
    [[nodiscard]] bool contains(Pane const* node) const noexcept;

    /// Returns the pane with id @p id in this subtree, or nullptr.
    [[nodiscard]] Pane* findPane(PaneId id);

    /// Returns the leaf carrying @p session in this subtree, or nullptr.
    [[nodiscard]] Pane* findLeaf(SessionId session);

    /// Returns the first leaf in depth-first order within this subtree (this node if it is a leaf).
    [[nodiscard]] Pane* firstLeaf();

    /// Returns the number of leaves in this subtree.
    [[nodiscard]] int leafCount() const noexcept;

    /// Returns the leaf adjacent to @p fromLeaf in @p direction, or nullptr if there is none.
    ///
    /// Walks up to the nearest ancestor split whose axis matches @p direction and where @p fromLeaf
    /// lies on the side you are moving away from, then descends into the opposite subtree picking
    /// the geometrically nearest leaf along that edge.
    [[nodiscard]] Pane* neighbor(Pane const* fromLeaf, FocusDirection direction);

    /// Returns the nearest ancestor split of @p fromLeaf whose axis equals @p axis, or nullptr.
    ///
    /// Unlike neighbor(), this does not require @p fromLeaf to lie on a particular side: it returns
    /// the first ancestor split laid out along @p axis, regardless of which child the walk came up
    /// from. It is what a directional pane *resize* nudges — the divider whose movement grows or
    /// shrinks @p fromLeaf along @p axis.
    /// @param fromLeaf The leaf to start the upward walk from.
    /// @param axis     The split axis to look for (Horizontal or Vertical; not None).
    /// @return The nearest matching ancestor split node, or nullptr if there is none.
    [[nodiscard]] static Pane* ancestorSplitOnAxis(Pane* fromLeaf, SplitState axis) noexcept;

    /// Returns the lowest node having both @p a and @p b in its subtree, or nullptr if they are the
    /// same node or belong to different trees.
    ///
    /// For two DISTINCT leaves the answer is always the split node that separates them — the divider
    /// between the two. That is how a peer names a split without knowing our pane ids: it sends ANY
    /// leaf from each side (the daemon protocol's ResizeSplit does exactly this), and mirrored trees
    /// resolve the same node. Which leaf each side contributes does not matter, so neither end has to
    /// agree with the other on how to pick one.
    /// @param a One node to start the upward walk from.
    /// @param b The node the ancestor must also contain.
    /// @return Their lowest common ancestor, or nullptr.
    [[nodiscard]] static Pane* lowestCommonAncestor(Pane* a, Pane const* b) noexcept;

    /// Takes the tree under @p root apart: its leaves are handed back intact (PaneId and session), each a
    /// root of its own, and its split nodes are destroyed, their ids handed back for reuse.
    /// @param root The tree to take apart.
    /// @return The leaves in tree order, and the split node ids in pre-order.
    [[nodiscard]] static PaneParts dismantle(std::unique_ptr<Pane> root);

    /// Joins two roots under a new split node.
    /// @param id     The split node's id.
    /// @param state  Horizontal or Vertical.
    /// @param ratio  The first child's space share; clamped as setRatio() does.
    /// @param first  The first child, a root.
    /// @param second The second child, a root.
    /// @return The split node, a root.
    [[nodiscard]] static std::unique_ptr<Pane> join(
        PaneId id, SplitState state, double ratio, std::unique_ptr<Pane> first, std::unique_ptr<Pane> second);

  private:
    /// Descends into @p subtree to find the boundary leaf nearest the edge we arrived from when
    /// moving along @p direction (e.g. when moving Right, pick the left-most leaf of the subtree).
    [[nodiscard]] static Pane* descendToEdge(Pane* subtree, FocusDirection direction);

    PaneId _id;
    SplitState _splitState = SplitState::None;
    double _ratio = 0.5;
    SessionId _session {}; //!< Valid only when isLeaf().
    std::unique_ptr<Pane> _first;
    std::unique_ptr<Pane> _second;
    Pane* _parent = nullptr;
};

/// What Pane::dismantle() takes a tree apart into.
struct PaneParts
{
    std::vector<std::unique_ptr<Pane>> leaves; //!< The leaves, intact, in tree order.
    std::vector<PaneId> splitIds;              //!< The ids the split nodes had, in pre-order.
};

} // namespace vtworkspace
