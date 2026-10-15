// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>

#include <vtpty/ProgramLookup.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/WindowsCommandLine.hpp>

#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtpty
{

namespace
{
    /// What ends a word on a command line outside quotes.
    constexpr auto Whitespace = std::string_view { " \t" };
} // namespace

std::vector<std::string> shellSplit(std::string_view commandLine)
{
    auto tokens = std::vector<std::string> {};
    auto current = std::string {};
    auto inToken = false;

    // Consumes the quoted run whose opening quote is at rest.front(), appending its content to
    // `current`. Single quotes are literal; inside double quotes, \" and \\ are escapes. An
    // unterminated quote simply consumes the remainder.
    auto const consumeQuoted = [&current](std::string_view& rest, char const quote) {
        rest.remove_prefix(1); // the opening quote
        while (!rest.empty() && rest.front() != quote)
        {
            if (quote == '"' && rest.front() == '\\' && rest.size() >= 2
                && (rest[1] == '"' || rest[1] == '\\'))
                rest.remove_prefix(1); // drop the backslash, take the escaped character below
            current.push_back(rest.front());
            rest.remove_prefix(1);
        }
        if (!rest.empty())
            rest.remove_prefix(1); // the closing quote
    };

    auto const endToken = [&] {
        if (!inToken)
            return;
        tokens.push_back(std::move(current));
        current.clear();
        inToken = false;
    };

    auto rest = commandLine;
    while (!rest.empty())
    {
        auto const c = rest.front();
        if (c == '\'' || c == '"')
        {
            inToken = true;
            consumeQuoted(rest, c);
        }
        else if (c == '\\' && rest.size() >= 2)
        {
            // Outside quotes, a backslash escapes the next character (e.g. `a\ b` is one token).
            inToken = true;
            current.push_back(rest[1]);
            rest.remove_prefix(2);
        }
        else if (Whitespace.contains(c))
        {
            endToken();
            rest.remove_prefix(1);
        }
        else
        {
            inToken = true;
            current.push_back(c);
            rest.remove_prefix(1);
        }
    }
    endToken();
    return tokens;
}

std::optional<ProgramInvocation> readAsCommandLine(std::string_view program,
                                                   std::span<std::string const> arguments,
                                                   ProgramPredicate const& namesProgram)
{
    // Whitespace first: a single word is a program or nothing, and no reading can make it either.
    if (program.find_first_of(Whitespace) == std::string_view::npos || namesProgram(program))
        return std::nullopt;

#ifdef _WIN32
    auto reading = readWindowsProgram(program, namesProgram);
    if (reading.program.empty())
        return std::nullopt;

    return ProgramInvocation { .program = std::move(reading.program),
                               .writtenArguments = std::move(reading.rest),
                               .arguments = { arguments.begin(), arguments.end() } };
#else
    auto words = shellSplit(program);
    if (words.empty())
        return std::nullopt;

    auto invocation =
        ProgramInvocation { .program = std::move(words.front()), .writtenArguments = {}, .arguments = {} };
    invocation.arguments.insert(invocation.arguments.end(),
                                std::make_move_iterator(words.begin() + 1),
                                std::make_move_iterator(words.end()));
    invocation.arguments.insert(invocation.arguments.end(), arguments.begin(), arguments.end());
    return invocation;
#endif
}

ProgramInvocation invocationToRun(std::string const& program,
                                  std::vector<std::string> const& arguments,
                                  ProgramPredicate const& namesProgram)
{
    if (auto reading = readAsCommandLine(program, arguments, namesProgram))
    {
        ptyLog()("\"{}\" names no program to run: read as a command line, whose program is {}.",
                 program,
                 reading->program);
        return std::move(*reading);
    }
    return ProgramInvocation { .program = program, .writtenArguments = {}, .arguments = arguments };
}

std::string buildWindowsCommandLine(ProgramInvocation const& invocation)
{
    auto commandLine = buildWindowsCommandLine(invocation.program, {});
    commandLine += invocation.writtenArguments;
    if (!invocation.arguments.empty())
        commandLine.append(" ").append(buildWindowsArguments(invocation.arguments));
    return commandLine;
}

} // namespace vtpty
