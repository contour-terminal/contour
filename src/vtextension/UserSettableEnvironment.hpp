// SPDX-License-Identifier: Apache-2.0
//
// The variables a user may set in an extension's configuration (`daemon.extensions.<name>.env_sets`): which
// names a set may hold, and the shape each value must have before the host applies it.
#pragma once

#include <vtpty/PathStyle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// What a user-set variable's value must be.
enum class UserValueShape : uint8_t
{
    AbsolutePath,     ///< A directory: rootSpellingOf() spells it, and it must be absolute.
    LocalOrSecureUrl, ///< An https:// URL, or an http:// one to the loopback host.
    ProxyUrl,         ///< A http, https, socks5 or socks5h URL without credentials.
    HostList,         ///< A comma-separated list of host names, domains or addresses.
};

/// One row of the user-settable table.
struct UserSettableVariable
{
    std::string_view name; ///< The variable's name, compared exactly.
    UserValueShape shape;  ///< The shape its value must have.
};

/// The variables a user's `env_sets` may hold: a separate list from SettableVariables, which a manifest's
/// literal environment uses. No API key is offered.
inline constexpr auto UserSettableVariables = std::array {
    UserSettableVariable { .name = "CLAUDE_CONFIG_DIR", .shape = UserValueShape::AbsolutePath },
    UserSettableVariable { .name = "COPILOT_HOME", .shape = UserValueShape::AbsolutePath },
    UserSettableVariable { .name = "ANTHROPIC_BASE_URL", .shape = UserValueShape::LocalOrSecureUrl },
    UserSettableVariable { .name = "HTTPS_PROXY", .shape = UserValueShape::ProxyUrl },
    UserSettableVariable { .name = "HTTP_PROXY", .shape = UserValueShape::ProxyUrl },
    UserSettableVariable { .name = "NO_PROXY", .shape = UserValueShape::HostList },
};

/// The longest URL or host list a user may set, in bytes.
inline constexpr std::size_t MaxUserValueBytes = 2048;

/// Why a user-set value is refused.
enum class UserValueFault : uint8_t
{
    NotSettable,     ///< The name is not in UserSettableVariables.
    NotAbsolutePath, ///< An AbsolutePath value is not an absolute path, or starts with `~` without a home.
    NotAllowedUrl,   ///< A LocalOrSecureUrl value is neither https://, nor http:// to a loopback host.
    NotUrl,          ///< A ProxyUrl value is not an http, https, socks5 or socks5h URL without credentials.
    NotHostList,     ///< A HostList value is not a comma-separated list of host names.
};

/// The text of one UserValueFault.
struct UserValueFaultText
{
    UserValueFault fault;  ///< The fault the row describes.
    std::string_view text; ///< What the fault says, after the variable's name.
};

/// One row per UserValueFault, indexed by its value.
inline constexpr auto UserValueFaultTexts = std::array {
    UserValueFaultText { .fault = UserValueFault::NotSettable,
                         .text = "is not a variable a set may hold: CLAUDE_CONFIG_DIR, COPILOT_HOME, "
                                 "ANTHROPIC_BASE_URL, HTTPS_PROXY, HTTP_PROXY or NO_PROXY" },
    UserValueFaultText { .fault = UserValueFault::NotAbsolutePath,
                         .text = "is not an absolute path, or one starting with ~" },
    UserValueFaultText { .fault = UserValueFault::NotAllowedUrl,
                         .text =
                             "is not an https:// URL, or an http:// one to localhost, 127.0.0.1 or [::1], "
                             "without credentials" },
    UserValueFaultText { .fault = UserValueFault::NotUrl,
                         .text = "is not an http, https or socks5 URL without credentials" },
    UserValueFaultText { .fault = UserValueFault::NotHostList,
                         .text = "is not a comma-separated list of host names, domains or addresses" },
};

static_assert(UserValueFaultTexts.size() == std::to_underlying(UserValueFault::NotHostList) + 1U,
              "UserValueFaultTexts needs one row per UserValueFault");

/// A named set of variables from the user's configuration (`daemon.extensions.<name>.env_sets.<set>`).
struct EnvironmentSet
{
    std::string name;                        ///< The set's name: an ArgumentKind::Name.
    std::vector<EnvironmentEntry> variables; ///< In the configuration's order.

    bool operator==(EnvironmentSet const&) const = default; ///< Compares name and variables, in order.
};

/// A variable dropped from a set, and why.
struct EnvironmentSetProblem
{
    std::string set;      ///< The set the variable was in.
    std::string variable; ///< The variable's name.
    UserValueFault fault; ///< Why its value is refused.
};

/// The sets as the host applies them, and what was dropped.
struct CheckedEnvironmentSets
{
    std::vector<EnvironmentSet> sets;            ///< Every set, in order, with its normalised variables.
    std::vector<EnvironmentSetProblem> problems; ///< One per variable dropped.
};

/// Finds the row of UserSettableVariables that @p name matches.
/// @param name The variable's name, compared exactly.
/// @return The row; nullptr where the name is not user-settable.
[[nodiscard]] UserSettableVariable const* userSettableRowOf(std::string_view name) noexcept;

/// Checks a user-set value against its row's shape and spells it the way the host applies it.
/// @param name The variable's name.
/// @param value The value as configured.
/// @param home The user's home directory, for `~` in an AbsolutePath value.
/// @param style The path grammar an AbsolutePath value is spelled in.
/// @return The value as the host applies it; or why it is refused.
[[nodiscard]] std::expected<std::string, UserValueFault> normalisedUserValue(std::string_view name,
                                                                             std::string_view value,
                                                                             std::string_view home,
                                                                             vtpty::PathStyle style);

/// Tells whether a value is already in the form normalisedUserValue() returns for it.
/// @param name The variable's name.
/// @param value The value to judge.
/// @param style The path grammar an AbsolutePath value is spelled in.
/// @return True when normalisedUserValue() accepts @p value unchanged.
[[nodiscard]] bool isNormalisedUserValue(std::string_view name,
                                         std::string_view value,
                                         vtpty::PathStyle style);

/// The text describing a fault, to follow the variable's name in a message.
/// @param fault The fault.
/// @return The fault's row of UserValueFaultTexts.
[[nodiscard]] std::string_view userValueFaultText(UserValueFault fault) noexcept;

/// Checks every variable of every set, keeping the sets and their order.
/// @param sets The sets as the configuration states them.
/// @param home The user's home directory.
/// @param style The path grammar AbsolutePath values are spelled in.
/// @return The sets with each variable normalised; the variables refused, one problem each. A set left empty
/// is kept.
[[nodiscard]] CheckedEnvironmentSets checkEnvironmentSets(std::span<EnvironmentSet const> sets,
                                                          std::string_view home,
                                                          vtpty::PathStyle style);

} // namespace vtextension
