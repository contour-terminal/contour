// SPDX-License-Identifier: Apache-2.0
//
// What automation starts: which Work-view items a rule's condition matches, what state an item is in for the
// rule's memory, and the planner that picks, from the items in Work-view order, the ones started now and the
// ones that wait for a free slot. Pure: the engine feeds it and carries out its plan.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/AutomationRules.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/WorkItems.hpp>

namespace orchestrator
{

/// @param item A Work-view item.
/// @param condition A condition.
/// @return Whether @p item meets @p condition. A draft MR meets none: it is the user's unfinished work.
[[nodiscard]] bool matchesCondition(WorkItem const& item, AutomationCondition condition) noexcept;

/// The state a rule's memory keeps of an item: a rule fires on it again only once this changed.
/// @param item A Work-view item.
/// @return An MR's `sha:<head commit>`, or `upd:<update time>` when the forge gave no head; a ticket's
///         `status:<status>`, or `cat:<status category as a number>` when it has no status name.
[[nodiscard]] std::string stateSignatureOf(WorkItem const& item);

/// Whether a rule may fire.
enum class RuleState : uint8_t
{
    Active = 0, ///< It fires.
    Paused,     ///< Paused after MaxRuleRefusals refusals in a row; the switch off and on resumes it.
};

/// What each rule last started on each item: by memoryKeyOf(), the item's stateSignatureOf() then.
using AutomationMemory = std::map<std::string, std::string, std::less<>>;

/// @param ruleKey A rule's ruleKeyOf().
/// @param itemKey An item's workItemKey().
/// @return `<ruleKey>|<itemKey>`.
[[nodiscard]] std::string memoryKeyOf(std::string_view ruleKey, std::string_view itemKey);

/// An item the planner chose, and what starts on it.
struct PlannedStart
{
    std::size_t item = 0;                 ///< Into PlanInput::items.
    std::optional<std::size_t> rule = {}; ///< Into PlanInput::rules; std::nullopt for a bulk start.
    std::string task = {};                ///< The task's id.
    [[nodiscard]] bool operator==(PlannedStart const&) const = default;
};

/// What the planner chose.
struct AutomationPlan
{
    std::vector<PlannedStart> start;  ///< Started now, in order.
    std::vector<PlannedStart> queued; ///< Waiting for a free slot, in order.
};

/// Everything the planner reads.
struct PlanInput
{
    std::span<WorkItem const> items;        ///< In Work-view order (orderOf() by Importance).
    std::span<AutomationRule const> rules;  ///< The rules; empty while the switch is paused.
    std::span<RuleState const> ruleStates;  ///< One per rule.
    AutomationMemory const& memory;         ///< What the rules last started.
    std::span<std::string const> busyItems; ///< The items whose start is in flight, by workItemKey().
    std::span<std::string const> bulkItems; ///< The items a bulk start still waits to start, in order.
    std::span<std::string const> bulkTasks; ///< One per bulk item: its task's id; empty: the suggested one.
    Config const& config;      ///< Its tasks: what a bulk item's task is, and whether it applies.
    std::size_t freeSlots = 0; ///< How many more automatic runs may start now.
};

/// Bulk items first, in their order; then each item in order, with the first rule (in configuration order)
/// that is Active, whose condition it meets and whose memory of it differs from its state. An item with a
/// run, or busy, is left out; so is a bulk item that is not listed, no task is suggested for, or whose task
/// does not apply to it. The first PlanInput::freeSlots go to AutomationPlan::start, the rest to queued.
/// @param input What the planner reads.
/// @return What starts now, and what waits.
[[nodiscard]] AutomationPlan planAutomation(PlanInput const& input);

} // namespace orchestrator
