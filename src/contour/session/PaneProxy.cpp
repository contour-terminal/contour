// SPDX-License-Identifier: Apache-2.0
#include <contour/session/PaneProxy.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>

namespace contour::session
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

int PaneProxy::focus() const noexcept
{
    auto const paneFocus =
        active() ? window::activePaneFocus(_manager.lastPaneActivation(_tab)) : window::PaneFocus::Unfocused;
    return static_cast<int>(paneFocus);
}

void PaneProxy::setRatio(double ratio)
{
    _manager.setPaneRatio(_tab, _id, ratio);
}

void PaneProxy::activate()
{
    _manager.notePaneActivation(_tab, window::PaneActivation::Pointer);
    _manager.activatePane(_tab, _id);
}

} // namespace contour::session
