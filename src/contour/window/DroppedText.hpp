// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace contour::window
{

/// How the shell in a pane reads a path typed at its prompt.
enum class ShellQuoting : uint8_t
{
    Posix = 0,  //!< Backslash-escapes; what bash, zsh, fish, sh, vim's `:e` and a remote shell read.
    PowerShell, //!< Single quotes, a single quote doubled.
    Cmd,        //!< Double quotes; no way to escape `%` or `"` inside them.
};

/// A row of the quoting table: a foreground process and how its shell reads paths.
struct ShellQuotingRow
{
    std::string_view process; //!< The process's base name, lower case, without `.exe`.
    ShellQuoting quoting;     //!< How it reads a path.
};

/// The processes that do not read POSIX escapes. Everything else is @c ShellQuoting::Posix.
inline constexpr auto ShellQuotingRows = std::array {
    ShellQuotingRow { .process = "pwsh", .quoting = ShellQuoting::PowerShell },
    ShellQuotingRow { .process = "powershell", .quoting = ShellQuoting::PowerShell },
    ShellQuotingRow { .process = "cmd", .quoting = ShellQuoting::Cmd },
};

namespace detail
{
    /// ASCII lower-casing of one character, independent of the locale.
    [[nodiscard]] constexpr char asciiLower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// Whether @p a and @p b are equal in ASCII, ignoring case.
    [[nodiscard]] constexpr bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(
            a, b, [](char lhs, char rhs) { return asciiLower(lhs) == asciiLower(rhs); });
    }
} // namespace detail

/// Picks the quoting for the shell whose foreground process is @p foregroundProcess.
///
/// Only the base name counts (a path in either separator style), its arguments are ignored, and a
/// trailing `.exe` is ignored, in any case.
/// @param foregroundProcess The foreground process's name or path; may be empty.
/// @return The quoting of the matching row; @c ShellQuoting::Posix when none matches.
[[nodiscard]] constexpr ShellQuoting shellQuotingFor(std::string_view foregroundProcess) noexcept
{
    if (auto const slash = foregroundProcess.find_last_of("/\\"); slash != std::string_view::npos)
        foregroundProcess.remove_prefix(slash + 1);
    // A configured shell is a command line ("pwsh -NoLogo"): the program is its first word.
    foregroundProcess = foregroundProcess.substr(0, foregroundProcess.find(' '));
    constexpr auto Exe = std::string_view { ".exe" };
    if (foregroundProcess.size() >= Exe.size()
        && detail::equalsIgnoringCase(foregroundProcess.substr(foregroundProcess.size() - Exe.size()), Exe))
        foregroundProcess.remove_suffix(Exe.size());
    for (auto const& row: ShellQuotingRows)
        if (detail::equalsIgnoringCase(foregroundProcess, row.process))
            return row.quoting;
    return ShellQuoting::Posix;
}

/// Why dropped paths produced no text.
enum class DropTextError : uint8_t
{
    NoPaths = 0,      //!< There was nothing to type.
    ControlCharacter, //!< A name holds a control character (U+0000-U+001F, U+007F, U+0080-U+009F).
    Unquotable,       //!< The shell has no way to quote a character in a name (`%` or `"` under cmd).
};

namespace detail
{
    /// The characters a POSIX shell (and zsh, and fish) treat specially in a word, besides a space.
    inline constexpr auto PosixSpecials = std::string_view { "\\!\"#$&'()*,;<=>?[]^`{|}~ " };

    /// PowerShell reads these typographic quotes as single quotes; each is doubled like `'`.
    inline constexpr auto PowerShellQuotes =
        std::array<std::string_view, 5> { "'", "\u2018", "\u2019", "\u201A", "\u201B" };

    /// Whether @p path holds a C0 or C1 control character, or DEL, in UTF-8.
    [[nodiscard]] inline bool holdsControlCharacter(std::string_view path) noexcept
    {
        constexpr auto C1Lead = '\xC2';
        for (auto const [i, c]: std::views::enumerate(path))
        {
            auto const byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return true;
            if (c == C1Lead && i + 1 < static_cast<std::ptrdiff_t>(path.size()))
            {
                auto const next = static_cast<unsigned char>(path[static_cast<size_t>(i) + 1]);
                if (next >= 0x80 && next <= 0x9F)
                    return true;
            }
        }
        return false;
    }

    inline void appendPosix(std::string& out, std::string_view path)
    {
        // A relative name that starts with '-' would be read as an option.
        if (path.starts_with('-'))
            out += "./";
        for (auto const c: path)
        {
            if (PosixSpecials.contains(c))
                out += '\\';
            out += c;
        }
    }

    inline void appendPowerShell(std::string& out, std::string_view path)
    {
        out += '\'';
        while (!path.empty())
        {
            auto const quote = std::ranges::find_if(PowerShellQuotes,
                                                    [&](std::string_view q) { return path.starts_with(q); });
            if (quote != PowerShellQuotes.end())
            {
                out += *quote;
                out += *quote;
                path.remove_prefix(quote->size());
            }
            else
            {
                out += path.front();
                path.remove_prefix(1);
            }
        }
        out += '\'';
    }

    [[nodiscard]] inline bool isUnquotableForCmd(std::string_view path) noexcept
    {
        return path.find_first_of("%\"") != std::string_view::npos;
    }
} // namespace detail

/// Turns dropped file paths into the text typed at the prompt of the pane's shell.
///
/// Each path is escaped for @p quoting and followed by one space, so typing can go on. A path holding
/// a control character refuses the whole drop, because the text is typed into a shell: a newline would
/// press Return. Under cmd a path holding `%` or `"` is refused too, as cmd expands `%NAME%` inside
/// double quotes and cannot escape `"`.
/// @param paths The paths, in UTF-8.
/// @param quoting How the pane's shell reads them.
/// @return The text, or why there is none.
[[nodiscard]] inline std::expected<std::string, DropTextError> droppedPathsText(
    std::span<std::string const> paths, ShellQuoting quoting)
{
    if (paths.empty())
        return std::unexpected(DropTextError::NoPaths);

    // Validate the whole drop first: one bad name refuses all of it, so nothing is half typed.
    for (auto const& path: paths)
    {
        if (detail::holdsControlCharacter(path))
            return std::unexpected(DropTextError::ControlCharacter);
        if (quoting == ShellQuoting::Cmd && detail::isUnquotableForCmd(path))
            return std::unexpected(DropTextError::Unquotable);
    }

    auto text = std::string {};
    for (auto const& path: paths)
    {
        switch (quoting)
        {
            case ShellQuoting::Posix: detail::appendPosix(text, path); break;
            case ShellQuoting::PowerShell: detail::appendPowerShell(text, path); break;
            case ShellQuoting::Cmd:
                text += '"';
                text += path;
                text += '"';
                break;
        }
        text += ' ';
    }
    return text;
}

} // namespace contour::window
