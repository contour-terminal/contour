// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtpty
{

/// How environment variable names compare.
enum class EnvironmentNames : std::uint8_t
{
    CaseSensitive,   ///< As on POSIX, where "Path" and "PATH" are two variables.
    CaseInsensitive, ///< As on Windows, where they are one.
};

/// How this platform compares environment variable names.
inline constexpr auto PlatformEnvironmentNames =
#ifdef _WIN32
    EnvironmentNames::CaseInsensitive;
#else
    EnvironmentNames::CaseSensitive;
#endif

/// Builds a child's environment as "NAME=VALUE" entries.
/// @param inherited The parent's entries ("NAME=VALUE").
/// @param overrides Variables to add, or to replace in @p inherited.
/// @param removed Names dropped from @p inherited; an override of the same name still applies.
/// @param names How the names in all three compare.
/// @return The entries, in no particular order.
[[nodiscard]] std::vector<std::string> buildEnvironmentBlock(
    std::span<std::string_view const> inherited,
    std::map<std::string, std::string> const& overrides,
    std::span<std::string const> removed,
    EnvironmentNames names);

/// Builds a Windows child's environment as "NAME=VALUE" entries: buildEnvironmentBlock() with names compared
/// as Windows compares them, and three rules of its own.
/// - TERM, if configured, is not passed on: a console program has no use for it, and some -- git diff --
///   misread it.
/// - A configured PATH goes ahead of the inherited one rather than in its place.
/// - A configured empty value removes the variable, as setting none would.
/// @param inherited The parent's entries ("NAME=VALUE"), the "=C:" ones that say each drive's directory
///                  among them.
/// @param configured The variables configured for the child.
/// @param removed Names dropped from @p inherited; a configured variable of the same name still applies.
/// @return The entries, in no particular order.
[[nodiscard]] std::vector<std::string> buildWindowsEnvironment(
    std::span<std::string_view const> inherited,
    std::map<std::string, std::string> const& configured,
    std::span<std::string const> removed);

/// @return The value @p name has in @p entries ("NAME=VALUE"), compared as @p names says; std::nullopt where
///         it has none.
[[nodiscard]] std::optional<std::string_view> environmentValue(std::span<std::string const> entries,
                                                               std::string_view name,
                                                               EnvironmentNames names);

#ifdef _WIN32
/// Spells @p entries as the environment block CreateProcessW() takes with CREATE_UNICODE_ENVIRONMENT: each
/// entry in UTF-16 and ended by a NUL, sorted by name as Windows requires -- case-insensitively, in Unicode
/// order, without regard to locale -- and one NUL more after the last.
/// @param entries The entries ("NAME=VALUE"), UTF-8.
/// @return The block; std::nullopt where an entry is not UTF-8.
[[nodiscard]] std::optional<std::wstring> windowsEnvironmentBlock(std::span<std::string const> entries);
#endif

} // namespace vtpty
