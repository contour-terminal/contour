// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/DispatchForm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Agents.hpp>
#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    // {{{ what the form says

    /// What each refusal tells the user, indexed by DispatchRefusal.
    constexpr auto RefusalTexts = std::array {
        "the orchestrator is not configured: the line above says why"sv,            // NotConfigured
        "max_agents agents are running: end one first"sv,                           // AgentLimit
        "max_agents_per_repo agents run in this repository: end one first"sv,       // RepoLimit
        "the repository is no longer configured"sv,                                 // UnknownRepo
        "the task does not apply to this item"sv,                                   // UnknownTask
        "the model is not one of models"sv,                                         // UnknownModel
        "an agent already works on this item"sv,                                    // AlreadyRunning
        "the base branch is not known yet: it is read with the next refresh (R)"sv, // BaseUnknown
        "the prompt or the note cannot be passed to the agent"sv,                   // BadPrompt
        "the permission profile is no longer configured"sv,                         // UnknownProfile
        "no worker has that id: check `workers`"sv,                                 // UnknownWorker
        "that worker takes no permission profile, and this task has one: pick another worker"sv, // WorkerTakesNoProfile
    };
    static_assert(RefusalTexts.size() == std::to_underlying(DispatchRefusal::WorkerTakesNoProfile) + 1U,
                  "RefusalTexts has one row per DispatchRefusal");

    /// The arguments the command line shows by their placeholder, being long: lines of their own say what
    /// they hold.
    constexpr auto ShownByPlaceholder = std::array { "board"sv, "settings"sv };

    /// @return The index into Config::profiles() of the profile @p taskId's runs start with.
    [[nodiscard]] std::size_t profileIndexFor(Config const& config, std::string_view taskId) noexcept
    {
        auto const profiles = config.profiles();
        auto const& profile = config.profileFor(taskId);
        return static_cast<std::size_t>(&profile - profiles.data());
    }

    /// @return The index into Config::workers() of the worker @p taskId's runs start on now
    /// (defaultWorker()).
    [[nodiscard]] std::size_t workerIndexFor(Engine const& engine,
                                             Config const& config,
                                             std::string_view taskId)
    {
        auto const& worker = defaultWorker(config, engine.runs(), taskId);
        return static_cast<std::size_t>(&worker - config.workers().data());
    }

    // }}}
    // {{{ the settings

    /// @return The index after (or before, by @p step) @p from among @p count, wrapping around, whose entry
    ///         @p accepts; @p from when no other does.
    template <typename Accepts>
    [[nodiscard]] std::size_t cycled(std::size_t from, std::size_t count, int step, Accepts accepts)
    {
        if (count == 0)
            return from;
        auto index = from;
        for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 0 }, count))
        {
            index = step > 0 ? (index + 1) % count : (index + count - 1) % count;
            if (accepts(index))
                return index;
        }
        return from;
    }

    // }}}
} // namespace

std::string_view refusalText(DispatchRefusal refusal) noexcept
{
    return RefusalTexts[std::to_underlying(refusal)];
}

std::optional<DispatchFormState> openItemForm(Engine const& engine,
                                              std::string_view repo,
                                              ItemRef const& item,
                                              std::string url,
                                              std::string title,
                                              std::string_view task,
                                              std::string session)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return std::nullopt;
    auto const tasks = config->tasks();
    auto const found = std::ranges::find(tasks, task, &TaskDef::id);
    if (found == tasks.end() || found->appliesTo != appliesToOf(item))
        return std::nullopt;
    auto const models = config->models();
    auto const model = std::ranges::find(models, found->model);
    return DispatchFormState {
        .repo = std::string { repo },
        .item = item,
        .url = std::move(url),
        .title = std::move(title),
        .task = static_cast<std::size_t>(std::ranges::distance(tasks.begin(), found)),
        .model = model != models.end()
                     ? static_cast<std::size_t>(std::ranges::distance(models.begin(), model))
                     : 0,
        .permissions = profileIndexFor(*config, found->id),
        .worker = workerIndexFor(engine, *config, found->id),
        .note = {},
        .topic = {},
        .session = std::move(session),
    };
}

std::optional<DispatchFormState> openTopicForm(Engine const& engine,
                                               std::string_view repo,
                                               std::string topic,
                                               std::string session)
{
    auto const* const config = engine.config();
    if (config == nullptr || config->repos().empty())
        return std::nullopt;
    auto const* const known = config->findRepo(repo);
    auto const* const task = config->firstTaskFor(AppliesTo::Topic);
    auto const tasks = config->tasks();
    auto const models = config->models();
    auto const model = task != nullptr ? std::ranges::find(models, task->model) : models.end();
    return DispatchFormState {
        .repo = known != nullptr ? known->name : config->repos().front().name,
        .item = TopicRef {},
        .url = {},
        .title = {},
        .task = task != nullptr ? static_cast<std::size_t>(task - tasks.data()) : 0,
        .model = model != models.end()
                     ? static_cast<std::size_t>(std::ranges::distance(models.begin(), model))
                     : 0,
        .permissions = task != nullptr ? profileIndexFor(*config, task->id) : 0,
        .worker = workerIndexFor(engine, *config, task != nullptr ? std::string_view { task->id } : ""sv),
        .note = {},
        .topic = std::move(topic),
        .session = std::move(session),
    };
}

DispatchAction actionOf(DispatchFormState const& form, Engine const& engine, std::string_view nearTab)
{
    auto const* const config = engine.config();
    auto const tasks = config != nullptr ? config->tasks() : std::span<TaskDef const> {};
    auto const models = config != nullptr ? config->models() : std::span<std::string const> {};
    return DispatchAction {
        .repo = form.repo,
        .item = form.item,
        .url = form.url,
        .topic = form.topic,
        .task = form.task < tasks.size() ? tasks[form.task].id : std::string {},
        .model = form.model < models.size() ? models[form.model] : std::string {},
        .permissions = config != nullptr && form.permissions < config->profiles().size()
                           ? config->profiles()[form.permissions].name
                           : std::string {},
        .worker = config != nullptr && form.worker < config->workers().size()
                      ? config->workers()[form.worker].id
                      : std::string {},
        .note = form.note.empty() ? std::nullopt : std::optional { form.note },
        .session = form.session,
        .nearTab = std::string { nearTab },
    };
}

std::expected<DispatchPreview, DispatchRefusal> previewOf(DispatchFormState const& form, Engine const& engine)
{
    auto action = actionOf(form, engine, {});
    // The wall time only stamps the names, which the form shows without their stamp.
    return engine.preview(Dispatch { .repo = std::move(action.repo),
                                     .item = std::move(action.item),
                                     .url = std::move(action.url),
                                     .topic = std::move(action.topic),
                                     .task = std::move(action.task),
                                     .model = std::move(action.model),
                                     .permissions = std::move(action.permissions),
                                     .worker = std::move(action.worker),
                                     .note = std::move(action.note),
                                     .session = std::move(action.session),
                                     .nearTab = std::nullopt,
                                     .wallMs = 0,
                                     .monoMs = 0 });
}

bool canConfirm(DispatchFormState const& form, Engine const& engine)
{
    // A note or a topic starting with `-` would be read as an option.
    return !form.note.starts_with('-') && !form.topic.starts_with('-') && previewOf(form, engine).has_value();
}

void changeSetting(DispatchFormState& form, FormField field, int step, Engine const& engine)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return;
    auto const any = [](std::size_t) {
        return true;
    };
    switch (field)
    {
        case FormField::Task: {
            auto const tasks = config->tasks();
            form.task = cycled(form.task, tasks.size(), step, [&](std::size_t index) {
                return tasks[index].appliesTo == appliesToOf(form.item);
            });
            // A task comes with its own model and permission profile.
            auto const models = config->models();
            if (auto const model = std::ranges::find(models, tasks[form.task].model); model != models.end())
                form.model = static_cast<std::size_t>(std::ranges::distance(models.begin(), model));
            form.permissions = profileIndexFor(*config, tasks[form.task].id);
            form.worker = workerIndexFor(engine, *config, tasks[form.task].id);
            break;
        }
        case FormField::Worker: form.worker = cycled(form.worker, config->workers().size(), step, any); break;
        case FormField::Model: form.model = cycled(form.model, config->models().size(), step, any); break;
        case FormField::Permissions:
            form.permissions = cycled(form.permissions, config->profiles().size(), step, any);
            break;
        case FormField::Repo: {
            auto const repos = config->repos();
            auto const at = std::ranges::find(repos, form.repo, &Repo::name);
            auto const index = static_cast<std::size_t>(std::ranges::distance(repos.begin(), at));
            if (index < repos.size())
                form.repo = repos[cycled(index, repos.size(), step, any)].name;
            break;
        }
        case FormField::Note: break;
    }
}

std::optional<std::vector<std::string>> commandShown(Run const& run)
{
    auto const* const pane = findPaneTemplate(agentPaneTemplateOf(run));
    if (pane == nullptr)
        return std::nullopt;
    auto arguments = agentPaneArguments(run);
    for (auto& argument: arguments)
        if (std::ranges::contains(ShownByPlaceholder, argument.name))
            argument.value = std::format("{{{}}}", argument.name);
    auto shown = substitute(pane->argv, arguments);
    return shown ? std::optional { std::move(*shown) } : std::nullopt;
}

WorkerDef const* workerOf(DispatchFormState const& form, Engine const& engine) noexcept
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return nullptr;
    auto const workers = config->workers();
    if (form.worker < workers.size())
        return &workers[form.worker];
    auto const tasks = config->tasks();
    return &config->workerFor(form.task < tasks.size() ? std::string_view { tasks[form.task].id } : ""sv);
}

bool isSetupIncomplete(Engine const& engine, WorkerDef const& worker) noexcept
{
    if (agentRowOf(worker.agent).claudeState == ClaudeState::None)
        return false;
    return engine.hooksFor(worker.envSet) == HooksState::Missing
           || !engine.missingPluginsFor(worker.envSet).empty();
}

HooksWarning setupWarningOf(DispatchFormState const& form, Engine const& engine) noexcept
{
    auto const* const worker = workerOf(form, engine);
    return worker != nullptr && isSetupIncomplete(engine, *worker) ? HooksWarning::Shown : HooksWarning::None;
}

SetupGate passSetupWarning(DispatchFormState const& form,
                           Engine const& engine,
                           HooksWarning& hooks,
                           HooksWarning hooksAtInput) noexcept
{
    if (setupWarningOf(form, engine) == HooksWarning::None)
        return SetupGate::Passed;
    // The first Enter confirms the warning; only an Enter of a later input dispatches, so a paste or a held
    // key cannot click through it.
    if (hooks != HooksWarning::Confirmed)
    {
        hooks = HooksWarning::Confirmed;
        return SetupGate::Held;
    }
    return hooksAtInput == HooksWarning::Confirmed ? SetupGate::Passed : SetupGate::Held;
}

std::string_view enterHint(DispatchFormState const& form, Engine const& engine, HooksWarning hooks) noexcept
{
    // What Enter does while the setup is incomplete, indexed by HooksWarning.
    constexpr auto IncompleteEnterHints = std::array { "enter confirms the warning"sv,
                                                       "enter confirms the warning"sv,
                                                       "enter starts it anyway"sv };
    static_assert(IncompleteEnterHints.size() == std::to_underlying(HooksWarning::Confirmed) + 1U);
    if (setupWarningOf(form, engine) == HooksWarning::None)
        return "enter start"sv;
    return IncompleteEnterHints[std::to_underlying(hooks)];
}

/// @return @p text with the run's stamp, which is known only once the form is confirmed, shown as `…`.
std::string withoutStamp(std::string_view text, Run const& run)
{
    auto const slug = itemSlug(run.item);
    auto const at = text.find(run.id);
    if (!slug || at == std::string_view::npos)
        return std::string { text };
    return std::format("{}{}-…{}", text.substr(0, at), *slug, text.substr(at + run.id.size()));
}

/// @return How the form checks the item out into the run's worktree.
std::string checkoutText(Run const& run)
{
    auto const branch = withoutStamp(run.branch, run);
    if (auto const* const request = std::get_if<ChangeRequestRef>(&run.item))
        return std::format("gh pr checkout {} --branch {}", request->number, branch);
    return std::format("new branch {}", branch);
}

std::vector<std::string> setupWarnings(Engine const& engine, WorkerDef const& worker)
{
    auto warnings = std::vector<std::string> {};
    if (agentRowOf(worker.agent).claudeState == ClaudeState::None)
        return warnings;
    if (engine.hooksFor(worker.envSet) == HooksState::Missing)
        warnings.emplace_back(HooksMissingWarning);
    for (auto const& plugin: engine.missingPluginsFor(worker.envSet))
        warnings.push_back(
            std::format("Claude Code lacks the plugin {}: a prompt naming its skills fails (^K "
                        "on the dashboard installs it)",
                        plugin.id));
    return warnings;
}

} // namespace orchestrator::view
