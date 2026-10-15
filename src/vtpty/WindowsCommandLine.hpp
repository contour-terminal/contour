// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ProgramLookup.hpp>

#include <span>
#include <string>
#include <string_view>

// Windows hands a child one command line, which the child splits into argv itself: the Microsoft C runtime
// and CommandLineToArgvW() split it at every space or tab outside double quotes, and read a backslash
// that precedes a double quote as an escape ("Parsing C command-line arguments", Microsoft Learn). These
// spell a command line so that the child's split gives back exactly what was passed in -- the way
// Windows Terminal, alacritty and wezterm spell theirs.
//
// Pure, and compiled on every platform, so that they are tested everywhere and not only where they run.

namespace vtpty
{

/// Spells @p argument for a Windows command line so that the C runtime reads it back unchanged: wrapped
/// in double quotes, a double quote in it written \", and the backslashes before one, or before the
/// closing quote, doubled.
/// @param argument The argument, as the child is to receive it.
/// @return It quoted, the surrounding double quotes included.
[[nodiscard]] std::string quoteWindowsArgument(std::string_view argument);

/// Spells @p arguments for a Windows command line, one space between each: an argument that is empty, or
/// holds whitespace or a double quote, by quoteWindowsArgument(), and any other as it is, so that a program
/// that splits its command line its own way -- cmd.exe -- still reads a plain one as before.
/// @param arguments The arguments, as the child is to receive them.
/// @return Them spelled; empty for none.
[[nodiscard]] std::string buildWindowsArguments(std::span<std::string const> arguments);

/// Builds the command line CreateProcess() takes to run @p program with @p arguments.
///
/// - @p program is wrapped in double quotes when it holds a space or a tab, as
///   "C:\Program Files\PowerShell\7\pwsh.exe" does: unquoted, CreateProcess tries each space-ended
///   prefix of it in turn, and runs "C:\Program.exe" wherever there is one. The C runtime reads a
///   program name without escapes, so nothing inside it is escaped. A program that already holds a
///   double quote -- which no file name can -- was quoted by whoever wrote it, and is passed on as it is.
/// - The arguments follow it as buildWindowsArguments() spells them.
///
/// @param program   The program to run, a path or a name to look up on PATH.
/// @param arguments The arguments the program is to receive, after its own name.
/// @return The command line, in the same encoding as its parts.
[[nodiscard]] std::string buildWindowsCommandLine(std::string_view program,
                                                  std::span<std::string const> arguments);

/// A command line read as far as CreateProcess() reads one itself: the program it runs.
struct WindowsProgramReading
{
    std::string program; ///< The program, without the double quotes that may group it.
    std::string rest;    ///< What follows the program, as written: its leading whitespace, and its arguments.

    bool operator==(WindowsProgramReading const&) const = default;
};

/// Reads the program off the front of @p commandLine as CreateProcess() does, and leaves the rest as it
/// is: the child splits that itself, cmd.exe by rules of its own. An unquoted program runs up to the first
/// whitespace at which what precedes it names a program, tried from the left -- so that
/// "C:\Program Files\Git\bin\bash.exe --login" runs bash.exe, not C:\Program -- or up to the first
/// whitespace where none does; a quoted one up to its closing quote.
/// @param commandLine  The command line.
/// @param namesProgram Whether a string names a program to run.
/// @return The program and what follows it; an empty program for an empty or blank @p commandLine.
[[nodiscard]] WindowsProgramReading readWindowsProgram(std::string_view commandLine,
                                                       ProgramPredicate const& namesProgram);

} // namespace vtpty
