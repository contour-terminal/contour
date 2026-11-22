// SPDX-License-Identifier: Apache-2.0
//
// The programs a declared command may name that the daemon runs as its own executable: a command that runs
// `contour` runs the daemon's own binary, so that it works from a .dmg, an AppImage or a launchd agent, whose
// PATH holds no `contour`. Beside them, the programs the user's configuration names an executable for: a
// daemon started by a service manager often has a PATH that holds none of the user's own bin directories.
#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>

namespace vtextension
{

/// The names under which a declared command runs the daemon's own executable. Adding one is adding a row.
inline constexpr auto SelfProgramNames = std::array { std::string_view { "contour" } };

/// @param executable The daemon's executable, absolute (vtpty::currentExecutablePath()).
/// @return Each of SelfProgramNames, run as @p executable.
[[nodiscard]] std::vector<SelfProgram> selfProgramsOf(std::filesystem::path const& executable);

/// A program an extension's argv names and the executable run for it, whatever the daemon's PATH holds.
struct ProgramPath
{
    std::string name; ///< The program as a declared command's or pane template's argv names it: no `/`.
    std::string path; ///< The executable run for it: absolute.

    [[nodiscard]] bool operator==(ProgramPath const&) const = default;
};

/// The table an extension's programs are resolved by: the user's (`daemon.extensions.<name>.programs`) first,
/// so that they win, then the daemon's self programs.
/// @param configured What the user's configuration names an executable for.
/// @param self What the host runs as its own executable.
/// @return The table, in that order.
[[nodiscard]] std::vector<ProgramPath> programTableOf(std::span<ProgramPath const> configured,
                                                      std::span<SelfProgram const> self);

/// Replaces the program of @p argv by the executable of the first entry of @p programs that names it; leaves
/// it to the PATH search otherwise.
/// @param argv A declared command's or pane template's argv, its placeholders substituted.
/// @param programs The table (programTableOf()).
void resolveProgram(std::vector<std::string>& argv, std::span<ProgramPath const> programs);

} // namespace vtextension
