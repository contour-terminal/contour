// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/ArgumentKinds.hpp>

namespace orchestrator
{

/// One variable of a fixed environment.
struct EnvironmentVariable
{
    std::string_view name;  ///< The variable's name.
    std::string_view value; ///< Its value.
};

/// A typed argument: what a `{name}` placeholder may be replaced with.
struct ArgumentDecl
{
    std::string_view name;    ///< The placeholder's name.
    ArgumentKind kind;        ///< What its value may hold.
    std::optional<Root> root; ///< The root a Path must lie in; empty for every other kind.
};

/// A command the extension may ask the host to run: executed directly, never through a shell.
struct CommandDecl
{
    std::string_view name;                  ///< The name the guest starts it by.
    Timeout timeout;                        ///< How long it may run.
    Root root;                              ///< The root its working directory must lie in.
    std::span<std::string_view const> argv; ///< Its words; a whole-word `{name}` is an argument.
    /// Added for this command only, after commandEnvironment(): a variable both name takes this value, so
    /// a GIT_CONFIG_COUNT here replaces the global one and must come with every pair it counts.
    std::span<EnvironmentVariable const> environment;
};

/// A program the extension may put in a tab.
struct PaneDecl
{
    std::string_view name;                            ///< The name the guest opens it by.
    Root root;                                        ///< The root its working directory must lie in.
    std::span<std::string_view const> argv;           ///< Its words; a whole-word `{name}` is an argument.
    std::span<EnvironmentVariable const> environment; ///< Its fixed environment.
};

/// A value a call passes for a placeholder.
struct Argument
{
    std::string_view name; ///< The placeholder's name.
    std::string value;     ///< Its value, checked against the placeholder's kind before it is used.
};

/// Why substitute() refused an argv.
enum class SubstituteError : uint8_t
{
    UnknownPlaceholder, ///< A word names no declared argument, or holds a brace without being `{name}`.
    MissingArgument,    ///< A placeholder the call passed no value for.
    Rejected,           ///< A value its argument's kind does not accept.
};

/// The name a whole-word placeholder stands for.
/// @param word One word of an argv.
/// @return The name between the braces if @p word is exactly `{name}` with a non-empty name holding no
///         brace; std::nullopt for every other word.
[[nodiscard]] constexpr std::optional<std::string_view> placeholderName(std::string_view word) noexcept
{
    if (word.size() < 3 || !word.starts_with('{') || !word.ends_with('}'))
        return std::nullopt;
    auto const name = word.substr(1, word.size() - 2);
    if (name.contains('{') || name.contains('}'))
        return std::nullopt;
    return name;
}

/// Every typed argument, in declaration order.
[[nodiscard]] std::span<ArgumentDecl const> argumentDecls() noexcept;

/// The environment added to every declared command. It keeps git from prompting, limits the transports a
/// remote may use, and switches off the two configuration keys that would make a plain `git status` run a
/// program the repository names.
[[nodiscard]] std::span<EnvironmentVariable const> commandEnvironment() noexcept;

/// Every command the extension may run, in declaration order.
[[nodiscard]] std::span<CommandDecl const> commands() noexcept;

/// Every program the extension may put in a tab, in declaration order.
[[nodiscard]] std::span<PaneDecl const> paneTemplates() noexcept;

/// @param name A typed argument's name.
/// @return Its declaration, or nullptr if no argument has that name.
[[nodiscard]] ArgumentDecl const* findArgumentDecl(std::string_view name) noexcept;

/// @param name A command's name.
/// @return Its declaration, or nullptr if no command has that name.
[[nodiscard]] CommandDecl const* findCommand(std::string_view name) noexcept;

/// The pane template an agent runs in.
inline constexpr std::string_view AgentPaneTemplate = "agent";

/// @param name A pane template's name.
/// @return Its declaration, or nullptr if no pane template has that name.
[[nodiscard]] PaneDecl const* findPaneTemplate(std::string_view name) noexcept;

/// Fills in an argv's placeholders.
/// @param argv A declared argv.
/// @param arguments The values for its placeholders, by name.
/// @return @p argv with each `{name}` replaced by its checked value, one word staying one word whatever the
///         value holds; or why it was refused.
[[nodiscard]] std::expected<std::vector<std::string>, SubstituteError> substitute(
    std::span<std::string_view const> argv, std::span<Argument const> arguments);

/// The manifest lines the tables declare: every root, read, arg, command-env, command, command-env-for,
/// pane, pane-env, inbox, serve and action line, in that order and each kind in table order. The manifest
/// is these lines after its fixed `name`, `version` and `placement` lines.
/// @return One string per line, without line ends.
[[nodiscard]] std::vector<std::string> manifestLines();

} // namespace orchestrator
