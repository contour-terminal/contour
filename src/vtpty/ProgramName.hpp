// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Comparing program names across platforms: a PTY reports `ssh` on POSIX and `ssh.exe` or `SSH.EXE` on
/// Windows for the same program. Dependency-free and constexpr, shared by every layer that matches a
/// foreground process against a table (shells, relays, a profile's shell).

#include <algorithm>
#include <string_view>

namespace vtpty
{

/// The program @p name names: its last path component (after either separator), without a trailing `.exe`
/// in any case.
/// @param name A process name or a program's path.
/// @return Its stem; empty for an empty name or a bare `.exe`.
[[nodiscard]] constexpr std::string_view programStem(std::string_view name) noexcept
{
    constexpr auto Exe = std::string_view { ".exe" };
    if (auto const separator = name.find_last_of("/\\"); separator != std::string_view::npos)
        name.remove_prefix(separator + 1);
    auto const lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (name.size() >= Exe.size()
        && std::ranges::equal(name.substr(name.size() - Exe.size()), Exe, {}, lower))
        name.remove_suffix(Exe.size());
    return name;
}

/// Whether @p lhs and @p rhs name the same program: their stems (programStem()) equal, ignoring ASCII case.
/// @param lhs A process name or a program's path.
/// @param rhs Another one.
/// @return Whether they name the same program; never for two empty stems.
[[nodiscard]] constexpr bool isSameProgram(std::string_view lhs, std::string_view rhs) noexcept
{
    auto const lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    auto const left = programStem(lhs);
    return !left.empty() && std::ranges::equal(left, programStem(rhs), {}, lower, lower);
}

} // namespace vtpty
