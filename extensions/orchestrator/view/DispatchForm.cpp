// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/DispatchForm.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

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

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

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
    };
    static_assert(RefusalTexts.size() == std::to_underlying(DispatchRefusal::UnknownProfile) + 1U,
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

    /// The form's fields in the order Tab moves through them: an item's, and a topic's.
    constexpr auto FieldOrder = std::array { FormField::Task, FormField::Model, FormField::Note };
    constexpr auto TopicFieldOrder =
        std::array { FormField::Repo, FormField::Note, FormField::Task, FormField::Model };

    /// @return Whether @p form is a topic's.
    [[nodiscard]] bool isTopicForm(DispatchFormState const& form) noexcept
    {
        return std::holds_alternative<TopicRef>(form.item);
    }

    /// @return The order Tab moves through @p form's fields.
    [[nodiscard]] std::span<FormField const> fieldOrderOf(DispatchFormState const& form) noexcept
    {
        if (isTopicForm(form))
            return TopicFieldOrder;
        return FieldOrder;
    }

    // }}}
    // {{{ the item

    /// The url and title the engine last listed for an item.
    struct Listed
    {
        std::string url;
        std::string title;
    };

    [[nodiscard]] std::optional<Listed> listedItem(Engine const& engine,
                                                   std::string_view repo,
                                                   ItemRef const& item)
    {
        if (auto const* const request = std::get_if<ChangeRequestRef>(&item))
        {
            auto const requests = engine.changeRequests(repo);
            auto const found = std::ranges::find(requests, request->number, &ChangeRequest::number);
            if (found != requests.end())
                return Listed { .url = found->url, .title = found->title };
            return std::nullopt;
        }
        auto const issues = engine.unclaimedIssues(repo);
        auto const found =
            std::ranges::find_if(issues, [&](Issue const& issue) { return issue.item == item; });
        if (found != issues.end())
            return Listed { .url = found->url, .title = found->title };
        return std::nullopt;
    }

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

    /// @return Whether the note would be read as an option.
    [[nodiscard]] bool startsWithDash(DispatchFormState const& form) noexcept
    {
        return form.note.starts_with('-');
    }

    /// @return Whether Enter does something now: not while the note starts with `-` or the engine would
    /// refuse.
    [[nodiscard]] bool enterActs(DispatchFormState const& form, Engine const& engine)
    {
        return !startsWithDash(form) && previewOf(form, engine).has_value();
    }

    // }}}
} // namespace

std::string_view refusalText(DispatchRefusal refusal) noexcept
{
    return RefusalTexts[std::to_underlying(refusal)];
}

std::optional<DispatchFormState> openDispatchForm(Engine const& engine,
                                                  std::string_view repo,
                                                  ItemRef const& item,
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
    auto listed = listedItem(engine, repo, item);
    if (!listed)
        return std::nullopt;
    auto const models = config->models();
    auto const model = std::ranges::find(models, found->model);
    return DispatchFormState {
        .repo = std::string { repo },
        .item = item,
        .url = std::move(listed->url),
        .title = std::move(listed->title),
        .task = static_cast<std::size_t>(std::ranges::distance(tasks.begin(), found)),
        .model = model != models.end()
                     ? static_cast<std::size_t>(std::ranges::distance(models.begin(), model))
                     : 0,
        .permissions = profileIndexFor(*config, found->id),
        .note = {},
        .session = std::move(session),
        .focus = FormField::Task,
        .hooks = isSetupIncomplete(engine) ? HooksWarning::Shown : HooksWarning::None,
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
        .note = std::move(topic),
        .session = std::move(session),
        .focus = FormField::Note,
        .hooks = isSetupIncomplete(engine) ? HooksWarning::Shown : HooksWarning::None,
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
        .topic = isTopicForm(form) ? form.note : std::string {},
        .task = form.task < tasks.size() ? tasks[form.task].id : std::string {},
        .model = form.model < models.size() ? models[form.model] : std::string {},
        .permissions = config != nullptr && form.permissions < config->profiles().size()
                           ? config->profiles()[form.permissions].name
                           : std::string {},
        // A topic form's text is its topic, not a note.
        .note = form.note.empty() || isTopicForm(form) ? std::nullopt : std::optional { form.note },
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
                                     .note = std::move(action.note),
                                     .session = std::move(action.session),
                                     .nearTab = std::nullopt,
                                     .wallMs = 0,
                                     .monoMs = 0 });
}

std::optional<DispatchAction> applyFormKey(core::tui::KeyEvent const& key,
                                           std::optional<DispatchFormState>& form,
                                           Engine const& engine,
                                           HooksWarning hooksAtInput,
                                           std::string_view nearTab)
{
    if (!form)
        return std::nullopt;
    auto const* const config = engine.config();
    auto& state = *form;
    auto const move = [&](int step) {
        auto const order = fieldOrderOf(state);
        auto const at = std::ranges::find(order, state.focus);
        auto const index = static_cast<std::size_t>(std::ranges::distance(order.begin(), at));
        state.focus = order[cycled(index, order.size(), step, [](std::size_t) { return true; })];
    };
    auto const change = [&](int step) {
        if (config == nullptr)
            return;
        if (state.focus == FormField::Task)
        {
            auto const tasks = config->tasks();
            state.task = cycled(state.task, tasks.size(), step, [&](std::size_t index) {
                return tasks[index].appliesTo == appliesToOf(state.item);
            });
            // A task comes with its own model and permission profile.
            auto const models = config->models();
            if (auto const model = std::ranges::find(models, tasks[state.task].model); model != models.end())
                state.model = static_cast<std::size_t>(std::ranges::distance(models.begin(), model));
            state.permissions = profileIndexFor(*config, tasks[state.task].id);
        }
        else if (state.focus == FormField::Permissions)
            state.permissions =
                cycled(state.permissions, config->profiles().size(), step, [](std::size_t) { return true; });
        else if (state.focus == FormField::Model)
            state.model =
                cycled(state.model, config->models().size(), step, [](std::size_t) { return true; });
        else if (state.focus == FormField::Repo)
        {
            auto const repos = config->repos();
            auto const at = std::ranges::find(repos, state.repo, &Repo::name);
            auto const index = static_cast<std::size_t>(std::ranges::distance(repos.begin(), at));
            if (index < repos.size())
                state.repo = repos[cycled(index, repos.size(), step, [](std::size_t) { return true; })].name;
        }
    };

    // A key with Alt is how an Esc reads when the next key follows it in the same read: the user pressed Esc,
    // and the form is cancelled, not acted on (Alt+Enter never dispatches). A Ctrl chord does nothing here.
    if (core::tui::hasModifier(key.modifiers, core::tui::Modifier::Alt))
    {
        form.reset();
        return std::nullopt;
    }
    if (core::tui::hasModifier(key.modifiers, core::tui::Modifier::Ctrl))
        return std::nullopt;
    switch (key.key)
    {
        case core::tui::KeyCode::Escape: form.reset(); return std::nullopt;
        case core::tui::KeyCode::Tab:
            move(core::tui::hasModifier(key.modifiers, core::tui::Modifier::Shift) ? -1 : 1);
            return std::nullopt;
        case core::tui::KeyCode::Up: move(-1); return std::nullopt;
        case core::tui::KeyCode::Down: move(1); return std::nullopt;
        case core::tui::KeyCode::Left: change(-1); return std::nullopt;
        case core::tui::KeyCode::Right: change(1); return std::nullopt;
        case core::tui::KeyCode::Backspace:
            if (state.focus == FormField::Note)
                dropLastCodepoint(state.note);
            return std::nullopt;
        case core::tui::KeyCode::Enter: {
            if (!enterActs(state, engine))
                return std::nullopt;
            if (isSetupIncomplete(engine))
            {
                // The first Enter confirms the warning; only an Enter of a later input dispatches, so a paste
                // or a held key cannot click through it.
                if (state.hooks != HooksWarning::Confirmed)
                {
                    state.hooks = HooksWarning::Confirmed;
                    return std::nullopt;
                }
                if (hooksAtInput != HooksWarning::Confirmed)
                    return std::nullopt;
            }
            auto action = actionOf(state, engine, nearTab);
            form.reset();
            return action;
        }
        default: break;
    }
    if (state.focus != FormField::Note)
        return std::nullopt;
    if (auto const printable = printableOf(key))
        appendUtf8(state.note, *printable);
    return std::nullopt;
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

/// @return Whether the agents' setup lacks something a run needs: the status hooks, or a plugin. The form
///         then says so, and asks for a second Enter.
bool isSetupIncomplete(Engine const& engine) noexcept
{
    return engine.hooks() == HooksState::Missing || !engine.missingPlugins().empty();
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

std::vector<std::string> setupWarnings(Engine const& engine)
{
    auto warnings = std::vector<std::string> {};
    if (engine.hooks() == HooksState::Missing)
        warnings.emplace_back(HooksMissingWarning);
    for (auto const& plugin: engine.missingPlugins())
        warnings.push_back(std::format("Claude Code lacks the plugin {}: a prompt naming its skills fails (I "
                                       "on the dashboard installs it)",
                                       plugin.id));
    return warnings;
}

} // namespace orchestrator::view
