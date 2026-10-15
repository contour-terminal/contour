// SPDX-License-Identifier: Apache-2.0
#include <vtpty/WindowsCommandLine.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace vtpty
{

namespace
{
    /// The C runtime's whitespace: what ends a word outside double quotes.
    constexpr auto Whitespace = std::string_view { " \t" };

    /// What an argument may not hold unquoted: whitespace, which would end it, and the double quote,
    /// which would open a quoted part. LF and VT as well, as wezterm quotes them: quoting costs nothing,
    /// and not every child splits its command line with the C runtime.
    constexpr auto ArgumentSpecials = std::string_view { " \t\n\v\"" };

    /// Takes the program name off the front of @p rest: a double quote only groups, and nothing escapes.
    [[nodiscard]] std::string takeProgram(std::string_view& rest)
    {
        auto program = std::string {};
        auto quoted = false;
        // Whitespace outside a quoted part ends the word.
        while (!rest.empty() && (quoted || !Whitespace.contains(rest.front())))
        {
            if (rest.front() == '"')
                quoted = !quoted;
            else
                program += rest.front();
            rest.remove_prefix(1);
        }
        return program;
    }

    /// Drops the whitespace at the front of @p rest.
    void skipBlanks(std::string_view& rest) noexcept
    {
        rest.remove_prefix(std::min(rest.find_first_not_of(Whitespace), rest.size()));
    }
} // namespace

std::string quoteWindowsArgument(std::string_view argument)
{
    auto quoted = std::string { '"' };
    auto backslashes = std::size_t { 0 };
    for (auto const ch: argument)
    {
        // 2n+1 backslashes before a literal double quote: n backslashes, then the quote itself.
        if (ch == '"')
            quoted.append(backslashes + 1, '\\');
        backslashes = ch == '\\' ? backslashes + 1 : 0;
        quoted += ch;
    }
    // 2n backslashes before the closing quote: n backslashes, and the quote still closes.
    quoted.append(backslashes, '\\');
    quoted += '"';
    return quoted;
}

std::string buildWindowsArguments(std::span<std::string const> arguments)
{
    auto spelled = std::string {};
    auto separator = std::string_view {};
    for (auto const& argument: arguments)
    {
        spelled += std::exchange(separator, " ");
        if (argument.empty() || argument.find_first_of(ArgumentSpecials) != std::string::npos)
            spelled += quoteWindowsArgument(argument);
        else
            spelled += argument;
    }
    return spelled;
}

std::string buildWindowsCommandLine(std::string_view program, std::span<std::string const> arguments)
{
    auto commandLine = std::string {};
    if (program.find_first_of(Whitespace) != std::string_view::npos && !program.contains('"'))
        commandLine.append("\"").append(program).append("\"");
    else
        commandLine += program;

    if (!arguments.empty())
        commandLine.append(" ").append(buildWindowsArguments(arguments));
    return commandLine;
}

WindowsProgramReading readWindowsProgram(std::string_view commandLine, ProgramPredicate const& namesProgram)
{
    auto rest = commandLine;
    skipBlanks(rest);
    if (rest.empty() || rest.starts_with('"'))
    {
        auto program = takeProgram(rest);
        return WindowsProgramReading { .program = std::move(program), .rest = std::string { rest } };
    }

    // Where a word ends: whitespace after a character that is not.
    auto wordEnds = std::views::iota(std::size_t { 1 }, rest.size()) | std::views::filter([rest](auto index) {
                        return Whitespace.contains(rest[index]) && !Whitespace.contains(rest[index - 1]);
                    });
    auto const programEnd =
        std::ranges::find_if(wordEnds, [&](auto index) { return namesProgram(rest.substr(0, index)); });
    if (programEnd == wordEnds.end())
    {
        auto program = takeProgram(rest);
        return WindowsProgramReading { .program = std::move(program), .rest = std::string { rest } };
    }
    return WindowsProgramReading { .program = std::string { rest.substr(0, *programEnd) },
                                   .rest = std::string { rest.substr(*programEnd) } };
}

} // namespace vtpty
