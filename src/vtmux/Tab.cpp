// SPDX-License-Identifier: Apache-2.0
#include <vtmux/Tab.h>

#include <cassert>

namespace vtmux
{

void Tab::touchMru(PaneId id)
{
    forgetFromMru(id);
    _mru.insert(_mru.begin(), id);
}

void Tab::forgetFromMru(PaneId id)
{
    std::erase(_mru, id);
}

void Tab::setActivePane(Pane* leaf)
{
    assert(leaf != nullptr && leaf->isLeaf());
    _activeLeaf = leaf;
    touchMru(leaf->id());
}

bool Tab::toggleZoom() noexcept
{
    // Nothing to hide in a single-pane tab, so there is no zoom to enter (and none to leave: the
    // restructuring operations below already cleared it on the way down to one pane).
    if (!hasMultiplePanes())
        return false;

    _zoomed = !_zoomed;
    return true;
}

Pane* Tab::splitActivePane(
    SplitState direction, PaneId splitNodeId, PaneId newLeafId, SessionId newSession, double ratio)
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());

    _zoomed = false; // the new pane must be visible (see Tab.h's zoom block)

    auto const [firstChild, secondChild] =
        _activeLeaf->split(direction, splitNodeId, newLeafId, newSession, ratio);

    // The promoted node took splitNodeId; its old id migrated to firstChild. Fix up the MRU: the
    // active leaf's id now lives on firstChild (same id value), and the new leaf becomes active.
    setActivePane(secondChild);
    return secondChild;
}

SessionId Tab::closePane(Pane* leaf)
{
    assert(leaf != nullptr && leaf->isLeaf());
    assert(!isLastPane(leaf) && "closing the last pane must close the tab instead");

    // Drop the zoom: the survivors are re-laid out, and the closed pane may well have been the zoomed
    // one. Unconditional, so no zoomed-pane bookkeeping has to survive the absorption below.
    _zoomed = false;

    auto* parent = leaf->parent();
    assert(parent != nullptr);

    auto const closedId = leaf->id();
    // The pane the parent will become (its surviving sibling), determined before absorption.
    Pane* sibling = (parent->first() == leaf) ? parent->second() : parent->first();

    // Decide whether the active leaf is about to be invalidated BEFORE closeChild() runs: it absorbs the
    // sibling's contents into the parent and destroys both the closed leaf and the old sibling Pane
    // object, leaving `leaf`, `sibling`, and possibly `_activeLeaf` dangling. Reading (even comparing)
    // those pointers after absorption is undefined behaviour, so capture the decision as a plain bool now.
    bool const activeLeafInvalidated = _activeLeaf == leaf || _activeLeaf == sibling;

    auto const closedSession = parent->closeChild(leaf);

    forgetFromMru(closedId);

    // If the active leaf was the one closed or the sibling that was absorbed away, pick a new active leaf
    // from the MRU, falling back to the first surviving leaf.
    if (activeLeafInvalidated)
    {
        Pane* next = nullptr;
        for (auto const id: _mru)
        {
            if (auto* p = _root->findPane(id); p != nullptr && p->isLeaf())
            {
                next = p;
                break;
            }
        }
        // MRU invariant: _mru names exactly the live leaf ids (the ctor seeds the root id, split
        // and absorption migrate ids in place, forgetFromMru drops only closed ids), and closePane
        // requires a parent split — so a surviving leaf is always found above. Keep the contract
        // explicit instead of silently picking an arbitrary leaf if it ever broke.
        assert(next != nullptr && "MRU invariant: _mru names every surviving leaf");
        setActivePane(next);
    }

    return closedSession;
}

Pane* Tab::focusDirection(FocusDirection direction)
{
    assert(_activeLeaf != nullptr);
    if (auto* target = _root->neighbor(_activeLeaf, direction); target != nullptr)
    {
        setActivePane(target);
        return target;
    }
    return nullptr;
}

Pane* Tab::toggleActivePaneOrientation()
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());
    auto* parent = _activeLeaf->parent();
    if (parent == nullptr)
        return nullptr; // single-pane tab: no split to flip

    _zoomed = false; // the flipped axis must be visible (see Tab.h's zoom block)
    parent->toggleOrientation();
    return parent;
}

Pane* Tab::resizeActivePane(FocusDirection direction, double fraction)
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());

    auto* split = Pane::ancestorSplitOnAxis(_activeLeaf, crossingSplitFor(direction));
    if (split == nullptr)
        return nullptr; // single pane, or only cross-axis splits above the active pane

    _zoomed = false; // the divider being moved must be visible (see Tab.h's zoom block)

    // `ratio` is the FIRST child's share of the split. The user presses a direction to move the shared
    // divider that way: pressing toward the second child (Right/Down) enlarges the first child's share
    // (+fraction); pressing toward the first child (Left/Up) shrinks it (-fraction). This "move the
    // boundary in this direction" model matches Windows Terminal and is independent of which side the
    // active pane sits on. setRatio() clamps.
    auto const delta = pointsTowardSecondChild(direction) ? fraction : -fraction;
    split->setRatio(split->ratio() + delta);
    return split;
}

std::pair<Pane*, Pane*> Tab::swapActivePane(FocusDirection direction)
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());
    auto* neighbor = _root->neighbor(_activeLeaf, direction);
    if (neighbor == nullptr)
        return { nullptr, nullptr };

    _zoomed = false; // the swapped panes must be visible (see Tab.h's zoom block)

    auto* previouslyActive = _activeLeaf;
    previouslyActive->swapLeafPayload(neighbor);
    // The active session now lives in the neighbor slot; keep focus on it so the pane "moved".
    setActivePane(neighbor);
    return { previouslyActive, neighbor };
}

bool Tab::moveActivePane(FocusDirection direction, PaneId newSplitId)
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());
    auto* neighbor = _root->neighbor(_activeLeaf, direction);
    if (neighbor == nullptr)
        return false;

    _zoomed = false; // the moved pane must be visible (see Tab.h's zoom block)

    // Degenerate case: the active leaf and its neighbor are the two children of one split. "Moving
    // across" then has no distinct destination — it is an in-place session swap, which preserves the
    // tree. Detect it before any structural mutation (afterwards the pointers may dangle).
    if (_activeLeaf->parent() == neighbor->parent())
    {
        _activeLeaf->swapLeafPayload(neighbor);
        setActivePane(neighbor);
        return true;
    }

    // Genuine re-parent. Capture everything we need as VALUES up front: closeChild() below destroys
    // Pane objects and re-homes ids, so `_activeLeaf`, `neighbor`, and their parents may all dangle
    // afterwards. The moved session and the neighbor's id survive the collapse and let us re-find the
    // destination in the mutated tree.
    auto const movedSession = _activeLeaf->session();
    auto const movedId = _activeLeaf->id();
    auto const neighborId = neighbor->id();

    // Collapse the active leaf out of its current split (its sibling absorbs the parent). This is the
    // close half of the move; the session is NOT torn down (we re-open it below).
    auto* oldParent = _activeLeaf->parent();
    assert(oldParent != nullptr && "a neighbor exists, so the active leaf is not the root");
    oldParent->closeChild(_activeLeaf);
    forgetFromMru(movedId);

    // Re-find the destination in the now-mutated tree and split it along the axis we moved on, dropping
    // the moved session into the new leaf. The moved leaf keeps its original id (its identity is
    // preserved across the move); the promoted node takes newSplitId.
    auto* dest = _root->findPane(neighborId);
    assert(dest != nullptr && dest->isLeaf() && "the neighbor survives the sibling collapse");
    auto const axis = crossingSplitFor(direction);
    // split() homes the pre-existing (neighbor) session in the first child and the incoming (moved)
    // session in the second child, with the moved leaf keeping movedId. So the moved pane is naturally
    // the second/far child — correct for a Right/Down move.
    auto const [firstChild, secondChild] = dest->split(axis, newSplitId, movedId, movedSession);
    Pane* movedLeaf = secondChild;

    // For a Left/Up move the moved pane belongs on the near (first) side. Swap the two child subtrees
    // wholesale — ids and sessions travel together, so no PaneId is ever separated from its session.
    if (!pointsTowardSecondChild(direction))
    {
        dest->swapChildren();
        movedLeaf = dest->first();
    }

    setActivePane(movedLeaf);
    return true;
}

std::string Tab::title(SessionTitleResolver const& resolver) const
{
    if (_runtimeTitle.has_value())
        return *_runtimeTitle;
    if (usesMultiplePanesLabel())
        return std::string { MultiplePanesLabel };
    return resolver(_activeLeaf->session());
}

} // namespace vtmux
