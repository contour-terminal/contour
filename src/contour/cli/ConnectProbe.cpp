// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectProbe.hpp>

#include <vthost/Daemon.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>

namespace contour::cli
{

SessionCounter nativeSessionCounter()
{
    return [](std::filesystem::path const& socket) -> std::optional<std::size_t> {
        // No ensureDaemon: asking about a daemon never starts one.
        auto const control =
            vthost::client::makeDaemonControl(std::make_shared<vthost::UnixTransportFactory>(socket, nullptr),
                                              vthost::client::DefaultControlTimeout);
        auto const status = control->status();
        if (!status)
            return std::nullopt;
        return status->sessions;
    };
}

LocalDaemonState probeLocalDaemon(vthost::DaemonSpawner& spawner,
                                  std::filesystem::path const& socket,
                                  SessionCounter const& sessions)
{
    if (spawner.probe(socket) != vthost::DaemonPresence::Serving)
        return LocalDaemonState { .run = DaemonRunState::NotRunning, .sessions = std::nullopt };
    return LocalDaemonState { .run = DaemonRunState::Running, .sessions = sessions(socket) };
}

std::expected<void, std::string> checkHostReachable(vthost::client::DaemonControlPort& port)
{
    auto const status = port.status();
    if (status || status.error().code == vthost::client::ControlErrorCode::Unsupported)
        return {};
    return std::unexpected { status.error().toString() };
}

std::unique_ptr<vthost::client::DaemonControlPort> makeHostControl(
    std::shared_ptr<vthost::ITransportFactory> transport, std::chrono::milliseconds timeout)
{
    return vthost::client::makeDaemonControl(std::move(transport), timeout);
}

} // namespace contour::cli
