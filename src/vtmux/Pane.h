// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <memory>
#include <optional>
#include <utility>

#include <vtmux/Primitives.h>

namespace vtmux
{

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
    void setRatio(double ratio) noexcept
    {
        // A minimum visible share for each child; keeps the divider grabbable at the extremes.
        constexpr double MinimumRatio = 0.05;
        _ratio = std::clamp(ratio, MinimumRatio, 1.0 - MinimumRatio);
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

    /// Absorbs the surviving sibling when @p child (which must be a direct child of this split) is
    /// being closed. After this call, this node *becomes* the surviving sibling (taking over its
    /// leaf session or its split children and id).
    ///
    /// @return the SessionId of the (now removed) closed child, so the caller can tear down that
    ///         session.
    SessionId closeChild(Pane* child);

    /// Depth-first pre-order visit. @p f is called on this node, then the first subtree, then the
    /// second.
    ///
    /// If @p f returns a value convertible to bool, traversal short-circuits: the first node for
    /// which @p f returns true is returned, otherwise nullptr. If @p f returns void, every node is
    /// visited and nullptr is returned.
    template <typename F>
    Pane* walkTree(F&& f)
    {
        if constexpr (std::convertible_to<std::invoke_result_t<F&, Pane&>, bool>)
        {
            if (f(*this))
                return this;
            if (_first)
                if (auto* found = _first->walkTree(f))
                    return found;
            if (_second)
                if (auto* found = _second->walkTree(f))
                    return found;
            return nullptr;
        }
        else
        {
            f(*this);
            if (_first)
                _first->walkTree(f);
            if (_second)
                _second->walkTree(f);
            return nullptr;
        }
    }

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

} // namespace vtmux
