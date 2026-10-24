// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What `contour daemon --stdio` does: the remote end of an ssh attach, relaying the native protocol
/// between its standard input/output and this user's daemon.

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <vthost/ByteStream.hpp>

namespace vthost
{

class DaemonSpawner;
class DaemonStarter;

/// Reaches the local daemon's native socket, starting the daemon when needed. Injected so the relay is
/// testable.
using DaemonConnector = std::function<std::expected<std::shared_ptr<IByteStream>, std::string>()>;

/// What `contour daemon --stdio` does: connect, write the preamble, relay. Writes nothing else to
/// @p client: no PDU is parsed, so a relay of one release serves any protocol version the two ends
/// negotiate.
/// @param client This process's stdio (makeProcessStdioStream()).
/// @param connect Reaches the daemon.
/// @return EXIT_SUCCESS after the daemon side ended; EXIT_FAILURE after writing an error preamble, or
///         when the relay failed.
[[nodiscard]] int runStdioAttach(std::shared_ptr<IByteStream> client, DaemonConnector const& connect);

/// The production connector: when no daemon serves @p socketPath, @p starter starts one; then a
/// blocking connect to its native socket (`nativeSocketPath(socketPath)`).
/// @param socketPath The daemon's control socket.
/// @param spawner Asks whether a daemon serves (DaemonSpawner::probe()); not owned, outlives the connector.
/// @param starter Phase 2's `chooseDaemonStarter(...)`: a registered user service when there is one,
///        else spawn-on-demand from the staged per-version binary. Owned by the connector.
/// @return The connector.
[[nodiscard]] DaemonConnector makeLocalDaemonConnector(std::filesystem::path socketPath,
                                                       DaemonSpawner& spawner,
                                                       std::unique_ptr<DaemonStarter> starter);

} // namespace vthost
