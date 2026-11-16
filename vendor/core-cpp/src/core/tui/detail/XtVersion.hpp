// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The part of the XTVERSION capability probe that reads the same on every platform:
/// what a `CSI > q` reply means. Only the reading of the reply differs — POSIX polls
/// stdin, Windows drains console input records — so `posix/TerminalOutput.cpp` and
/// `windows/TerminalOutput.cpp` bring their own bytes and share the answer.
///
/// In endo these two functions were duplicated in `platform/TerminalOutput.cpp` and
/// `platform/TerminalOutputWin32.cpp` (f774a210).

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace core::tui::detail
{

/// @brief Extracts the terminal name from an XTVERSION reply.
///
/// The reply is `DCS > | <name-and-version> ST`, for example
/// `"\033P>|kitty(0.26.5)\033\\"`, and both the 7-bit (`ESC P`, `ESC \`) and the
/// 8-bit (0x90, 0x9C) framings are accepted. The version, in parentheses or after a
/// space, is dropped.
///
/// @param response The raw bytes read back from the terminal.
/// @return The lowercased terminal name, or empty when the reply is not one.
[[nodiscard]] inline auto parseXtVersionName(std::string_view response) -> std::string
{
    // Look for the DCS > | prefix: ESC P > | or 0x90 > |
    auto pos = response.find("\033P>|");
    if (pos == std::string_view::npos)
    {
        pos = response.find("\x90>|");
        if (pos == std::string_view::npos)
            return {};
        pos += 3; // Skip 0x90 > |
    }
    else
    {
        pos += 4; // Skip ESC P > |
    }

    // Find ST: ESC \ or 0x9C
    auto end = response.find("\033\\", pos);
    if (end == std::string_view::npos)
    {
        end = response.find('\x9C', pos);
        if (end == std::string_view::npos)
            end = response.size();
    }

    auto name = std::string(response.substr(pos, end - pos));

    // Extract just the terminal name (before version info)
    // e.g., "kitty(0.26.5)" -> "kitty", "contour 0.4.3" -> "contour"
    if (auto const parenPos = name.find('('); parenPos != std::string::npos)
        name = name.substr(0, parenPos);
    if (auto const spacePos = name.find(' '); spacePos != std::string::npos)
        name = name.substr(0, spacePos);

    for (char& c: name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return name;
}

/// @brief Whether a terminal known by @p name implements the Kitty unscroll extension.
///
/// `CSI Ps + T` restores lines from the scrollback instead of inserting blank ones, and
/// it is not advertised by any query, so the terminal is recognised by name.
///
/// @param name A terminal name as @c parseXtVersionName() returns it.
/// @return true for the terminals known to support it.
[[nodiscard]] inline auto supportsUnscroll(std::string_view name) -> bool
{
    constexpr auto Known = std::array<std::string_view, 3> { "kitty", "contour", "mintty" };
    return std::ranges::find(Known, name) != Known.end();
}

} // namespace core::tui::detail
