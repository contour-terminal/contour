// SPDX-License-Identifier: Apache-2.0
//
// The vocabulary of the configuration's `automation`: the conditions a rule may name, a rule, and the
// settings that hold them. What a condition matches is decided in Automation.hpp, beside the work items.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/AppliesTo.hpp>

namespace orchestrator
{

/// What a rule waits for on a Work-view item.
enum class AutomationCondition : uint8_t
{
    PipelineFailed = 0, ///< An MR whose checks failed.
    Conflicts,          ///< An MR that conflicts with its base.
    ChangesRequested,   ///< An MR a reviewer asked changes of.
    ReadyToMerge,       ///< An MR in the Ready to merge group.
    NoMrYet,            ///< A ticket or forge issue in progress that no MR names.
};

/// One condition, its spelling in `automation.rules[].when`, and the kind of item it matches.
struct AutomationConditionRow
{
    AutomationCondition condition; ///< The condition.
    std::string_view name;         ///< How `when` spells it.
    AppliesTo appliesTo;           ///< The items it matches; a rule's task must apply to them.
};

/// Every condition, one row each, in the order a problem line lists them.
inline constexpr auto AutomationConditionRows = std::array {
    AutomationConditionRow { .condition = AutomationCondition::PipelineFailed,
                             .name = "pipeline_failed",
                             .appliesTo = AppliesTo::PullRequest },
    AutomationConditionRow { .condition = AutomationCondition::Conflicts,
                             .name = "conflicts",
                             .appliesTo = AppliesTo::PullRequest },
    AutomationConditionRow { .condition = AutomationCondition::ChangesRequested,
                             .name = "changes_requested",
                             .appliesTo = AppliesTo::PullRequest },
    AutomationConditionRow { .condition = AutomationCondition::ReadyToMerge,
                             .name = "ready_to_merge",
                             .appliesTo = AppliesTo::PullRequest },
    AutomationConditionRow {
        .condition = AutomationCondition::NoMrYet, .name = "no_mr_yet", .appliesTo = AppliesTo::Issue },
};

/// @param name A spelling, such as `pipeline_failed`.
/// @return The condition it spells; std::nullopt for one no row spells.
[[nodiscard]] constexpr std::optional<AutomationCondition> automationConditionNamed(
    std::string_view name) noexcept
{
    auto const row = std::ranges::find(AutomationConditionRows, name, &AutomationConditionRow::name);
    return row != AutomationConditionRows.end() ? std::optional { row->condition } : std::nullopt;
}

/// @param condition A condition.
/// @return Its row.
[[nodiscard]] constexpr AutomationConditionRow const& automationConditionRow(
    AutomationCondition condition) noexcept
{
    return *std::ranges::find(AutomationConditionRows, condition, &AutomationConditionRow::condition);
}

/// @param condition A condition.
/// @return How `when` spells it.
[[nodiscard]] constexpr std::string_view automationConditionName(AutomationCondition condition) noexcept
{
    return automationConditionRow(condition).name;
}

/// Whether the rules may start agents.
enum class AutomationSwitch : uint8_t
{
    Paused = 0, ///< No rule fires.
    On,         ///< The rules fire.
};

/// One `automation.rules` entry: a condition and the task started on every item that meets it.
struct AutomationRule
{
    AutomationCondition when = AutomationCondition::PipelineFailed; ///< The condition.
    std::string task;             ///< The task's id; it applies to the condition's items.
    std::string worker = {};      ///< The worker; empty: as the start dialog picks it for the task.
    std::string model = {};       ///< The model; empty: the task's.
    std::string permissions = {}; ///< The permission profile; empty: the task's, else the default.
    [[nodiscard]] bool operator==(AutomationRule const&) const = default;
};

/// @param rule A rule.
/// @return `<when>/<task>`: its identity in the stored memory and the paused list, so that reordering the
///         rules keeps both.
[[nodiscard]] inline std::string ruleKeyOf(AutomationRule const& rule)
{
    return std::format("{}/{}", automationConditionName(rule.when), rule.task);
}

/// The configuration's `automation`.
struct AutomationSettings
{
    AutomationSwitch enabled = AutomationSwitch::Paused; ///< `enabled`: the switch while nothing is stored.
    std::optional<uint32_t> maxAgents = std::nullopt;    ///< `max_agents`; std::nullopt: automationCapOf()'s.
    std::vector<AutomationRule> rules = {};              ///< `rules`, in the configured order.
    [[nodiscard]] bool operator==(AutomationSettings const&) const = default;
};

/// @param settings The settings.
/// @param workers How many workers are configured.
/// @return The most automatic runs active at once: `max_agents`, else one less than the workers, at least 1.
[[nodiscard]] constexpr uint32_t automationCapOf(AutomationSettings const& settings,
                                                 std::size_t workers) noexcept
{
    if (settings.maxAgents)
        return *settings.maxAgents;
    return workers > 1 ? static_cast<uint32_t>(workers - 1) : 1U;
}

} // namespace orchestrator
