// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProgramLookup.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The rules a Windows spawn obeys, as pure string work: which file a program name starts, which PATH entries
// are searched, and the environment a child receives. No std::filesystem here, whose is_absolute answers
// differently on Linux, so these rules test the same on every platform. Strings are UTF-8.

namespace vtpty
{

/// The longest command line CreateProcessW takes, in UTF-16 units.
inline constexpr std::size_t MaxWindowsCommandLineUnits = 32'766;

/// The longest current directory a Windows process may have, in UTF-16 units: MAX_PATH less the trailing `\`
/// and the NUL.
inline constexpr std::size_t MaxWindowsCurrentDirectoryUnits = 258;

/// The extensions a program file may have to be started: one row, so that the rule is data.
inline constexpr auto WindowsImageExtensions = std::array { std::string_view { ".exe" } };

/// @param path A path, UTF-8.
/// @return Whether @p path is absolute as Win32 reads it: `C:\…` or `C:/…`, `\\server\share…`, `\\?\…`.
///         Drive-relative `C:x`, root-relative `\x`, relative and double-quoted paths are not.
[[nodiscard]] bool isAbsoluteWindowsPath(std::string_view path) noexcept;

/// @param program A program's file name or path, UTF-8.
/// @return The file it names to start: @p program with `.exe` appended where its last component has no
///         extension; @p program itself where the extension is in WindowsImageExtensions (ASCII
///         case-insensitive); std::nullopt for any other extension (`.cmd`, `.bat`, `.com`, `.ps1`, …), a
///         trailing dot, or an empty name.
[[nodiscard]] std::optional<std::string> windowsImageName(std::string_view program);

/// Finds the file a program runs from, for a spawn into a held directory, as findHeldDirectoryProgram() does
/// on POSIX, with Windows' rules: windowsImageName() first; an absolute program is taken as it is; a relative
/// one carrying a directory (`.\x`, `bin\x`, `C:x`) is refused; a bare name is looked up on the absolute
/// entries of @p searchPath (`;`-separated) only.
/// @param program The program as declared.
/// @param searchPath The PATH it runs under.
/// @param isExecutable Whether a candidate (absolute, UTF-8) names a file to run.
/// @return The absolute path to start; std::nullopt where none is found or the program is refused.
[[nodiscard]] std::optional<std::string> findWindowsHeldDirectoryProgram(std::string_view program,
                                                                         std::string_view searchPath,
                                                                         ExecutableTest const& isExecutable);

/// @param environment A spec's environment, in order.
/// @return It as "NAME=VALUE" entries, names that differ only in ASCII case collapsed to the last of them.
[[nodiscard]] std::vector<std::string> windowsEnvironmentEntries(
    std::span<std::pair<std::string, std::string> const> environment);

/// @param environment A spec's environment, in order.
/// @return The value of its PATH, the name compared ASCII-case-insensitively, the last one winning; empty
///         where it has none.
[[nodiscard]] std::string windowsSearchPathOf(
    std::span<std::pair<std::string, std::string> const> environment);

} // namespace vtpty
