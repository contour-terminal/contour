// SPDX-License-Identifier: Apache-2.0
#include <contour/PaneProxy.h>
#include <contour/TerminalSessionManager.h>

namespace contour
{

bool PaneProxy::isLeaf() const noexcept
{
    auto* p = pane();
    return p == nullptr || p->isLeaf();
}

int PaneProxy::orientation() const noexcept
{
    auto* p = pane();
    return static_cast<int>(p != nullptr ? p->splitState() : vtworkspace::SplitState::None);
}

double PaneProxy::ratio() const noexcept
{
    auto* p = pane();
    return p != nullptr ? p->ratio() : 0.5;
}

TerminalSession* PaneProxy::session() const noexcept
{
    auto* p = pane();
    if (p == nullptr || !p->isLeaf())
        return nullptr;
    return _manager.sessionForId(p->session());
}

bool PaneProxy::active() const noexcept
{
    return _manager.isActivePane(_tab, _id);
}

void PaneProxy::setRatio(double ratio)
{
    _manager.setPaneRatio(_tab, _id, ratio);
}

void PaneProxy::activate()
{
    _manager.activatePane(_tab, _id);
}

} // namespace contour
