// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's PTY factories: what a hosted session's shell is spawned as, and on whose device. Their own
/// header rather than corners of Daemon.cpp, so that what a spawned child sees -- its directory, the names
/// withheld from its environment -- is tested through the very factories the daemon spawns with.

#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>

#include <filesystem>
#include <functional>
#include <memory>

#include <vthost/SessionHost.hpp>

namespace vthost
{

/// Makes the device a hosted shell runs on, at the size given: its process on a fresh PTY, whoever is its
/// parent. Shared by the daemon's PTY factories, so that one decides who spawns for both.
using ShellDevices =
    std::function<std::unique_ptr<vtpty::Pty>(vtbackend::PageSize, vtpty::Process::ExecInfo, SandboxEscape)>;

/// @param placement Moves each hosted shell into its own resource domain.
/// @return Shell devices this daemon spawns itself, each a child of its own (vtpty::Process).
[[nodiscard]] ShellDevices ownShellDevices(std::shared_ptr<vtpty::ProcessPlacement> placement);

/// The daemon's PTY factory: every session spawns the configured shell over a fresh PTY. Shared by the POSIX
/// and Windows runDaemon paths.
///
/// @param shell         What to spawn; a spawn's override (a layout pane's command and directory, an agent
///                      pane's program, the names it withholds and the directory it holds open) is laid over
///                      it (ShellOverride::Overlays). A host with profiles spawns through
///                      makeProfilePtyFactory.
/// @param sandbox       Whether the spawned shell escapes its sandbox (Flatpak), from the resolved profile's
///                      `escape_sandbox`.
/// @param socketPath    The daemon's control socket, which every hosted shell is told about so it can reach
///                      back. Merged in here rather than by the caller, so that every entry into runDaemon()
///                      hosts shells that can find their own daemon.
/// @param devices       Makes each shell's device: spawned by this daemon, or by its PTY keeper.
/// @return The factory.
[[nodiscard]] PtyFactory makeShellPtyFactory(vtpty::Process::ExecInfo shell,
                                             SandboxEscape sandbox,
                                             std::filesystem::path const& socketPath,
                                             ShellDevices devices);

/// The PTY factory of a daemon with profiles: each session runs its resolved profile's whole shell, with that
/// profile's own environment and sandbox policy, as the in-process GUI spawns that profile (SessionFactory
/// takes `escape_sandbox` from the session's own profile).
/// @param shell      The daemon's default shell, whose program runs when a profile names none.
/// @param socketPath The daemon's control socket, which every hosted shell is told about.
/// @param devices    Makes each shell's device: spawned by this daemon, or by its PTY keeper.
/// @return The factory.
[[nodiscard]] ProfilePtyFactory makeProfilePtyFactory(vtpty::Process::ExecInfo shell,
                                                      std::filesystem::path const& socketPath,
                                                      ShellDevices devices);

} // namespace vthost
