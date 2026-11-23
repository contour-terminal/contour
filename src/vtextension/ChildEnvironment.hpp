// SPDX-License-Identifier: Apache-2.0
//
// The environment a declared command runs with: the daemon's own, less what could steer git or gh to
// another repository or reach Contour's control socket, then the manifest's declared values.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// How an inheritance rule's name is compared with a variable's.
enum class NameMatch : uint8_t
{
    Exact,  ///< The variable's name is the rule's.
    Prefix, ///< The variable's name starts with the rule's.
};

/// Whether a command inherits a variable of the daemon's environment.
enum class Inheritance : uint8_t
{
    Withheld, ///< The command does not see it.
    Kept,     ///< The command sees it as the daemon does.
};

/// One row of the inheritance table.
struct InheritanceRule
{
    std::string_view name;   ///< The name, or the prefix, the rule is for.
    NameMatch match;         ///< How @ref name is compared.
    Inheritance inheritance; ///< What becomes of a variable it matches.
};

/// What a command inherits of the daemon's environment. A variable no rule matches is kept; an Exact rule
/// outranks a Prefix rule, and of two Prefix rules the longer wins.
inline constexpr auto InheritanceRules = std::array {
    // Contour's control socket and pane identity: a command reaches no pane through them.
    InheritanceRule { .name = "CONTOUR_", .match = NameMatch::Prefix, .inheritance = Inheritance::Withheld },
    // git's repository, configuration and object selection (GIT_DIR, GIT_CONFIG_PARAMETERS, …).
    InheritanceRule { .name = "GIT_", .match = NameMatch::Prefix, .inheritance = Inheritance::Withheld },
    // How git authenticates, which the user configured for every git they run.
    InheritanceRule {
        .name = "GIT_SSH_COMMAND", .match = NameMatch::Exact, .inheritance = Inheritance::Kept },
    InheritanceRule { .name = "GIT_SSH", .match = NameMatch::Exact, .inheritance = Inheritance::Kept },
    InheritanceRule { .name = "GIT_ASKPASS", .match = NameMatch::Exact, .inheritance = Inheritance::Kept },
    // gh's repository and host selection.
    InheritanceRule { .name = "GH_REPO", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule { .name = "GH_HOST", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GH_ENTERPRISE_TOKEN", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    // glab's host, repository, group and remote selection, as https://docs.gitlab.com/cli/configuration/
    // lists it: a command, or an agent, would otherwise talk to another GitLab instance or project than the
    // repository's remote names. GITLAB_TOKEN is kept: it is how the user logs glab in.
    InheritanceRule {
        .name = "GITLAB_HOST", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule { .name = "GL_HOST", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule { .name = "GITLAB_URI", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GITLAB_API_HOST", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GITLAB_SSH_HOST", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GITLAB_REPO", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GITLAB_GROUP", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "GITLAB_HEAD_REPO", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    // GLAB_REMOTE_ALIAS, and the names glab checks after it but the GIT_ ones the prefix above withholds.
    InheritanceRule {
        .name = "GLAB_REMOTE_ALIAS", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "REMOTE_ALIAS", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
    InheritanceRule {
        .name = "REMOTE_NICKNAME", .match = NameMatch::Exact, .inheritance = Inheritance::Withheld },
};

/// How environment variable names compare.
enum class EnvironmentNameCase : uint8_t
{
    Sensitive = 0, ///< Byte for byte, as on POSIX.
    Insensitive,   ///< ASCII letters fold, as Windows does; a name with a non-ASCII byte is withheld, since
                   ///< Windows folds it by a table this host does not share (Decision 10).
};

/// The platform's own rule: Insensitive on Windows, Sensitive elsewhere.
#ifdef _WIN32
inline constexpr auto NativeEnvironmentNameCase = EnvironmentNameCase::Insensitive;
#else
inline constexpr auto NativeEnvironmentNameCase = EnvironmentNameCase::Sensitive;
#endif

/// Tells whether a command inherits a variable, by InheritanceRules.
/// @param name The variable's name.
/// @param rule How @p name is compared with the rules' names; NativeEnvironmentNameCase for the platform's.
/// @return What the most specific matching rule says; Kept when none matches. Withheld, whatever the rules
///         say, for a name with a non-ASCII byte under EnvironmentNameCase::Insensitive.
[[nodiscard]] Inheritance inheritanceOf(std::string_view name, EnvironmentNameCase rule) noexcept;

/// Builds a declared command's whole environment: the inherited variables InheritanceRules keeps, then the
/// manifest's `command-env`, then the command's own `command-env-for`. A later layer replaces a name in
/// place, so every name appears once; where names compare by @p rule as equal but are spelled differently,
/// the later spelling replaces the earlier one with its value.
/// @param inherited The daemon's environment, as `NAME=value` entries; an entry without `=` is skipped.
/// @param commandEnvironment The manifest's `command-env` values.
/// @param perCommand The command's `command-env-for` values.
/// @param rule How names compare; NativeEnvironmentNameCase for the platform's.
/// @return The environment, as name and value pairs.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> childEnvironment(
    std::span<std::string const> inherited,
    std::span<EnvironmentEntry const> commandEnvironment,
    std::span<EnvironmentEntry const> perCommand,
    EnvironmentNameCase rule);

} // namespace vtextension
