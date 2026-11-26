// SPDX-License-Identifier: Apache-2.0
//
// The engine's automation: the switch, the rules' memory, the planner's run at every step's end, and what an
// automatic dispatch's answer does to its rule.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Automation.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Sanitize.hpp>

namespace orchestrator::engine::detail
{

/// Automation's store entry. Glaze reflects only types with linkage, so it cannot sit in an anonymous
/// namespace.
struct JsonAutomationState
{
    std::string on;                                                ///< `on` or `paused`.
    std::map<std::string, std::string, std::less<>> attempts = {}; ///< AutomationMemory.
    std::vector<std::string> paused = {};                          ///< The paused rules' keys.
};

} // namespace orchestrator::engine::detail

/// The switch is stored as `switch`, a word C++ keeps for itself.
template <>
struct glz::meta<orchestrator::engine::detail::JsonAutomationState>
{
    using T = orchestrator::engine::detail::JsonAutomationState;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value =
        glz::object("switch", &T::on, "attempts", &T::attempts, "paused", &T::paused);
};

namespace orchestrator
{

namespace
{
    using engine::detail::JsonAutomationState;

    /// The input is a string_view: nothing terminates it.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false };

    /// How the store spells the switch.
    constexpr auto SwitchOn = std::string_view { "on" };
    constexpr auto SwitchPaused = std::string_view { "paused" };

    /// The key a group start's refusals are noted under: no rule has it, since a rule's has a `/`.
    constexpr auto GroupStartKey = std::string_view { "a group start" };

    /// What a rule is told when its worker's hooks are missing: nobody watches an automatic start.
    constexpr auto HooksMissingWhy = std::string_view { "its worker's hooks are missing" };

    /// @return Whether @p refusal says only that there is no room now: the item waits, the rule is not
    /// blamed.
    [[nodiscard]] constexpr bool isCapacity(DispatchRefusal refusal) noexcept
    {
        switch (refusal)
        {
            case DispatchRefusal::AgentLimit:
            case DispatchRefusal::RepoLimit:
            case DispatchRefusal::AlreadyRunning: return true;
            case DispatchRefusal::NotConfigured:
            case DispatchRefusal::UnknownRepo:
            case DispatchRefusal::UnknownTask:
            case DispatchRefusal::UnknownModel:
            case DispatchRefusal::BaseUnknown:
            case DispatchRefusal::BadPrompt:
            case DispatchRefusal::UnknownProfile:
            case DispatchRefusal::UnknownWorker:
            case DispatchRefusal::WorkerTakesNoProfile: return false;
        }
        return false;
    }

    /// @return The item key of a dispatch: workItemKey() of its repository and item.
    [[nodiscard]] std::string itemKeyOf(Dispatch const& request)
    {
        return workItemKey(WorkItem { .repo = request.repo, .item = request.item });
    }

    /// @return The part of a memory key after its rule: the item's key.
    [[nodiscard]] std::string_view itemOfMemoryKey(std::string_view key) noexcept
    {
        auto const bar = key.find('|');
        return bar == std::string_view::npos ? std::string_view {} : key.substr(bar + 1);
    }

    /// @return The web page of @p item: its MR's, else its ticket's.
    [[nodiscard]] std::string urlOf(WorkItem const& item)
    {
        if (item.changeRequest)
            return item.changeRequest->url;
        return item.ticket ? item.ticket->url : std::string {};
    }
} // namespace

bool Engine::hasAutomationRules() const noexcept
{
    return _config && !_config->automation().rules.empty();
}

bool Engine::isQueuedForAutomation(std::string_view itemKey) const noexcept
{
    return std::ranges::contains(_automationQueued, itemKey);
}

std::size_t Engine::activeAutomaticRuns() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(
        _runs, [](Run const& run) { return countsAsActive(run) && run.origin != RunOrigin::User; }));
}

void Engine::loadAutomation(std::string_view json)
{
    auto const read = json::readDocument<ReadOptions, JsonAutomationState>(json);
    if (!read)
    {
        _startProblems.emplace_back(
            "automation: its stored state cannot be read; the switch starts as configured");
        return;
    }
    _automation = read->on == SwitchOn ? AutomationSwitch::On : AutomationSwitch::Paused;
    _automationMemory = AutomationMemory { read->attempts.begin(), read->attempts.end() };
    _pausedRules = read->paused;
}

void Engine::storeAutomation(std::vector<Effect>& out) const
{
    auto const state =
        JsonAutomationState { .on = std::string { _automation == AutomationSwitch::On ? SwitchOn
                                                                                      : SwitchPaused },
                              .attempts = { _automationMemory.begin(), _automationMemory.end() },
                              .paused = _pausedRules };
    auto value = std::string {};
    if (glz::write_json(state, value))
        return;
    out.emplace_back(StoreSet { .key = std::string { AutomationKey }, .value = std::move(value) });
}

void Engine::setAutomation(AutomationSwitch to, std::vector<Effect>& out)
{
    if (to == _automation)
        return;
    _automation = to;
    // Switching on again gives every rule a fresh start: the paused ones resume, the counts start over.
    if (to == AutomationSwitch::On)
    {
        _pausedRules.clear();
        _ruleRecords.clear();
    }
    ++_revision;
    storeAutomation(out);
}

void Engine::runOnItems(RunOnItems const& request)
{
    for (auto const& item: request.items)
        if (!std::ranges::contains(_bulk, item, &BulkItem::item))
            _bulk.push_back(BulkItem { .item = item, .task = request.task });
    ++_revision;
}

void Engine::planAndStart(std::vector<Effect>& out)
{
    if (!_config)
        return;
    auto const& settings = _config->automation();
    auto const rules = _automation == AutomationSwitch::On
                           ? std::span<AutomationRule const> { settings.rules }
                           : std::span<AutomationRule const> {};
    auto const ruleStates = rules | std::views::transform([&](AutomationRule const& rule) {
                                return std::ranges::contains(_pausedRules, ruleKeyOf(rule))
                                           ? RuleState::Paused
                                           : RuleState::Active;
                            })
                            | std::ranges::to<std::vector>();
    auto const unordered = workItems();
    auto const items = orderOf(unordered, _repositoryNames, WorkSort::Importance)
                       | std::views::transform([&](std::size_t index) { return unordered[index]; })
                       | std::ranges::to<std::vector>();
    auto busy = _automaticPending;
    busy.insert(busy.end(), _refusedThisPoll.begin(), _refusedThisPoll.end());
    auto const bulkItems = _bulk | std::views::transform(&BulkItem::item) | std::ranges::to<std::vector>();
    auto const bulkTasks = _bulk | std::views::transform(&BulkItem::task) | std::ranges::to<std::vector>();

    // The slots left under automation's own cap, and under max_agents, which a user's run counts against too.
    auto const pending = _automaticPending.size();
    auto const room = [pending](std::size_t limit, std::size_t used) {
        return limit > used + pending ? limit - used - pending : std::size_t { 0 };
    };
    auto const freeSlots =
        std::min(room(automationCapOf(settings, _config->workers().size()), activeAutomaticRuns()),
                 room(_config->maxAgents(), activeRuns()));

    auto const plan = planAutomation(PlanInput { .items = items,
                                                 .rules = rules,
                                                 .ruleStates = ruleStates,
                                                 .memory = _automationMemory,
                                                 .busyItems = busy,
                                                 .bulkItems = bulkItems,
                                                 .bulkTasks = bulkTasks,
                                                 .config = *_config,
                                                 .freeSlots = freeSlots });
    auto queued =
        plan.queued
        | std::views::transform([&](PlannedStart const& start) { return workItemKey(items[start.item]); })
        | std::ranges::to<std::vector>();
    // A repository at max_agents_per_repo keeps its items waiting, as a full cap does.
    auto starts = std::vector<PlannedStart> {};
    auto const inRepo = [&](std::string_view repo) {
        auto const active = std::ranges::count_if(
            _runs, [&](Run const& run) { return countsAsActive(run) && run.repo == repo; });
        auto const started = std::ranges::count_if(
            starts, [&](PlannedStart const& start) { return items[start.item].repo == repo; });
        auto const waiting = std::ranges::count_if(_automaticPending, [&](std::string const& key) {
            return key.starts_with(std::format("{}/", repo));
        });
        return static_cast<std::size_t>(active + started + waiting);
    };
    for (auto const& start: plan.start)
        if (inRepo(items[start.item].repo) < _config->maxAgentsPerRepo())
            starts.push_back(start);
        else
            queued.push_back(workItemKey(items[start.item]));
    // A group start's item the planner neither starts nor queues is gone, taken or takes no task: dropped.
    std::erase_if(_bulk, [&](BulkItem const& bulk) {
        auto const planned = [&](PlannedStart const& start) {
            return workItemKey(items[start.item]) == bulk.item;
        };
        return std::ranges::none_of(plan.start, planned) && std::ranges::none_of(plan.queued, planned)
               && !std::ranges::contains(busy, bulk.item);
    });
    if (queued != _automationQueued)
    {
        _automationQueued = std::move(queued);
        ++_revision;
    }
    for (auto const& start: starts)
        startAutomatic(items[start.item], start, out);
}

void Engine::startAutomatic(WorkItem const& item, PlannedStart const& start, std::vector<Effect>& out)
{
    auto const& settings = _config->automation();
    auto const* const rule = start.rule ? &settings.rules[*start.rule] : nullptr;
    auto const itemKey = workItemKey(item);
    auto const ruleKey = rule != nullptr ? ruleKeyOf(*rule) : std::string { GroupStartKey };
    auto const workerId = rule != nullptr && !rule->worker.empty()
                              ? rule->worker
                              : defaultWorker(*_config, _runs, start.task).id;
    std::erase_if(_bulk, [&](BulkItem const& bulk) { return bulk.item == itemKey; });
    if (auto const* const worker = _config->findWorker(workerId);
        worker != nullptr && worker->agent == AgentKind::Claude
        && hooksFor(worker->envSet) == HooksState::Missing)
    {
        _refusedThisPoll.push_back(itemKey);
        refuseAutomatic(ruleKey, itemKey, HooksMissingWhy, out);
        return;
    }
    if (rule != nullptr)
    {
        _automationMemory[memoryKeyOf(ruleKey, itemKey)] = stateSignatureOf(item);
        storeAutomation(out);
    }
    _automaticPending.push_back(itemKey);
    out.emplace_back(
        StartAutomatic { Dispatch { .repo = item.repo,
                                    .item = item.item,
                                    .url = urlOf(item),
                                    .task = start.task,
                                    .model = rule != nullptr ? rule->model : std::string {},
                                    .permissions = rule != nullptr ? rule->permissions : std::string {},
                                    .worker = workerId,
                                    .note = std::nullopt,
                                    .session = {},
                                    .nearTab = std::nullopt,
                                    .wallMs = 0,
                                    .monoMs = 0,
                                    .origin = rule != nullptr ? RunOrigin::Rule : RunOrigin::Bulk,
                                    .originRule = rule != nullptr ? ruleKey : std::string {} } });
}

void Engine::onAutomaticDispatch(Dispatch const& request,
                                 std::optional<DispatchRefusal> refusal,
                                 std::vector<Effect>& out)
{
    auto const itemKey = itemKeyOf(request);
    std::erase(_automaticPending, itemKey);
    auto const ruleKey =
        request.origin == RunOrigin::Rule ? request.originRule : std::string { GroupStartKey };
    if (!refusal)
    {
        _ruleRecords.erase(ruleKey);
        return;
    }
    // Not started: tried again once the next poll ended, with the rule's memory of the item undone.
    _refusedThisPoll.push_back(itemKey);
    if (request.origin == RunOrigin::Rule && _automationMemory.erase(memoryKeyOf(ruleKey, itemKey)) > 0)
        storeAutomation(out);
    if (!isCapacity(*refusal))
        refuseAutomatic(ruleKey, itemKey, dispatchRefusalText(*refusal), out);
}

void Engine::refuseAutomatic(std::string_view ruleKey,
                             std::string_view itemKey,
                             std::string_view why,
                             std::vector<Effect>& out)
{
    auto& record = _ruleRecords[std::string { ruleKey }];
    record.lastWhy = displayText(
        std::format("automation: {} could not start on {}: {}", ruleKey, itemKey, why), MaxSummaryLineChars);
    ++_revision;
    if (ruleKey == GroupStartKey || ++record.refusals < MaxRuleRefusals)
        return;
    if (!std::ranges::contains(_pausedRules, ruleKey))
        _pausedRules.emplace_back(ruleKey);
    storeAutomation(out);
}

void Engine::settleAutomationAfterPoll(std::vector<Effect>& out)
{
    _refusedThisPoll.clear();
    auto const listed = workItems() | std::views::transform(workItemKey) | std::ranges::to<std::vector>();
    auto const forgotten = std::erase_if(_automationMemory, [&](auto const& entry) {
        return !std::ranges::contains(listed, itemOfMemoryKey(entry.first));
    });
    if (forgotten > 0)
        storeAutomation(out);
}

std::vector<std::string> Engine::automationProblems() const
{
    auto lines = std::vector<std::string> {};
    for (auto const& [ruleKey, record]: _ruleRecords)
        if (!record.lastWhy.empty() && !std::ranges::contains(_pausedRules, ruleKey))
            lines.push_back(record.lastWhy);
    for (auto const& ruleKey: _pausedRules)
        lines.push_back(
            displayText(std::format("automation: {} is paused after {} refusals; switch automation "
                                    "off and on to resume it",
                                    ruleKey,
                                    MaxRuleRefusals),
                        MaxSummaryLineChars));
    return lines;
}

} // namespace orchestrator
