// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cassert>

#include <vtmux/Tab.h>

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

Pane* Tab::splitActivePane(
    SplitState direction, PaneId splitNodeId, PaneId newLeafId, SessionId newSession, double ratio)
{
    assert(_activeLeaf != nullptr && _activeLeaf->isLeaf());

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

    auto* parent = leaf->parent();
    assert(parent != nullptr);

    auto const closedId = leaf->id();
    // The pane the parent will become (its surviving sibling), determined before absorption.
    Pane* sibling = (parent->first() == leaf) ? parent->second() : parent->first();
    bool const siblingIsLeaf = sibling->isLeaf();
    auto const siblingId = sibling->id();

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
        if (next == nullptr)
        {
            // The absorbed parent now *is* the surviving sibling; descend to a leaf within it.
            next = siblingIsLeaf ? parent->findPane(siblingId) : parent->firstLeaf();
            if (next == nullptr)
                next = _root->firstLeaf();
        }
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

std::string Tab::title(SessionTitleResolver const& resolver) const
{
    if (_runtimeTitle.has_value())
        return *_runtimeTitle;
    if (hasMultiplePanes())
        return std::string { MultiplePanesLabel };
    return resolver(_activeLeaf->session());
}

} // namespace vtmux
