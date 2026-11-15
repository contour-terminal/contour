// SPDX-License-Identifier: Apache-2.0
//
// The environment an extension's manifest may set for the commands and panes it runs. A variable reaches a
// program the user granted, so one that makes it run something else -- LD_PRELOAD, PATH, NODE_OPTIONS, a git
// configuration that names a program -- would turn "it may run only these" into "it may run anything". Only
// what is listed here may be set, and only to a value of the listed shape; a manifest naming anything else is
// refused when it is read.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// How a settable variable's name is compared with a declared one.
enum class SettableName : uint8_t
{
    Exact,   ///< The declared name is the row's.
    Indexed, ///< The declared name is the row's followed by a decimal index: `GIT_CONFIG_KEY_0`.
};

/// What a settable variable's value must be.
enum class SettableValue : uint8_t
{
    Listed,         ///< One of the row's listed values, exactly.
    Decimal,        ///< A decimal number: digits, no sign, no leading zero.
    ObjectId,       ///< A git object name: 40 or 64 lower-case hexadecimal digits.
    GitProtocols,   ///< A colon-separated list of SettableGitProtocols.
    GitConfigKey,   ///< A key of SettableGitConfigs; its value, the variable of the same index, one it lists.
    GitConfigValue, ///< Judged with the key of the same index, by that key's row; free where none is set.
};

/// One row of the settable-variable table.
struct SettableVariable
{
    std::string_view name;                       ///< The name, or the name before the index.
    SettableName shape;                          ///< How @ref name is compared.
    SettableValue value;                         ///< What its value must be.
    std::span<std::string_view const> listed {}; ///< For SettableValue::Listed, the values allowed.
};

/// One row of the settable git configuration: a key `GIT_CONFIG_KEY_n` may name, and its allowed values.
/// What is not listed -- core.sshCommand, core.pager, credential.helper, any `*.command`, `alias.*`,
/// `include.*`, `includeIf.*`, `filter.*`, diff.external -- an extension never sets. A key that names a
/// program or a path is listed only with the value that turns it off.
struct SettableGitConfig
{
    std::string_view key; ///< The key, `section.name`, compared ignoring case as git does.
    std::span<std::string_view const> values; ///< The values it may take, exactly.
};

inline constexpr auto OffOnly = std::array<std::string_view, 1> { "0" };
inline constexpr auto CLocales = std::array<std::string_view, 3> { "C", "C.UTF-8", "POSIX" };
inline constexpr auto GitBooleans = std::array<std::string_view, 2> { "true", "false" };
inline constexpr auto GitFalse = std::array<std::string_view, 1> { "false" };
inline constexpr auto NullDevice = std::array<std::string_view, 1> { "/dev/null" };
inline constexpr auto PushDefaults =
    std::array<std::string_view, 5> { "nothing", "current", "upstream", "simple", "matching" };

/// The protocols GIT_ALLOW_PROTOCOL may list: never `ext`, which runs a command a URL spells.
inline constexpr auto SettableGitProtocols =
    std::array<std::string_view, 4> { "https", "ssh", "git", "file" };

/// The variables an extension's `command-env`, `command-env-for` and `pane-env` may set.
inline constexpr auto SettableVariables = std::array {
    // Never prompt for credentials: a command runs without a terminal to answer on.
    SettableVariable { .name = "GIT_TERMINAL_PROMPT",
                       .shape = SettableName::Exact,
                       .value = SettableValue::Listed,
                       .listed = OffOnly },
    SettableVariable {
        .name = "GIT_ALLOW_PROTOCOL", .shape = SettableName::Exact, .value = SettableValue::GitProtocols },
    // git's configuration from the environment, key by key: see SettableGitConfigs.
    SettableVariable {
        .name = "GIT_CONFIG_COUNT", .shape = SettableName::Exact, .value = SettableValue::Decimal },
    SettableVariable {
        .name = "GIT_CONFIG_KEY_", .shape = SettableName::Indexed, .value = SettableValue::GitConfigKey },
    SettableVariable {
        .name = "GIT_CONFIG_VALUE_", .shape = SettableName::Indexed, .value = SettableValue::GitConfigValue },
    // The attributes read from a tree (the empty one), not from a worktree an agent writes.
    SettableVariable {
        .name = "GIT_ATTR_SOURCE", .shape = SettableName::Exact, .value = SettableValue::ObjectId },
    SettableVariable { .name = "GIT_OPTIONAL_LOCKS",
                       .shape = SettableName::Exact,
                       .value = SettableValue::Listed,
                       .listed = OffOnly },
    // git's untranslated messages, for a caller that reads them.
    SettableVariable {
        .name = "LC_ALL", .shape = SettableName::Exact, .value = SettableValue::Listed, .listed = CLocales },
};

/// The git configuration keys GIT_CONFIG_KEY_n may name.
inline constexpr auto SettableGitConfigs = std::array {
    SettableGitConfig { .key = "push.default", .values = PushDefaults },
    SettableGitConfig { .key = "push.autoSetupRemote", .values = GitBooleans },
    // Off: no fsmonitor program, no hooks, no attributes file of the user's -- each would run, or read,
    // what a worktree an agent writes in provides.
    SettableGitConfig { .key = "core.fsmonitor", .values = GitFalse },
    SettableGitConfig { .key = "core.hooksPath", .values = NullDevice },
    SettableGitConfig { .key = "core.attributesFile", .values = NullDevice },
};

/// Checks one environment a manifest sets for a program -- a command's `command-env` and `command-env-for`
/// laid over each other, or a pane's `pane-env` -- against SettableVariables and SettableGitConfigs.
/// @param environment The environment as the program gets it: every name once.
/// @return Nothing when every variable may be set to its value; ForbiddenEnvironment for a name not
///         listed, or a value not of its row's shape; ForbiddenGitConfig for a git configuration key not
///         listed, or a value its key does not allow.
[[nodiscard]] std::expected<void, ManifestFault> checkSettableEnvironment(
    std::span<EnvironmentEntry const> environment);

/// Checks every environment @p manifest sets: `command-env` alone, each command's laid over it, and each
/// pane's. @see checkSettableEnvironment
/// @param manifest A parsed manifest.
/// @return Nothing, or the first fault found.
[[nodiscard]] std::expected<void, ManifestFault> checkSettableEnvironments(Manifest const& manifest);

} // namespace vtextension
