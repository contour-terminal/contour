// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The application's switch directory: the local daemons running, the hosts the windows here show, and every
/// workspace of each daemon a window is on -- all from caches that refresh() renews off the GUI thread.

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/remote/WorkspaceLister.hpp>
#include <contour/session/SwitchDirectory.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vthost/Transport.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::remote
{

/// A window here, and the daemon workspace it shows.
struct WindowOnDaemon
{
    vtworkspace::WorkspaceId window; ///< The window.
    uint64_t workspace = 0;          ///< The daemon's workspace it shows.
};

/// A daemon a window here is on, as the switch directory sees it: one entry per link to it.
struct LinkedDaemon
{
    DaemonLinkId link;                                    ///< The link it is reached by.
    DaemonAddress address;                                ///< The daemon as a switch names it.
    std::shared_ptr<vthost::ITransportFactory> transport; ///< How to reach it, to list its workspaces.
    std::vector<WindowOnDaemon> windows;                  ///< The windows here it serves.
};

/// Answers, on the GUI thread, which daemons the windows here are on; the application asks its links.
using LinkedDaemons = std::function<std::vector<LinkedDaemon>()>;

/// The application's session::SwitchDirectory.
///
/// localDaemons() answers what the last local discovery found, hostsShownHere() the saved hosts the windows
/// here are on, and workspacesOf() the daemon's full workspace list as its `workspace.list` last answered it
/// (hidden and left ones included), with the asking window's own marked from what its link shows now. Nothing
/// it answers waits: refresh() starts the discovery on a thread of its own and each daemon's listing through
/// the lister, and their answers replace the caches as they arrive. GUI thread only.
class DaemonSwitchDirectory final: public session::SwitchDirectory
{
  public:
    /// @param discovery Finds the local daemons; run off the GUI thread. Shared with the Connect picker.
    /// @param lister Lists a daemon's workspaces.
    /// @param linked Which daemons the windows here are on.
    DaemonSwitchDirectory(std::shared_ptr<cli::IDaemonDiscovery> discovery,
                          std::unique_ptr<IWorkspaceLister> lister,
                          LinkedDaemons linked);
    ~DaemonSwitchDirectory() override;
    DaemonSwitchDirectory(DaemonSwitchDirectory const&) = delete;
    DaemonSwitchDirectory(DaemonSwitchDirectory&&) = delete;
    DaemonSwitchDirectory& operator=(DaemonSwitchDirectory const&) = delete;
    DaemonSwitchDirectory& operator=(DaemonSwitchDirectory&&) = delete;

    [[nodiscard]] std::vector<cli::DiscoveredDaemon> localDaemons() const override;
    [[nodiscard]] std::vector<std::string> hostsShownHere() const override;
    [[nodiscard]] std::vector<session::KnownWorkspace> workspacesOf(
        vtworkspace::WorkspaceId window) const override;
    void refresh() override;
    void refreshDiscovery() override;

  private:
    /// Whether a discovery runs now: a refresh meanwhile starts none.
    enum class Discovery : uint8_t
    {
        Idle = 0,
        Running,
    };

    /// @return What @p daemon's workspaces are cached under: its address, or for one no switch can name
    ///         (`--connect-tcp`) its link.
    [[nodiscard]] static std::string cacheKeyOf(LinkedDaemon const& daemon);

    void startDiscovery();
    void startListing(LinkedDaemon const& daemon);

    std::shared_ptr<cli::IDaemonDiscovery> _discovery;
    LinkedDaemons _linked;
    /// What the last discovery found; empty until one answered.
    std::vector<cli::DiscoveredDaemon> _localDaemons;
    /// Each daemon's workspaces, by cacheKeyOf(), as its last listing answered.
    std::map<std::string, std::vector<vtcontrol::WorkspaceInfo>> _workspaces;
    /// The daemons, by cacheKeyOf(), whose listing is under way: a refresh meanwhile does not ask them again.
    std::set<std::string> _listing;
    Discovery _discoveryState = Discovery::Idle;
    /// Destroyed before everything its answers touch: nothing answers into a directory going away.
    std::unique_ptr<IWorkspaceLister> _lister;
    /// The discoveries under way, their answers told back here; destroyed (joined) first.
    platform::OffThreadJobs _jobs;
};

} // namespace contour::remote
