// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarModel.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/SwitcherPresenter.hpp>

#include <utility>

namespace contour::window
{

SwitcherPresenter::SwitcherPresenter(SwitcherController& switcher, QObject* parent):
    QObject(parent), _switcher(switcher)
{
    // Both a choice and a cancel close it; a choice was handed on already.
    connect(&_switcher, &SwitcherController::finished, this, &SwitcherPresenter::withdraw);
    connect(&_switcher, &SwitcherController::cancelled, this, &SwitcherPresenter::withdraw);
}

SwitcherHost* SwitcherPresenter::host() const noexcept
{
    return _hostObject ? _host : nullptr;
}

void SwitcherPresenter::open(SwitcherHost& host, SwitcherOpening opening, std::optional<QRectF> anchor)
{
    // One switcher for the application: shown over another window, it leaves the first.
    if (auto* const previous = this->host(); previous != nullptr && previous != &host)
        previous->withdrawSwitcher();
    QObject::disconnect(_hostGone);
    QObject::disconnect(_sidebarFollowed);
    _host = &host;
    _hostObject = &host.switcherHostObject();
    _hostGone = connect(_hostObject, &QObject::destroyed, this, &SwitcherPresenter::hostDestroyed);
    _switcher.open(std::move(opening));
    host.presentSwitcher(&_switcher, anchor);
}

void SwitcherPresenter::followSidebar(SidebarModel const& sidebar,
                                      std::function<std::vector<SwitcherWorkspace>()> windowWorkspaces)
{
    QObject::disconnect(_sidebarFollowed);
    if (host() == nullptr)
        return;
    _sidebarFollowed = connect(
        &sidebar, &SidebarModel::workspaceRowsChanged, this, [this, read = std::move(windowWorkspaces)] {
            if (host() != nullptr)
                _switcher.updateWindowWorkspaces(read());
        });
}

void SwitcherPresenter::withdraw()
{
    QObject::disconnect(_hostGone);
    QObject::disconnect(_sidebarFollowed); // a hidden popup's rows are rebuilt for nobody
    if (auto* const presentedIn = host())
        presentedIn->withdrawSwitcher();
    _host = nullptr;
    _hostObject = nullptr;
}

void SwitcherPresenter::hostDestroyed()
{
    // Not withdrawn: the window is half destroyed. Nothing it asked for may land on it.
    QObject::disconnect(_hostGone);
    QObject::disconnect(_sidebarFollowed);
    _host = nullptr;
    _hostObject = nullptr;
    _switcher.abandon();
}

} // namespace contour::window
