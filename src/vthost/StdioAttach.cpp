// SPDX-License-Identifier: Apache-2.0
#include <vthost/StdioAttach.hpp>

#include <core/log/LogStore.hpp>

#include <cstdlib>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/PlatformStreams.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/StdioPreamble.hpp>

namespace vthost
{

namespace
{
    /// Writes @p line to @p client.
    /// @return Whether the client took it; a client gone already is logged, as there is no one else to tell.
    [[nodiscard]] std::expected<void, std::string> writeLine(IByteStream& client, std::string_view line)
    {
        auto written = client.writeAll(std::as_bytes(std::span { line }));
        if (!written)
            errorLog()("contour daemon --stdio: cannot write to the client: {}", written.error());
        return written;
    }
} // namespace

int runStdioAttach(std::shared_ptr<IByteStream> client, DaemonConnector const& connect)
{
    auto daemon = connect();
    if (!daemon)
    {
        std::ignore = writeLine(*client, stdioPreambleError(daemon.error()));
        client->closeWrite();
        return EXIT_FAILURE;
    }
    if (!writeLine(*client, StdioPreambleReady))
        return EXIT_FAILURE;
    return runStdioRelay(std::move(client), *std::move(daemon)) == RelayEnd::DaemonEnded ? EXIT_SUCCESS
                                                                                         : EXIT_FAILURE;
}

DaemonConnector makeLocalDaemonConnector(std::filesystem::path socketPath,
                                         DaemonSpawner& spawner,
                                         std::unique_ptr<DaemonStarter> starter)
{
    return [socketPath = std::move(socketPath), &spawner, starter = std::shared_ptr { std::move(starter) }]()
               -> std::expected<std::shared_ptr<IByteStream>, std::string> {
        if (spawner.probe(socketPath) == DaemonPresence::Absent)
            if (auto const started = starter->start(); !started)
                return std::unexpected { started.error() };
        return connectUnixStream(nativeSocketPath(socketPath));
    };
}

} // namespace vthost
