// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the Connect picker asks of the outside before and while it connects: is the local daemon up, and
/// does a host answer. Each takes its collaborator, so a test hands it a fake.

#include <contour/cli/ConnectList.hpp>
#include <contour/cli/DaemonDiscovery.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vthost/AttachBudget.hpp>

namespace vthost
{
class DaemonSpawner;
class ITransportFactory;
} // namespace vthost

namespace vthost::client
{
class DaemonControlPort;
}

namespace contour::cli
{

/// @return A counter of the sessions a running daemon hosts, asked through its control socket; never starts
///         one.
[[nodiscard]] DaemonCounter nativeSessionCounter();

/// @return A counter of the workspaces a running daemon holds, asked through its control socket; never starts
///         one. None from a daemon too old to say.
[[nodiscard]] DaemonCounter nativeWorkspaceCounter();

/// Probes the local daemon.
/// @param spawner Asks whether a daemon accepts connections at @p socket.
/// @param socket The daemon's control socket.
/// @param sessions Asks a running daemon for its session count.
/// @return Not running when nothing accepts; else running, with the count when it was had.
[[nodiscard]] LocalDaemonState probeLocalDaemon(vthost::DaemonSpawner& spawner,
                                                std::filesystem::path const& socket,
                                                DaemonCounter const& sessions);

/// Whether a host answers: connects, handshakes and asks for its status. A daemon that is too old to
/// answer a status is reachable all the same.
/// @param port Daemon control over the host's transport.
/// @return Nothing when it answers, or the transport's own classified words (host key, authentication,
///         contour missing, unreachable): what Attach to Host shows too.
[[nodiscard]] std::expected<void, std::string> checkHostReachable(vthost::client::DaemonControlPort& port);

/// @param transport How to reach the host.
/// @param timeout How long the check may take.
/// @return Daemon control over @p transport, for checkHostReachable().
[[nodiscard]] std::unique_ptr<vthost::client::DaemonControlPort> makeHostControl(
    std::shared_ptr<vthost::ITransportFactory> transport, vthost::AttachBudget timeout);

} // namespace contour::cli
