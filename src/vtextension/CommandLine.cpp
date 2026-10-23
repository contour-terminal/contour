// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CommandLine.hpp>

#include <algorithm>

namespace vtextension
{

namespace
{
    /// @return @p argument single-quoted for a POSIX shell.
    [[nodiscard]] std::string posixQuoted(std::string_view argument)
    {
        auto quoted = std::string { "'" };
        for (auto const c: argument)
        {
            if (c == '\'')
                quoted += "'\\''";
            else
                quoted += c;
        }
        quoted += '\'';
        return quoted;
    }

    /// @return @p argument double-quoted by the rules the MSVC runtime (and CommandLineToArgvW) splits a
    ///         command line by: a run of backslashes is literal unless a `"` follows it, so the run before
    ///         the closing quote (or before an embedded `"`) is doubled, and an embedded `"` escaped.
    [[nodiscard]] std::string msvcrtQuoted(std::string_view argument)
    {
        auto quoted = std::string { "\"" };
        auto backslashes = std::size_t { 0 };
        for (auto const c: argument)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            if (c == '"')
                backslashes = (2 * backslashes) + 1;
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted += c;
        }
        quoted.append(2 * backslashes, '\\');
        quoted += '"';
        return quoted;
    }

    /// @return Whether cmd.exe takes @p argument literally once double-quoted.
    [[nodiscard]] bool isCmdQuotable(std::string_view argument) noexcept
    {
        return std::ranges::none_of(argument, [](char c) {
            auto const byte = static_cast<unsigned char>(c);
            return byte < 0x20 || byte == 0x7F || WindowsCmdUnsafeCharacters.contains(c);
        });
    }
} // namespace

std::expected<std::string, ExtensionError> shellCommandLine(std::span<std::string const> argv,
                                                            ShellDialect dialect)
{
    auto line = std::string {};
    switch (dialect)
    {
        case ShellDialect::Posix:
            for (auto const& argument: argv)
            {
                if (!line.empty())
                    line += ' ';
                line += posixQuoted(argument);
            }
            return line;
        case ShellDialect::WindowsCmd:
            if (!std::ranges::all_of(argv, isCmdQuotable))
                return std::unexpected(ExtensionError::ArgumentRejected);
            line += '"';
            for (auto const& argument: argv)
            {
                if (line.size() > 1)
                    line += ' ';
                line += msvcrtQuoted(argument);
            }
            line += '"';
            return line;
    }
    return std::unexpected(ExtensionError::ArgumentRejected);
}

} // namespace vtextension
