// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProgramLookup.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtpty
{

/// Splits a command line into tokens the way a shell would: whitespace separates tokens; single quotes
/// ('...') quote a run literally; double quotes ("...") quote a run allowing \" and \\ escapes; a
/// backslash outside quotes escapes the next character. Used so a layout `command:` may be written as a
/// full command line ("emacs -nw") rather than requiring a separate `arguments:` list. Returns an empty
/// vector for an empty/whitespace-only input.
[[nodiscard]] std::vector<std::string> shellSplit(std::string_view commandLine);

/// A program and the arguments it receives, as a spawn runs them.
struct ProgramInvocation
{
    std::string program; ///< A path, or a name to look up on PATH.
    /// What a command line configured as the program held after it, as written. A Windows child is handed
    /// it unchanged, ahead of @c arguments: it splits its command line itself, and cmd.exe by rules of its
    /// own. Always empty on POSIX, where those words are among @c arguments.
    std::string writtenArguments;
    std::vector<std::string> arguments; ///< What it receives after its own name.

    bool operator==(ProgramInvocation const&) const = default;
};

/// Reads @p program as a whole command line when it names no program to run but holds whitespace, as a
/// `shell:` setting written "wsl.exe -d Ubuntu" or "bash -l" does: its first word becomes the program, and
/// the rest is passed on ahead of @p arguments -- on POSIX as the words a POSIX shell reads (shellSplit()),
/// on Windows as written, after the program CreateProcess would run (readWindowsProgram()), so that
/// "C:\Program Files\Git\bin\bash.exe --login" runs bash.exe. A program that names a file to run is
/// never split, spaces or not -- "C:\Program Files\PowerShell\7\pwsh.exe" runs as it is.
///
/// Pure but for @p namesProgram, so that the rule is testable without the programs it names.
/// @param program      The program as configured.
/// @param arguments    The arguments configured for it.
/// @param namesProgram Whether a string names a program to run.
/// @return What to run instead; std::nullopt when @p program is to run as it is.
[[nodiscard]] std::optional<ProgramInvocation> readAsCommandLine(std::string_view program,
                                                                 std::span<std::string const> arguments,
                                                                 ProgramPredicate const& namesProgram);

/// What a spawn runs for @p program and @p arguments: readAsCommandLine(), logged when it reads @p program
/// as a command line.
/// @param program      The program as configured.
/// @param arguments    The arguments configured for it.
/// @param namesProgram Whether a string names a program to run, as the machine the program runs on
///                     answers it (programsIn()).
/// @return The program and arguments to run.
[[nodiscard]] ProgramInvocation invocationToRun(std::string const& program,
                                                std::vector<std::string> const& arguments,
                                                ProgramPredicate const& namesProgram);

/// Builds the command line CreateProcess() takes to run @p invocation: its program and arguments as
/// buildWindowsCommandLine() spells them, and its written arguments between the two, as they are.
/// @param invocation What to run.
/// @return The command line.
[[nodiscard]] std::string buildWindowsCommandLine(ProgramInvocation const& invocation);

} // namespace vtpty
