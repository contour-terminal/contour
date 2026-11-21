// SPDX-License-Identifier: Apache-2.0
//
// The programs a declared command may name that the daemon runs as its own executable: a command that runs
// `contour` runs the daemon's own binary, so that it works from a .dmg, an AppImage or a launchd agent, whose
// PATH holds no `contour`.
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

/// Replaces the program of @p argv by its executable when it names one of @p programs.
/// @param argv A declared command's argv, its placeholders substituted.
/// @param programs The programs the host runs as its own executable.
void resolveSelfProgram(std::vector<std::string>& argv, std::span<SelfProgram const> programs);

} // namespace vtextension
