// SPDX-License-Identifier: Apache-2.0
//
// What the agents the orchestrator starts may do without asking: permission profiles, passed to Claude Code's
// `--settings`, so that routine work does not wait for a person who may be on another machine.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// Claude Code's permission mode for a session (its settings' `permissions.defaultMode`).
enum class PermissionMode : uint8_t
{
    Default = 0, ///< Everything that is not read-only asks.
    AcceptEdits, ///< Edits inside the working directory need no approval.
    Plan,        ///< The agent plans and changes nothing.
};

/// How the configuration and Claude Code spell a PermissionMode.
struct PermissionModeName
{
    std::string_view word; ///< As written, such as `acceptEdits`.
    PermissionMode mode;   ///< What it means.
};

/// Every mode a profile may name. A new mode is a new row.
inline constexpr auto PermissionModeNames = std::array {
    PermissionModeName { .word = "default", .mode = PermissionMode::Default },
    PermissionModeName { .word = "acceptEdits", .mode = PermissionMode::AcceptEdits },
    PermissionModeName { .word = "plan", .mode = PermissionMode::Plan },
};

/// The mode no profile may name: it would let an agent do anything without asking.
inline constexpr std::string_view RefusedPermissionMode = "bypassPermissions";

/// The profile a task without its own runs with, unless `permissions` names another.
inline constexpr std::string_view DefaultProfileName = "standard";

/// The most rules one list (allow or deny) of one profile holds, as configured.
inline constexpr std::size_t MaxPermissionRules = 64;

/// @param word A mode as written.
/// @return The mode, or nothing if no row spells it (RefusedPermissionMode included).
[[nodiscard]] std::optional<PermissionMode> permissionModeOf(std::string_view word) noexcept;

/// @param mode A mode.
/// @return How Claude Code spells it.
[[nodiscard]] std::string_view nameOf(PermissionMode mode) noexcept;

/// A permission profile: a built-in one, one from `permission_profiles`, or one of those resolved.
struct PermissionProfile
{
    std::string name;                        ///< Unique among the profiles; a Name.
    std::optional<std::string> extends = {}; ///< The profile it builds on; none once resolved.
    std::optional<PermissionMode> mode = {}; ///< Its own mode; none: its parent's. Always set once resolved.
    std::vector<std::string> allow = {};     ///< Rules allowed without asking, such as `Bash(ctest:*)`.
    std::vector<std::string> deny = {};      ///< Rules refused outright; deny wins over allow.
    [[nodiscard]] bool operator==(PermissionProfile const&) const = default;
};

/// @return The profiles every configuration has, unresolved: `standard`, `trusted` (standard, plus publishing
///         its own branch) and `ask` (Claude Code's own prompts).
[[nodiscard]] std::vector<PermissionProfile> builtinProfiles();

/// @param rule A rule as a profile lists it.
/// @return Whether it is `Tool` or `Tool(specifier)`: a tool name of `[A-Za-z][A-Za-z0-9_-]*`, and a
/// non-empty
///         specifier holding neither a parenthesis nor a control character.
[[nodiscard]] bool isPermissionRule(std::string_view rule) noexcept;

/// Why the profiles could not be resolved.
enum class ProfileError : uint8_t
{
    UnknownProfile, ///< A profile extends one that does not exist.
    ProfileCycle,   ///< A profile extends itself, through any number of others.
};

/// The profile resolution stopped at, and why.
struct ProfileProblem
{
    std::string profile; ///< The profile whose `extends` is at fault.
    ProfileError error;  ///< What is wrong with it.
    [[nodiscard]] bool operator==(ProfileProblem const&) const = default;
};

/// Resolves the profiles: the built-in ones, each replaced by a configured profile of its name, then the
/// other configured ones in order. A profile takes its parent's mode unless it has one (`default` at the
/// root), and its parent's rules followed by its own, without duplicates.
/// @param configured The profiles from `permission_profiles`, names unique.
/// @return Every profile, resolved; or the first profile whose `extends` is unknown or circular.
[[nodiscard]] std::expected<std::vector<PermissionProfile>, ProfileProblem> resolveProfiles(
    std::span<PermissionProfile const> configured);

/// @param resolved A resolved profile.
/// @return The settings Claude Code's `--settings` takes for it:
///         `{"permissions":{"defaultMode":…,"allow":[…],"deny":[…]}}`.
[[nodiscard]] std::string settingsJsonOf(PermissionProfile const& resolved);

/// What a run's stored settings let its agent do, as its details show it.
struct SettingsSummary
{
    PermissionMode mode = PermissionMode::Default; ///< Its mode.
    std::size_t allowed = 0;                       ///< How many rules it allows.
    std::size_t denied = 0;                        ///< How many rules it denies.
    [[nodiscard]] bool operator==(SettingsSummary const&) const = default;
};

/// @param settingsJson What settingsJsonOf() wrote for a run.
/// @return Its mode and counts; nothing when it is not such a document.
[[nodiscard]] std::optional<SettingsSummary> summaryOfSettings(std::string_view settingsJson);

} // namespace orchestrator
