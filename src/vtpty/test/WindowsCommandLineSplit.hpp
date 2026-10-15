// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/WindowsCommandLine.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The other half of WindowsCommandLine.hpp, which only the tests need: reading a command line back into
// the words the child's C runtime makes of it, so that what buildWindowsCommandLine() spells can be
// checked against the rules that read it ("Parsing C command-line arguments", Microsoft Learn).

namespace vtpty::testing
{

/// The C runtime's whitespace: what ends a word outside double quotes.
inline constexpr auto WindowsCommandLineWhitespace = std::string_view { " \t" };

/// Takes one argument off the front of @p rest, by the C runtime's rules for every word after the
/// program's: 2n backslashes before a double quote are n backslashes and the quote groups, 2n+1 are n
/// backslashes and a literal quote, and inside a quoted part "" is a literal quote. Backslashes before
/// anything else are as they are.
/// @param rest The command line from the argument on; left holding what follows it.
/// @return The argument.
[[nodiscard]] inline std::string takeWindowsArgument(std::string_view& rest)
{
    auto argument = std::string {};
    auto quoted = false;
    while (!rest.empty() && (quoted || !WindowsCommandLineWhitespace.contains(rest.front())))
    {
        if (auto const backslashes = std::min(rest.find_first_not_of('\\'), rest.size()); backslashes > 0)
        {
            auto const beforeQuote = backslashes < rest.size() && rest[backslashes] == '"';
            argument.append(beforeQuote ? backslashes / 2 : backslashes, '\\');
            rest.remove_prefix(backslashes);
            // An odd run escapes the quote after it; an even one leaves it to group.
            if (beforeQuote && backslashes % 2 == 1)
            {
                argument += '"';
                rest.remove_prefix(1);
            }
        }
        else if (rest.front() == '"' && quoted && rest.starts_with("\"\""))
        {
            argument += '"'; // "" inside a quoted part
            rest.remove_prefix(2);
        }
        else if (rest.front() == '"')
        {
            quoted = !quoted;
            rest.remove_prefix(1);
        }
        else
        {
            argument += rest.front();
            rest.remove_prefix(1);
        }
    }
    return argument;
}

/// Splits @p commandLine as the Microsoft C runtime splits its own: the inverse of
/// buildWindowsCommandLine().
///
/// - The program is read as readWindowsProgram() reads one that names no program at any of its spaces:
///   up to the first space or tab outside double quotes. A double quote only groups, and nothing
///   escapes -- a backslash is a directory separator.
/// - Each argument after it as takeWindowsArgument() reads it.
///
/// @param commandLine The command line.
/// @return Its words, the program first; empty for an empty or blank @p commandLine.
[[nodiscard]] inline std::vector<std::string> splitWindowsCommandLine(std::string_view commandLine)
{
    if (commandLine.find_first_not_of(WindowsCommandLineWhitespace) == std::string_view::npos)
        return {};

    auto const reading = readWindowsProgram(commandLine, [](std::string_view) { return false; });
    auto words = std::vector { reading.program };
    auto rest = std::string_view { reading.rest };
    auto const skipBlanks = [&rest] {
        rest.remove_prefix(std::min(rest.find_first_not_of(WindowsCommandLineWhitespace), rest.size()));
    };
    skipBlanks();
    while (!rest.empty())
    {
        words.push_back(takeWindowsArgument(rest));
        skipBlanks();
    }
    return words;
}

} // namespace vtpty::testing
