// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Automation.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orchestrator
{

bool matchesCondition(WorkItem const& item, AutomationCondition condition) noexcept
{
    if (condition == AutomationCondition::NoMrYet)
        return !item.changeRequest && item.group == WorkGroup::NoMrYet;
    if (!item.changeRequest || item.changeRequest->readiness == Readiness::Draft)
        return false;
    auto const& mr = *item.changeRequest;
    switch (condition)
    {
        case AutomationCondition::PipelineFailed: return mr.ci == CiState::Failing;
        case AutomationCondition::Conflicts: return mr.conflicts == Conflicts::Conflicting;
        case AutomationCondition::ChangesRequested: return mr.review == ReviewState::ChangesRequested;
        case AutomationCondition::ReadyToMerge: return groupOf(mr) == WorkGroup::ReadyToMerge;
        case AutomationCondition::NoMrYet: break;
    }
    return false;
}

std::string stateSignatureOf(WorkItem const& item)
{
    if (item.changeRequest)
    {
        auto const& mr = *item.changeRequest;
        return mr.headSha.empty() ? std::format("upd:{}", mr.updatedWallMs)
                                  : std::format("sha:{}", mr.headSha);
    }
    if (!item.ticket)
        return {};
    auto const& ticket = *item.ticket;
    return ticket.status.empty() ? std::format("cat:{}", std::to_underlying(ticket.category))
                                 : std::format("status:{}", ticket.status);
}

std::string memoryKeyOf(std::string_view ruleKey, std::string_view itemKey)
{
    return std::format("{}|{}", ruleKey, itemKey);
}

namespace
{
    /// @return The rule that starts on @p item, and its task; std::nullopt: none.
    [[nodiscard]] std::optional<PlannedStart> ruleStartOf(PlanInput const& input,
                                                          std::size_t index,
                                                          std::string_view itemKey)
    {
        auto const& item = input.items[index];
        for (auto const [ruleIndex, rule]: std::views::enumerate(input.rules))
        {
            auto const position = static_cast<std::size_t>(ruleIndex);
            if (position < input.ruleStates.size() && input.ruleStates[position] == RuleState::Paused)
                continue;
            if (!matchesCondition(item, rule.when))
                continue;
            auto const key = memoryKeyOf(ruleKeyOf(rule), itemKey);
            auto const remembered = input.memory.find(key);
            if (remembered != input.memory.end() && remembered->second == stateSignatureOf(item))
                continue;
            if (auto const tried = input.attempts.find(key);
                tried != input.attempts.end() && tried->second >= MaxAutomaticAttempts)
                continue;
            return PlannedStart { .item = index, .rule = position, .task = rule.task };
        }
        return std::nullopt;
    }

    /// @return The task a bulk start runs on @p item: @p chosen while it applies, else, @p chosen empty, the
    ///         suggested one; std::nullopt: none.
    [[nodiscard]] std::optional<std::string> bulkTaskOf(Config const& config,
                                                        WorkItem const& item,
                                                        std::string_view chosen)
    {
        if (chosen.empty())
        {
            auto const suggestion = suggestTask(config, item);
            return suggestion.task != nullptr ? std::optional { suggestion.task->id } : std::nullopt;
        }
        auto const* const task = config.findTask(chosen);
        if (task == nullptr || task->appliesTo != appliesToOf(item.item))
            return std::nullopt;
        return task->id;
    }
} // namespace

AutomationPlan planAutomation(PlanInput const& input)
{
    auto const keys = input.items | std::views::transform(workItemKey) | std::ranges::to<std::vector>();
    auto const isFree = [&](std::size_t index) {
        return input.items[index].agentRun.empty() && !std::ranges::contains(input.busyItems, keys[index]);
    };
    auto chosen = std::vector<PlannedStart> {};
    auto const isChosen = [&](std::size_t index) {
        return std::ranges::contains(chosen, index, &PlannedStart::item);
    };
    for (auto const [bulkIndex, bulkItem]: std::views::enumerate(input.bulkItems))
    {
        auto const found = std::ranges::find(keys, bulkItem);
        if (found == keys.end())
            continue;
        auto const index = static_cast<std::size_t>(found - keys.begin());
        if (!isFree(index) || isChosen(index))
            continue;
        auto const position = static_cast<std::size_t>(bulkIndex);
        auto const chosenTask = position < input.bulkTasks.size()
                                    ? std::string_view { input.bulkTasks[position] }
                                    : std::string_view {};
        if (auto task = bulkTaskOf(input.config, input.items[index], chosenTask))
            chosen.push_back(PlannedStart { .item = index, .rule = std::nullopt, .task = std::move(*task) });
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, input.items.size()))
        if (isFree(index) && !isChosen(index))
            if (auto start = ruleStartOf(input, index, keys[index]))
                chosen.push_back(std::move(*start));

    auto plan = AutomationPlan {};
    for (auto& start: chosen)
        (plan.start.size() < input.freeSlots ? plan.start : plan.queued).push_back(std::move(start));
    return plan;
}

} // namespace orchestrator
