// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utils.hpp>

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
    Cmd,        //!< Double quotes; no way to escape `%`, `"` or (delayed expansion) `!` inside them.
    /// The shell is not known (no foreground process to ask: Windows, a daemon's pane, or a transport such
    /// as ssh whose far side is anyone's). No quoting is right for all shells, so a name holding any
    /// character one of them reads as syntax (@c detail::UnknownShellMetacharacters) is refused, and the
    /// rest is double-quoted.
    Unknown,
};

/// A row of the quoting table: a foreground process and how its shell reads paths.
struct ShellQuotingRow
{
    std::string_view process; //!< The process's base name, lower case, without `.exe`.
    ShellQuoting quoting;     //!< How it reads a path.
};

/// The processes that do not read POSIX escapes, and the transports whose far side's shell is unknown.
/// Everything else is @c ShellQuoting::Posix.
inline constexpr auto ShellQuotingRows = std::array {
    ShellQuotingRow { .process = "pwsh", .quoting = ShellQuoting::PowerShell },
    ShellQuotingRow { .process = "powershell", .quoting = ShellQuoting::PowerShell },
    ShellQuotingRow { .process = "cmd", .quoting = ShellQuoting::Cmd },
    ShellQuotingRow { .process = "ssh", .quoting = ShellQuoting::Unknown },
    ShellQuotingRow { .process = "wsl", .quoting = ShellQuoting::Unknown },
    ShellQuotingRow { .process = "sudo", .quoting = ShellQuoting::Unknown },
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
/// @param foregroundProcess The foreground process's name or path; empty when it is not known.
/// @return The quoting of the matching row; @c ShellQuoting::Unknown when there is no process to name;
///         @c ShellQuoting::Posix when none matches.
[[nodiscard]] constexpr ShellQuoting shellQuotingFor(std::string_view foregroundProcess) noexcept
{
    if (foregroundProcess.empty())
        return ShellQuoting::Unknown;
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
    ControlCharacter, //!< A name holds a control character (U+0000-U+001F, U+007F, U+0080-U+009F), or one
                      //!< that changes what the text looks like (a bidi control, U+2028, U+2029).
    Unquotable,       //!< The shell has no way to quote a character in a name (`%`, `"` or `!` under cmd).
    UnknownShell,     //!< The shell is not known, and a name holds a character some shell reads as syntax.
};

namespace detail
{
    /// The characters a POSIX shell (and zsh, and fish) treat specially in a word, besides a space.
    inline constexpr auto PosixSpecials = std::string_view { "\\!\"#$&'()*,;<=>?[]^`{|}~ " };

    /// What a name may not hold when the shell is not known: the characters at least one of POSIX shells,
    /// PowerShell and cmd reads as syntax even inside double quotes, or that quoting itself needs. A
    /// backslash is in the list: Qt hands a local file over with `/` as its separator on every platform.
    inline constexpr auto UnknownShellMetacharacters = std::string_view { "$`;&|<>()'\"\\!%^" };

    /// PowerShell reads these typographic quotes as single quotes; each is doubled like `'`.
    inline constexpr auto PowerShellQuotes =
        std::array<std::string_view, 5> { "'", "\u2018", "\u2019", "\u201A", "\u201B" };

    /// Whether @p path holds a C0 or C1 control character, or DEL, in UTF-8.
    [[nodiscard]] inline bool holdsControlCharacter(std::string_view path) noexcept
    {
        constexpr auto C1Lead = '\xC2';
        for (auto const [i, c]: core::views::enumerate(path))
        {
            auto const byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return true;
            if (c == C1Lead && i + 1 < path.size())
            {
                auto const next = static_cast<unsigned char>(path[i + 1]);
                if (next >= 0x80 && next <= 0x9F)
                    return true;
            }
        }
        return false;
    }

    /// Whether @p path holds a character that makes the typed text look other than it runs: the bidi
    /// embeddings, overrides and isolates (U+202A..U+202E, U+2066..U+2069) and the line and paragraph
    /// separators (U+2028, U+2029), which sanitizeCommandLine() replaces for display too.
    [[nodiscard]] inline bool holdsDeceptiveCharacter(std::string_view path) noexcept
    {
        constexpr auto Lead = '\xE2';
        for (auto const [i, c]: core::views::enumerate(path))
        {
            if (c != Lead || i + 2 >= path.size())
                continue;
            auto const second = static_cast<unsigned char>(path[i + 1]);
            auto const third = static_cast<unsigned char>(path[i + 2]);
            if (second == 0x80 && third >= 0xA8 && third <= 0xAE) // U+2028..U+202E
                return true;
            if (second == 0x81 && third >= 0xA6 && third <= 0xA9) // U+2066..U+2069
                return true;
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

    /// PowerShell reads these typographic double quotes as `"`; inside double quotes they end the string.
    inline constexpr auto PowerShellDoubleQuotes =
        std::array<std::string_view, 3> { "\u201C", "\u201D", "\u201E" };

    /// Whether @p path holds what is unsafe to type when the shell is not known.
    [[nodiscard]] inline bool isUnsafeForUnknownShell(std::string_view path) noexcept
    {
        return path.find_first_of(UnknownShellMetacharacters) != std::string_view::npos
               || std::ranges::any_of(PowerShellDoubleQuotes,
                                      [&](std::string_view q) { return path.contains(q); });
    }

    [[nodiscard]] inline bool isUnquotableForCmd(std::string_view path) noexcept
    {
        return path.find_first_of("%\"!") != std::string_view::npos;
    }
} // namespace detail

/// Turns dropped file paths into the text typed at the prompt of the pane's shell.
///
/// Each path is escaped for @p quoting and followed by one space, so typing can go on. A path holding
/// a control character refuses the whole drop, because the text is typed into a shell: a newline would
/// press Return. So does one holding a bidi control or U+2028/U+2029, which would make the typed text look
/// other than what runs. Under cmd a path holding `%`, `"` or `!` is refused too, as cmd expands `%NAME%`
/// (and, with delayed expansion, `!NAME!`) inside double quotes and cannot escape `"`.
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
        if (detail::holdsControlCharacter(path) || detail::holdsDeceptiveCharacter(path))
            return std::unexpected(DropTextError::ControlCharacter);
        if (quoting == ShellQuoting::Cmd && detail::isUnquotableForCmd(path))
            return std::unexpected(DropTextError::Unquotable);
        if (quoting == ShellQuoting::Unknown && detail::isUnsafeForUnknownShell(path))
            return std::unexpected(DropTextError::UnknownShell);
    }

    auto text = std::string {};
    for (auto const& path: paths)
    {
        switch (quoting)
        {
            case ShellQuoting::Posix: detail::appendPosix(text, path); break;
            case ShellQuoting::PowerShell: detail::appendPowerShell(text, path); break;
            case ShellQuoting::Cmd:
            case ShellQuoting::Unknown:
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
