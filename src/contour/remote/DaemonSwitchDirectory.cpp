// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/DaemonSwitchDirectory.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

namespace contour::remote
{

DaemonSwitchDirectory::DaemonSwitchDirectory(DaemonPresence& presence,
                                             std::unique_ptr<IWorkspaceLister> lister,
                                             LinkedDaemons linked):
    _presence(presence), _linked(std::move(linked)), _lister(std::move(lister))
{
}

DaemonSwitchDirectory::~DaemonSwitchDirectory() = default;

std::string DaemonSwitchDirectory::cacheKeyOf(LinkedDaemon const& daemon)
{
    return daemonRefOf(daemon.address, daemon.link).value;
}

std::vector<cli::DiscoveredDaemon> DaemonSwitchDirectory::localDaemons() const
{
    return _presence.localDaemons();
}

std::vector<session::SwitchTarget> DaemonSwitchDirectory::daemonRing() const
{
    auto ring = std::vector<session::SwitchTarget> {};
    for (auto const& daemon: _presence.listed())
    {
        auto const parsed = window::parseDaemonRef(daemon);
        if (!parsed)
            continue;
        // A daemon reached by a link alone is one no switch can name.
        auto const& [kind, name] = *parsed;
        auto switchKind = vthost::proto::SwitchDaemonKind::Name;
        switch (kind)
        {
            case window::DaemonRefKind::Local: break;
            case window::DaemonRefKind::Host: switchKind = vthost::proto::SwitchDaemonKind::Host; break;
            case window::DaemonRefKind::Link: continue;
        }
        ring.push_back({ .kind = switchKind, .daemon = std::string { name }, .workspace = {} });
    }
    return ring;
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
                                                          .name = row.displayName,
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
    _presence.refresh();
    auto const linked = _linked();
    // A daemon no window is on any more is forgotten, so a later one by the same name starts afresh.
    auto const keys = std::ranges::to<std::set>(linked | std::views::transform(&cacheKeyOf));
    std::erase_if(_workspaces, [&](auto const& entry) { return !keys.contains(entry.first); });
    for (auto const& daemon: linked)
        startListing(daemon);
}

void DaemonSwitchDirectory::refreshDiscovery()
{
    // The presence's one discovery, and its observers: a listed daemon is watched whichever surface asks.
    _presence.refresh();
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
                          attachLog()("switch directory: listing {} failed: {}", key, listed.error().reason);
                          return;
                      }
                      _workspaces[key] = *std::move(listed);
                  });
}

} // namespace contour::remote
