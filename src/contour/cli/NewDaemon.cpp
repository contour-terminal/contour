// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/NewDaemon.hpp>

#include <core/Utf8Path.hpp>

#include <cstdlib>
#include <format>

namespace contour::cli
{

std::expected<NamedDaemonStart, std::string> startNamedDaemon(std::filesystem::path const& socket,
                                                              NamedDaemonSpawn const& spawn)
{
    if (spawn.spawner.probe(socket) == vthost::DaemonPresence::Serving)
        return NamedDaemonStart::AlreadyRunning;
    if (vthost::ensureDaemon(vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = socket } },
                             spawn.binary,
                             spawn.spawner,
                             spawn.placement,
                             spawn.options,
                             spawn.timeout,
                             vthost::DaemonLifecycle::Persistent)
        != EXIT_SUCCESS)
        return std::unexpected(std::format(
            "no daemon served on {} within {} seconds", core::pathToUtf8(socket), spawn.timeout.count()));
    return NamedDaemonStart::Started;
}

} // namespace contour::cli
