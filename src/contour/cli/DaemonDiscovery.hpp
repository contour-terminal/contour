// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which daemons run on this computer: the socket directory listed, each control socket probed. The
/// directory and the probe are injected, so the decisions (which entries are daemons, in what order) are
/// tested without a daemon. Qt-free; the call blocks, so callers run it off the GUI thread.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vthost/DaemonSpawn.hpp>

namespace contour::cli
{

/// Asks the daemon at a socket how many of something it holds (workspaces, sessions); none when it could not
/// say.
using DaemonCounter = std::function<std::optional<std::size_t>(std::filesystem::path const& socket)>;

/// Lists the sockets in a directory (the filesystem, injected): only entries that are sockets, any name.
using DirectoryLister = std::function<std::vector<std::filesystem::path>(std::filesystem::path const& dir)>;

/// @return A lister that reads the real directory; a directory that cannot be read lists nothing.
[[nodiscard]] DirectoryLister nativeDirectoryLister();

/// A daemon found running on this computer.
struct DiscoveredDaemon
{
    std::string label;                     ///< Its socket label: the control socket's file name.
    std::filesystem::path socket;          ///< Its control socket.
    std::optional<std::size_t> workspaces; ///< How many workspaces it hosts, when it said.
};

/// Finds the daemons running locally.
class IDaemonDiscovery
{
  public:
    virtual ~IDaemonDiscovery() = default;

    /// @return The running daemons, `default` first, then by label. Blocking: call off the GUI thread.
    [[nodiscard]] virtual std::vector<DiscoveredDaemon> discover() = 0;
};

/// @param socketPath A path in the socket directory.
/// @return Whether its file name is a control socket's, not a sibling's (`-native`, `-tmux`, `.log`).
[[nodiscard]] bool isControlSocketName(std::filesystem::path const& socketPath);

/// The daemons among the probed entries.
///
/// Drops what the prober reports Absent (the stale socket of a dead daemon) and what is not a control
/// socket by name, asks each remaining daemon for its workspace count, and sorts by label with `default`
/// first.
/// @param entries Each candidate socket with what probing it said.
/// @param counter Asks a running daemon for its workspaces.
/// @return The running daemons.
[[nodiscard]] std::vector<DiscoveredDaemon> filterDiscovered(
    std::vector<std::pair<std::filesystem::path, vthost::DaemonPresence>> const& entries,
    DaemonCounter const& counter);

/// @param socketDirectory The directory the daemons' control sockets live in (the parent of
///        vthost::labelSocketPath("default")).
/// @param prober Asks whether a daemon serves at a socket; borrowed, outlives the discovery.
/// @param counter Asks a running daemon for its workspaces.
/// @param lister Lists the directory.
/// @return A discovery of the daemons in @p socketDirectory.
[[nodiscard]] std::unique_ptr<IDaemonDiscovery> makeLocalDaemonDiscovery(
    std::filesystem::path socketDirectory,
    vthost::DaemonSpawner& prober,
    DaemonCounter counter,
    DirectoryLister lister = nativeDirectoryLister());

} // namespace contour::cli
