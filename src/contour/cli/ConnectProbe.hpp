// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the Connect picker asks of the outside before and while it connects: is the local daemon up, and
/// does a host answer. Each takes its collaborator, so a test hands it a fake.

#include <contour/cli/ConnectList.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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

/// Asks the daemon at a socket how many sessions it hosts; none when it could not say.
using SessionCounter = std::function<std::optional<std::size_t>(std::filesystem::path const& socket)>;

/// @return A counter that asks a running daemon through its control socket, and never starts one.
[[nodiscard]] SessionCounter nativeSessionCounter();

/// Probes the local daemon.
/// @param spawner Asks whether a daemon accepts connections at @p socket.
/// @param socket The daemon's control socket.
/// @param sessions Asks a running daemon for its session count.
/// @return Not running when nothing accepts; else running, with the count when it was had.
[[nodiscard]] LocalDaemonState probeLocalDaemon(vthost::DaemonSpawner& spawner,
                                                std::filesystem::path const& socket,
                                                SessionCounter const& sessions);

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
    std::shared_ptr<vthost::ITransportFactory> transport, std::chrono::milliseconds timeout);

} // namespace contour::cli
