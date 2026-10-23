// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

/// The shell a command line is written for: the one vtpty::SystemCommandCapture runs commands with.
enum class ShellDialect : uint8_t
{
    Posix,      ///< `/bin/sh -c <command>`.
    WindowsCmd, ///< `cmd.exe /c <command>`.
};

/// The dialect of the platform's command shell.
#ifdef _WIN32
inline constexpr auto NativeShellDialect = ShellDialect::WindowsCmd;
#else
inline constexpr auto NativeShellDialect = ShellDialect::Posix;
#endif

/// The characters cmd.exe interprets even inside double quotes (variable expansion, escapes) or that
/// end a command, which no quoting makes safe to pass: an argument holding one is refused.
inline constexpr auto WindowsCmdUnsafeCharacters = std::string_view { "\"%!^&|<>()\r\n" };

/// Writes a resolved argv as one shell command line that the shell splits back into exactly these
/// arguments, each taken literally: no expansion, no globbing, no command separators.
///
/// POSIX: every argument is single-quoted, an embedded `'` written as `'\''`. cmd.exe: every argument
/// is double-quoted by the MSVC runtime's (CommandLineToArgvW's) rules, so that the backslashes before
/// a closing quote are doubled, and the whole line quoted once more (cmd strips the outermost pair); an
/// argument that holds a character of WindowsCmdUnsafeCharacters, or any other control character, cannot be
/// quoted safely and is refused.
/// @param argv The command and its arguments; not empty.
/// @param dialect The shell the line is for.
/// @return The command line; `ArgumentRejected` if an argument cannot be written for @p dialect.
[[nodiscard]] std::expected<std::string, ExtensionError> shellCommandLine(std::span<std::string const> argv,
                                                                          ShellDialect dialect);

} // namespace vtextension
