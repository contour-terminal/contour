// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which program a restored pane types back into its fresh shell: only one the user allow-listed, and only
/// a command line that a shell reads literally.

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <vthost/SessionSnapshot.hpp>

namespace vthost
{

namespace detail
{
    /// The characters that separate the words of a command line.
    inline constexpr auto RerunWhitespace = std::string_view { " \t" };

    /// What makes a shell read a command line as more than words: command separators, expansions,
    /// redirections, grouping, bash's history expansion (`!`) and the quotes that would leave an interactive
    /// shell waiting for more lines, and `^`, a stderr redirection to fish before 4.0; NUL, CR and LF end or
    /// split the line. The backslash is listed apart, in @ref RerunEscape: it is a path separator in the
    /// program's own word on Windows.
    inline constexpr auto RerunMetacharacters = std::string_view { ";&|$`<>(){}!'\"^\0\r\n", 18 };

    /// The escape character, refused everywhere except in the program's word.
    inline constexpr auto RerunEscape = '\\';

    /// @param text A command line or one of its words.
    /// @return @p text without its leading and trailing whitespace.
    [[nodiscard]] constexpr std::string_view trimmed(std::string_view text) noexcept
    {
        auto const first = text.find_first_not_of(RerunWhitespace);
        if (first == std::string_view::npos)
            return {};
        auto const last = text.find_last_not_of(RerunWhitespace);
        return text.substr(first, last - first + 1);
    }

    /// @param line A trimmed command line.
    /// @return Its first word: the program as the shell was asked to run it.
    [[nodiscard]] constexpr std::string_view programWord(std::string_view line) noexcept
    {
        return line.substr(0, line.find_first_of(RerunWhitespace));
    }

    /// @param word A program word: a bare name, or a POSIX or Windows path.
    /// @return Its file name, without the extension when that is `.exe` in any case.
    [[nodiscard]] constexpr std::string_view programName(std::string_view word) noexcept
    {
        if (auto const slash = word.find_last_of("/\\"); slash != std::string_view::npos)
            word.remove_prefix(slash + 1);
        constexpr auto Exe = std::string_view { ".exe" };
        if (word.size() > Exe.size()
            && std::ranges::equal(word.substr(word.size() - Exe.size()), Exe, [](char a, char b) {
                   return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
               }))
            word.remove_suffix(Exe.size());
        return word;
    }

    /// @param line A trimmed command line.
    /// @return Whether a shell reads @p line as nothing but its words.
    [[nodiscard]] constexpr bool isLiteral(std::string_view line) noexcept
    {
        auto const program = programWord(line);
        return line.find_first_of(RerunMetacharacters) == std::string_view::npos
               && !line.substr(program.size()).contains(RerunEscape);
    }
} // namespace detail

/// The bytes to type into a restored shell to re-run what @p pane ran, or nullopt.
///
/// The program is the pane's foreground process, else the first word of its running command line, by file
/// name (an `.exe` extension dropped); it must equal an entry of @p allowedPrograms exactly. The whole
/// command line is typed only when its own program is that one and it holds no shell metacharacter (see
/// detail::RerunMetacharacters), so a forged OSC 133 command line runs nothing it names; otherwise the
/// program's name alone is, when the foreground process named it.
/// @param pane The pane as it was captured.
/// @param allowedPrograms The programs the user allowed to be re-run (`daemon.restore.rerun_programs`).
/// @return The line to type, ending in a carriage return; nullopt for nothing to re-run.
[[nodiscard]] inline std::optional<std::string> rerunInputFor(PaneSnapshot const& pane,
                                                              std::span<std::string const> allowedPrograms)
{
    if (allowedPrograms.empty())
        return std::nullopt;
    auto const line =
        pane.runningCommandLine ? detail::trimmed(*pane.runningCommandLine) : std::string_view {};
    auto const candidate = pane.foregroundProcess ? detail::programName(*pane.foregroundProcess)
                                                  : detail::programName(detail::programWord(line));
    if (candidate.empty() || !std::ranges::contains(allowedPrograms, candidate))
        return std::nullopt;
    if (!line.empty() && detail::programName(detail::programWord(line)) == candidate
        && detail::isLiteral(line))
        return std::string { line } + '\r';
    if (pane.foregroundProcess)
        return std::string { candidate } + '\r';
    return std::nullopt;
}

} // namespace vthost
