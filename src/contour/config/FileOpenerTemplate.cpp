// SPDX-License-Identifier: Apache-2.0
#include <contour/config/FileOpenerTemplate.hpp>

#include <vtpty/CommandLine.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace contour::config
{

namespace
{
    /// What fills the placeholders when no location was printed.
    constexpr auto NoLocation = vtbackend::LinkLocation { .line = 1, .column = 1 };
} // namespace

std::string_view describe(FileOpenerError error) noexcept
{
    switch (error)
    {
        case FileOpenerError::Empty: return "no program to open files with";
        case FileOpenerError::UnknownPlaceholder:
            return "an unknown placeholder; use {path}, {line} and {column}";
        case FileOpenerError::UnterminatedPlaceholder: return "a '{' without its '}'";
        case FileOpenerError::ProgramNotWrittenOut:
            return "the program must be written out, without a placeholder";
    }
    return "an unknown problem";
}

std::vector<std::string> detail::splitWrittenArguments(std::string_view text)
{
    auto arguments = std::vector<std::string> {};
    auto current = std::string {};
    auto inArgument = false; // "" is an argument too
    auto quoted = false;
    for (auto const ch: text)
    {
        if (ch == '"')
        {
            quoted = !quoted;
            inArgument = true;
        }
        else if (!quoted && (ch == ' ' || ch == '\t'))
        {
            if (inArgument)
                arguments.push_back(std::exchange(current, std::string {}));
            inArgument = false;
        }
        else
        {
            current += ch;
            inArgument = true;
        }
    }
    if (inArgument)
        arguments.push_back(std::move(current));
    return arguments;
}

std::expected<FileOpenerTemplate, FileOpenerError> FileOpenerTemplate::parse(std::string_view command)
{
    // Every placeholder file_opener knows, and what fills it.
    static constexpr auto Placeholders = std::array {
        std::pair { std::string_view { "path" }, Slot::Path },
        std::pair { std::string_view { "line" }, Slot::Line },
        std::pair { std::string_view { "column" }, Slot::Column },
    };

    if (command.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return std::unexpected(FileOpenerError::Empty);

    // A whole command line, never one program whose name holds a space: such a path is written quoted.
    auto const invocation =
        vtpty::readAsCommandLine(command, {}, [](std::string_view) { return false; })
            .value_or(vtpty::ProgramInvocation {
                .program = std::string { command }, .writtenArguments = {}, .arguments = {} });
    auto words = std::vector<std::string> { invocation.program };
    std::ranges::copy(invocation.arguments, std::back_inserter(words));
    // Windows only: POSIX leaves nothing written after the program.
    std::ranges::copy(detail::splitWrittenArguments(invocation.writtenArguments), std::back_inserter(words));

    auto arguments = std::vector<std::vector<Piece>> {};
    for (auto const& word: words)
    {
        auto pieces = std::vector<Piece> {};
        auto rest = std::string_view { word };
        // A walk over the word: literal text up to the next placeholder, then the placeholder.
        while (!rest.empty())
        {
            auto const open = rest.find('{');
            if (open != 0)
            {
                pieces.push_back(
                    Piece { .slot = Slot::Literal, .literal = std::string { rest.substr(0, open) } });
                if (open == std::string_view::npos)
                    break;
                rest.remove_prefix(open);
            }
            auto const close = rest.find('}');
            if (close == std::string_view::npos)
                return std::unexpected(FileOpenerError::UnterminatedPlaceholder);
            auto const known = std::ranges::find(
                Placeholders, rest.substr(1, close - 1), &std::pair<std::string_view, Slot>::first);
            if (known == Placeholders.end())
                return std::unexpected(FileOpenerError::UnknownPlaceholder);
            pieces.push_back(Piece { .slot = known->second, .literal = {} });
            rest.remove_prefix(close + 1);
        }
        arguments.push_back(std::move(pieces));
    }

    // The program is written out: an empty one a pane replaces with the shell, and one filled from printed
    // text a pane's spawn would read as a command line again.
    auto const& program = arguments.front();
    if (program.empty()
        || std::ranges::any_of(program, [](Piece const& piece) { return piece.slot != Slot::Literal; }))
        return std::unexpected(FileOpenerError::ProgramNotWrittenOut);
    return FileOpenerTemplate { std::move(arguments) };
}

std::vector<std::string> FileOpenerTemplate::expand(std::string_view path,
                                                    std::optional<vtbackend::LinkLocation> location) const
{
    auto const where = location.value_or(NoLocation);
    auto arguments = std::vector<std::string> {};
    arguments.reserve(_arguments.size());
    for (auto const& pieces: _arguments)
    {
        auto argument = std::string {};
        for (auto const& piece: pieces)
            switch (piece.slot)
            {
                case Slot::Literal: argument += piece.literal; break;
                case Slot::Path: argument += path; break;
                case Slot::Line: argument += std::to_string(where.line); break;
                case Slot::Column: argument += std::to_string(where.column); break;
            }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

} // namespace contour::config
