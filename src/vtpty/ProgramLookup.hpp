// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Where a program name leads: the PATH walk an exec does, and whether a file is one to run. Strings are
// UTF-8 here on every platform, as the configuration and the environment reader spell them.

namespace vtpty
{

/// Whether a string names a program to run. @see namesProgram
using ProgramPredicate = std::function<bool(std::string_view)>;

/// The character separating entries in the PATH environment variable on this platform.
inline constexpr char PathListSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

/// Resolves @p program to a path `execv` can actually run.
///
/// `execv` does NOT search PATH — and a client started as plain `contour` has exactly `contour` as
/// its argv[0], which is what the spawn inherits. Exec'ing it verbatim then fails with ENOENT, the
/// child exits, and the parent sits out its whole readiness timeout before reporting that the
/// daemon did not start: auto-spawn was broken for every invocation not spelled out as a path.
///
/// A name that carries a directory is already a path and passes through untouched (execvp's own
/// rule, so `./contour` and `/usr/bin/contour` keep meaning what they say). Anything else is looked
/// up in @p searchPath left-to-right, and the first entry that names an executable file in
/// @p fileSystem wins -- the filesystem's own answer, which on Windows counts an App Execution Alias
/// (a Store-installed pwsh.exe or winget) that is no regular file.
///
/// Resolution happens in the PARENT, before fork: between fork and exec only async-signal-safe
/// calls are allowed, and a PATH walk allocates.
/// @param program The binary as named by the caller (typically argv[0]).
/// @param searchPath The PATH value to search, @ref PathListSeparator separated.
/// @param fileSystem Where the candidates are looked for.
/// @return The resolved path, or @p program unchanged when nothing matched.
[[nodiscard]] std::string resolveExecutablePath(std::string_view program,
                                                std::string_view searchPath,
                                                core::platform::FileSystem const& fileSystem);

/// Where a spawn looks for the program it runs.
struct ProgramSearch
{
    /// The PATH a bare name is searched on, @ref PathListSeparator separated.
    std::string_view path;
    /// The directory a relative path names a file in: on POSIX the one the program runs in, which the
    /// child changes to before it execs; empty for this process's own, which CreateProcess reads one from.
    std::filesystem::path workingDirectory;
    /// The files of the machine the program runs on.
    core::platform::FileSystem const& fileSystem;
};

/// Finds the file @p program names for the spawn to run: a file at that path, or a name found on
/// @p search's PATH -- on Windows with ".exe" appended where it has no extension, as CreateProcess
/// appends it.
/// @param program The program as configured, UTF-8.
/// @param search  Where to look for it.
/// @return The file's path, UTF-8; std::nullopt where there is none.
[[nodiscard]] std::optional<std::string> findProgram(std::string_view program, ProgramSearch const& search);

/// @return Whether @p program names a program the spawn would run, as findProgram() finds one in @p search.
[[nodiscard]] bool namesProgram(std::string_view program, ProgramSearch const& search);

/// @return Whether a string names a program in @p search, as namesProgram() tells.
[[nodiscard]] ProgramPredicate programsIn(ProgramSearch const& search);

/// The machine a spawned program runs on, which is the one to look it up on.
enum class ProgramHost : std::uint8_t
{
    ThisMachine, ///< The program runs here.
    SandboxHost, ///< flatpak-spawn runs it on the host, outside the sandbox Contour runs in.
};

/// @return Whether a string names a program the spawn runs on @p host: as namesProgram() finds it in
///         @p search for this machine; never for the sandbox's host, whose files and PATH the sandbox
///         cannot see, and would answer about the wrong machine -- so that a program holding whitespace
///         is read as the command line it spells, as a shell reads one, and a host path with a space in
///         it is written quoted.
[[nodiscard]] ProgramPredicate programsFor(ProgramHost host, ProgramSearch const& search);

} // namespace vtpty
