// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/DaemonSwitchDirectory.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace contour::remote
{

DaemonSwitchDirectory::DaemonSwitchDirectory(std::shared_ptr<cli::IDaemonDiscovery> discovery,
                                             std::unique_ptr<IWorkspaceLister> lister,
                                             LinkedDaemons linked):
    _discovery(std::move(discovery)), _linked(std::move(linked)), _lister(std::move(lister))
{
}

DaemonSwitchDirectory::~DaemonSwitchDirectory() = default;

std::string DaemonSwitchDirectory::cacheKeyOf(LinkedDaemon const& daemon)
{
    switch (daemon.address.kind)
    {
        case vthost::proto::SwitchDaemonKind::Label: return std::format("label:{}", daemon.address.name);
        case vthost::proto::SwitchDaemonKind::Host: return std::format("host:{}", daemon.address.name);
        case vthost::proto::SwitchDaemonKind::Same: break;
    }
    return std::format("link:{}", daemon.link.value);
}

std::vector<cli::DiscoveredDaemon> DaemonSwitchDirectory::localDaemons() const
{
    return _localDaemons;
}

std::vector<std::string> DaemonSwitchDirectory::hostsShownHere() const
{
    auto hosts = std::vector<std::string> {};
    for (auto const& daemon: _linked())
        if (daemon.address.kind == vthost::proto::SwitchDaemonKind::Host && !daemon.windows.empty())
            hosts.push_back(daemon.address.name);
    std::ranges::sort(hosts);
    auto const duplicates = std::ranges::unique(hosts);
    hosts.erase(duplicates.begin(), duplicates.end());
    return hosts;
}

std::vector<session::KnownWorkspace> DaemonSwitchDirectory::workspacesOf(
    vtworkspace::WorkspaceId window) const
{
    for (auto const& daemon: _linked())
    {
        auto const shown = std::ranges::find(daemon.windows, window, &WindowOnDaemon::window);
        if (shown == daemon.windows.end())
            continue;
        auto const cached = _workspaces.find(cacheKeyOf(daemon));
        if (cached == _workspaces.end())
            return {};
        auto const rows = workspaceRowsOf(
            cached->second, [](uint64_t) { return std::optional<vtworkspace::WorkspaceId> {}; });
        auto known = std::ranges::to<std::vector>(rows | std::views::transform([&](WorkspaceRow const& row) {
                                                      return session::KnownWorkspace {
                                                          .position = row.position,
                                                          .name = row.name,
                                                          .role = row.id == shown->workspace
                                                                      ? session::WorkspaceRole::Current
                                                                      : session::WorkspaceRole::Other,
                                                      };
                                                  }));
        std::ranges::sort(known, {}, &session::KnownWorkspace::position);
        return known;
    }
    return {};
}

void DaemonSwitchDirectory::refresh()
{
    startDiscovery();
    auto const linked = _linked();
    // A daemon no window is on any more is forgotten, so a later one by the same name starts afresh.
    auto const keys = std::ranges::to<std::set>(linked | std::views::transform(&cacheKeyOf));
    std::erase_if(_workspaces, [&](auto const& entry) { return !keys.contains(entry.first); });
    for (auto const& daemon: linked)
        startListing(daemon);
}

void DaemonSwitchDirectory::startDiscovery()
{
    if (_discoveryState == Discovery::Running)
        return;
    _discoveryState = Discovery::Running;
    // Off the GUI thread: each socket is probed.
    _jobs.run([discovery = _discovery] { return discovery->discover(); },
              [this](std::vector<cli::DiscoveredDaemon> found) {
                  _localDaemons = std::move(found);
                  _discoveryState = Discovery::Idle;
              });
}

void DaemonSwitchDirectory::startListing(LinkedDaemon const& daemon)
{
    auto key = cacheKeyOf(daemon);
    if (!_listing.insert(key).second)
        return;
    // The lister is destroyed before everything its answer touches, and tells nothing once gone.
    _lister->list(ListTarget { .address = daemon.address, .transport = daemon.transport },
                  [this, key = std::move(key)](auto listed) {
                      _listing.erase(key);
                      if (!listed)
                      {
                          attachLog()("switch directory: listing {} failed: {}", key, listed.error());
                          return;
                      }
                      _workspaces[key] = *std::move(listed);
                  });
}

} // namespace contour::remote
