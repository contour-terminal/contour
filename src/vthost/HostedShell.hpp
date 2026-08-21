// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a daemon-hosted shell is told about the daemon hosting it.
///
/// Its own header rather than a corner of SocketPath.hpp: the socket path is derived by everyone —
/// the GUI, `contour client`, both daemon entry points — while this is spoken only where the daemon
/// spawns, and it is the one thing here that needs vtpty/Process.hpp.

#include <vtpty/Process.hpp>

#include <filesystem>

namespace vthost
{

/// The environment a daemon-hosted shell is started with.
///
/// One named, testable decision rather than an assignment buried in CLI argument parsing: what a
/// hosted shell is told is the daemon's business, so it is applied where the daemon spawns
/// (makeShellPtyFactory) rather than where one of its callers happens to parse arguments. It will
/// grow.
///
/// `$CONTOUR_MUX` is here because a hosted shell cannot DERIVE where its own daemon is. It is
/// derived everywhere else, but the derivation differs on the two sides of a sandbox boundary --
/// and with escape_sandbox (the default) that shell runs on the host while the daemon that spawned
/// it does not. @see muxSocketPath.
///
/// Note what is NOT here and arguably should be: `TERM`, `COLORTERM` and `TERM_PROGRAM`, which the
/// GUI path sets from YAMLConfigReader::defaultSettings() and which a daemon-hosted shell has
/// therefore never received at all. That is a pre-existing gap, named here rather than fixed,
/// because closing it means deciding which layer owns the terminfo answer.
///
/// @param socketPath The daemon's control socket, as it bound it.
/// @return The variables to add to a hosted shell's environment.
[[nodiscard]] inline vtpty::Process::Environment hostedShellEnvironment(
    std::filesystem::path const& socketPath)
{
    return vtpty::Process::Environment { { "CONTOUR_MUX", socketPath.string() } };
}

} // namespace vthost
