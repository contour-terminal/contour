// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a daemon-hosted shell is told about the daemon hosting it.
///
/// Its own header rather than a corner of SocketPath.hpp: the socket path is derived by everyone —
/// the GUI, `contour client`, both daemon entry points — while this is spoken only where the daemon
/// spawns, and it is the one thing here that needs vtpty/Process.hpp.

#include <vtpty/Process.hpp>

#include <core/Utf8Path.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

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
/// @return The variables to add to a hosted shell's environment, in UTF-8 as the spawn reads them.
[[nodiscard]] inline vtpty::Process::Environment hostedShellEnvironment(
    std::filesystem::path const& socketPath)
{
    return vtpty::Process::Environment { { "CONTOUR_MUX", core::pathToUtf8(socketPath) } };
}

/// What a spawn's command override is, against the daemon's configured shell.
enum class ShellOverride : uint8_t
{
    /// A layout pane's command and directory, laid over the daemon's shell: the host has no profiles.
    Overlays = 0,
    /// A whole shell the host resolved from a profile — program, arguments, directory and environment —
    /// which replaces the daemon's, so no variable of another profile reaches it. This is what the
    /// in-process GUI spawns for that profile (config::resolvedSessionConfig's shell).
    Replaces = 1,
};

/// The shell a hosted session is spawned with.
/// @param base The daemon's configured shell.
/// @param commandOverride What the spawn asked for; nullopt runs @p base.
/// @param mode How @p commandOverride relates to @p base. Replacing, a program-less override still runs
///        @p base's program and arguments.
/// @param hosted The daemon's own variables (@ref hostedShellEnvironment), which win over any profile's.
/// @return The shell to spawn.
[[nodiscard]] inline vtpty::Process::ExecInfo hostedShellFor(
    vtpty::Process::ExecInfo const& base,
    std::optional<vtpty::Process::ExecInfo> const& commandOverride,
    ShellOverride mode,
    vtpty::Process::Environment const& hosted)
{
    auto effective = base;
    if (commandOverride && mode == ShellOverride::Replaces)
    {
        effective = *commandOverride;
        if (effective.program.empty())
        {
            effective.program = base.program;
            effective.arguments = base.arguments;
        }
    }
    else
        vtpty::Process::applyCommandOverride(effective, commandOverride);
    // insert_or_assign, so the daemon wins over a profile that set the same name: a hosted shell being
    // told where a DIFFERENT daemon lives is never what was meant, and it has no other way to learn
    // where this one is.
    for (auto const& [name, value]: hosted)
        effective.env.insert_or_assign(name, value);
    return effective;
}

} // namespace vthost
