// SPDX-License-Identifier: Apache-2.0
//
// What the agents the orchestrator starts may do without asking: permission profiles, passed to Claude Code's
// `--settings`, so that routine work does not wait for a person who may be on another machine.
#pragma once

#include <array>
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

/// One PermissionMode: how the configuration and Claude Code spell it, and what it leaves to the agent.
struct PermissionModeName
{
    std::string_view word;   ///< As written, such as `acceptEdits`.
    PermissionMode mode;     ///< What it means.
    std::string_view effect; ///< What the dispatch form and a run's details say it leaves to the agent.
};

/// Every mode a profile may name. A new mode is a new row.
inline constexpr auto PermissionModeNames = std::array {
    PermissionModeName {
        .word = "default", .mode = PermissionMode::Default, .effect = "Claude Code asks for the rest" },
    PermissionModeName {
        .word = "acceptEdits", .mode = PermissionMode::AcceptEdits, .effect = "edits need no approval" },
    PermissionModeName {
        .word = "plan", .mode = PermissionMode::Plan, .effect = "it plans and changes nothing" },
};

/// The mode no profile may name: it would let an agent do anything without asking.
inline constexpr std::string_view RefusedPermissionMode = "bypassPermissions";

/// The profile a task without its own runs with, unless `permissions` names another.
inline constexpr std::string_view DefaultProfileName = "standard";

/// The settings of a run stored before profiles existed: none of the orchestrator's, Claude Code's own alone.
inline constexpr std::string_view EmptySettingsJson = "{}";

/// @param word A mode as written.
/// @return The mode, or nothing if no row spells it (RefusedPermissionMode included).
[[nodiscard]] std::optional<PermissionMode> permissionModeOf(std::string_view word) noexcept;

/// A built-in profile: one row of builtinProfiles().
struct BuiltinProfile
{
    std::string_view name;                   ///< Its name.
    std::string_view extends;                ///< The profile it builds on; empty: none.
    std::optional<PermissionMode> mode;      ///< Its own mode; none: its parent's.
    std::span<std::string_view const> allow; ///< The rules it allows besides its parent's.
    std::span<std::string_view const> deny;  ///< The rules it denies besides its parent's.
};

/// @return The profiles every configuration has: `standard`, `trusted` (standard, plus publishing its own
///         branch) and `ask` (Claude Code's own prompts). A new built-in profile is a new row.
[[nodiscard]] std::span<BuiltinProfile const> builtinProfiles() noexcept;

/// A profile as `permission_profiles` lists it, before resolution.
struct ProfileDef
{
    std::string name;                        ///< Unique among the profiles; a Name.
    std::optional<std::string> extends = {}; ///< The profile it builds on.
    std::optional<PermissionMode> mode = {}; ///< Its own mode; none: its parent's, else `default`.
    std::vector<std::string> allow = {};     ///< Rules allowed without asking, such as `Bash(ctest:*)`.
    std::vector<std::string> deny = {};      ///< Rules refused outright; deny wins over allow.
    [[nodiscard]] bool operator==(ProfileDef const&) const = default;
};

/// A resolved profile: what an agent starts with.
struct PermissionProfile
{
    std::string name;                              ///< Its name.
    PermissionMode mode = PermissionMode::Default; ///< Its mode.
    std::vector<std::string> allow = {};           ///< Its parent's allowed rules, then its own.
    std::vector<std::string> deny = {};            ///< Its parent's denied rules, then its own.
    [[nodiscard]] bool operator==(PermissionProfile const&) const = default;
};

/// @param profiles Resolved profiles.
/// @param name A profile name.
/// @return The profile of that name, or nullptr.
[[nodiscard]] PermissionProfile const* findProfile(std::span<PermissionProfile const> profiles,
                                                   std::string_view name) noexcept;

/// @param rule A rule as a profile lists it.
/// @return Whether it is `Tool` or `Tool(specifier)`: a tool name of `[A-Za-z][A-Za-z0-9_-]*`, and a
///         non-empty specifier holding neither a parenthesis nor a control character.
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
/// other configured ones in order.
/// @param configured The profiles from `permission_profiles`, names unique.
/// @return Every profile, resolved; or the first profile whose `extends` is unknown or circular.
[[nodiscard]] std::expected<std::vector<PermissionProfile>, ProfileProblem> resolveProfiles(
    std::span<ProfileDef const> configured);

/// @param profile A resolved profile.
/// @return The settings Claude Code's `--settings` takes for it:
///         `{"permissions":{"defaultMode":…,"allow":[…],"deny":[…]}}`.
[[nodiscard]] std::string settingsJsonOf(PermissionProfile const& profile);

/// What the dispatch form and a run's details say about the profile a run starts with.
/// @param profile The profile's name.
/// @param settingsJson What settingsJsonOf() wrote for it.
/// @return Such as `standard · 19 allowed · 4 denied · edits need no approval`, or `ask · Claude Code asks
///         for the rest` for a profile with no rules; the name alone when @p settingsJson cannot be read;
///         empty for a run stored before profiles existed (an empty @p profile).
[[nodiscard]] std::string permissionsText(std::string_view profile, std::string_view settingsJson);

} // namespace orchestrator
