// SPDX-License-Identifier: Apache-2.0
#include <cassert>

#include <vtmux/Pane.h>

namespace vtmux
{

std::pair<Pane*, Pane*> Pane::split(
    SplitState direction, PaneId splitNodeId, PaneId newLeafId, SessionId newSession, double ratio)
{
    assert(isLeaf() && "split() must be called on a leaf");
    assert(direction != SplitState::None && "split direction must be Horizontal or Vertical");

    // Move this leaf's session into a new first child, keeping this pane's *old* id on that child so
    // the surviving terminal's identity is preserved across the split.
    auto firstChild = std::make_unique<Pane>(_id, _session);
    auto secondChild = std::make_unique<Pane>(newLeafId, newSession);
    firstChild->_parent = this;
    secondChild->_parent = this;

    // This node becomes the split.
    _id = splitNodeId;
    _session = SessionId {};
    _splitState = direction;
    setRatio(ratio); // clamp the initial ratio into (0, 1) like every other ratio mutation
    _first = std::move(firstChild);
    _second = std::move(secondChild);

    return { _first.get(), _second.get() };
}

SessionId Pane::closeChild(Pane* child)
{
    assert(!isLeaf() && "closeChild() must be called on a split node");
    assert((child == _first.get() || child == _second.get()) && "child must be a direct child");

    auto const closedSession = child->isLeaf() ? child->session() : SessionId {};

    // Identify the surviving sibling and detach it from ownership so we can absorb its contents.
    std::unique_ptr<Pane> survivor = (child == _first.get()) ? std::move(_second) : std::move(_first);

    // Absorb the survivor: become whatever it was (leaf or split). We keep the survivor's id so its
    // own identity (and any host rendering it) is preserved.
    _id = survivor->_id;
    _splitState = survivor->_splitState;
    _ratio = survivor->_ratio;
    _session = survivor->_session;
    _first = std::move(survivor->_first);
    _second = std::move(survivor->_second);
    if (_first)
        _first->_parent = this;
    if (_second)
        _second->_parent = this;

    // The old child nodes are released here (the closed one and the now-emptied survivor wrapper).
    return closedSession;
}

Pane* Pane::findPane(PaneId id)
{
    return walkTree([id](Pane& p) { return p.id() == id; });
}

Pane* Pane::findLeaf(SessionId session)
{
    return walkTree([session](Pane& p) { return p.isLeaf() && p.session() == session; });
}

Pane* Pane::firstLeaf()
{
    return walkTree([](Pane& p) { return p.isLeaf(); });
}

int Pane::leafCount() const noexcept
{
    if (isLeaf())
        return 1;
    int count = 0;
    if (_first)
        count += _first->leafCount();
    if (_second)
        count += _second->leafCount();
    return count;
}

Pane* Pane::descendToEdge(Pane* subtree, FocusDirection direction)
{
    // When moving along `direction`, the boundary leaf of the neighbouring subtree is the one
    // closest to the edge we came from: arriving by moving Right, we want the left-most leaf, i.e.
    // keep descending into the *first* child across vertical splits; symmetrically for the others.
    Pane* node = subtree;
    while (!node->isLeaf())
    {
        if (node->splitState() == crossingSplitFor(direction))
            node = pointsTowardSecondChild(direction) ? node->first() : node->second();
        else
            node = node->first();
    }
    return node;
}

Pane* Pane::neighbor(Pane const* fromLeaf, FocusDirection direction)
{
    auto const axis = crossingSplitFor(direction);
    auto const towardSecond = pointsTowardSecondChild(direction);

    // Walk up until we find an ancestor split on the matching axis where `fromLeaf` is on the side
    // we are moving away from (so the opposite subtree lies in the requested direction).
    Pane const* node = fromLeaf;
    Pane* ancestor = fromLeaf->parent();
    while (ancestor != nullptr)
    {
        if (ancestor->splitState() == axis)
        {
            bool const cameFromFirst = (ancestor->first() == node);
            // Moving toward the second child: we can cross only if we came up from the first child.
            // Moving toward the first child: we can cross only if we came up from the second child.
            if (cameFromFirst == towardSecond)
            {
                Pane* into = towardSecond ? ancestor->second() : ancestor->first();
                return descendToEdge(into, direction);
            }
        }
        node = ancestor;
        ancestor = ancestor->parent();
    }
    return nullptr;
}

} // namespace vtmux
