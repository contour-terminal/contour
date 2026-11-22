// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Launcher.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>

namespace orchestrator::view
{

namespace
{
    /// @return @p text with ASCII letters lower-cased: titles are matched as typed, whatever their case.
    [[nodiscard]] std::string folded(std::string_view text)
    {
        return text | std::views::transform([](char c) {
                   return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
               })
               | std::ranges::to<std::string>();
    }

    /// @return @p text without the blanks around it.
    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        auto const isBlank = [](char c) {
            return c == ' ' || c == '\t';
        };
        while (!text.empty() && isBlank(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isBlank(text.back()))
            text.remove_suffix(1);
        return text;
    }

    [[nodiscard]] bool needsAttention(ChangeRequest const& request) noexcept
    {
        return request.ci == CiState::Failing || request.review == ReviewState::ChangesRequested;
    }

    /// @return Every item the engine lists, in group order and, within a group, the engine's.
    [[nodiscard]] std::vector<Candidate> itemsOf(Engine const& engine)
    {
        auto items = std::vector<Candidate> {};
        auto const activeRunOf = [&](std::string_view repo, ItemRef const& item) {
            auto const* const run = engine.activeRunOn(repo, item);
            return run != nullptr ? run->id : std::string {};
        };
        for (auto const& repo: engine.repositories())
        {
            for (auto const& request: engine.changeRequests(repo))
            {
                auto const item = ItemRef { ChangeRequestRef { request.number } };
                items.push_back(Candidate { .group = needsAttention(request) ? CandidateGroup::NeedsAttention
                                                                             : CandidateGroup::MyPullRequests,
                                            .repo = repo,
                                            .item = item,
                                            .url = request.url,
                                            .title = request.title,
                                            .ci = request.ci,
                                            .review = request.review,
                                            .failingChecks = request.failingChecks,
                                            .activeRun = activeRunOf(repo, item) });
            }
            for (auto const& issue: engine.unclaimedIssues(repo))
                items.push_back(Candidate { .group = CandidateGroup::Issues,
                                            .repo = repo,
                                            .item = issue.item,
                                            .url = issue.url,
                                            .title = issue.title,
                                            .activeRun = activeRunOf(repo, issue.item) });
        }
        std::ranges::stable_sort(items, {}, &Candidate::group);
        return items;
    }

    /// @return Whether @p query names @p candidate exactly: its number, `#number`, URL or Jira key.
    [[nodiscard]] bool namesExactly(Candidate const& candidate, std::string_view query)
    {
        auto const label = itemLabel(candidate.item);
        return query == candidate.url || folded(query) == folded(label)
               || (label.starts_with('#') && query == std::string_view { label }.substr(1));
    }

    /// The settings Tab moves through: an item's, and a topic's, which also chooses its repository.
    constexpr auto ItemSettings = std::array { FormField::Task, FormField::Model, FormField::Permissions };
    constexpr auto TopicSettings =
        std::array { FormField::Repo, FormField::Task, FormField::Model, FormField::Permissions };

    [[nodiscard]] std::span<FormField const> settingsOf(DispatchFormState const& form) noexcept
    {
        if (std::holds_alternative<TopicRef>(form.item))
            return TopicSettings;
        return ItemSettings;
    }

    /// Builds the form for the highlighted candidate afresh: the settings follow the row, and only how far
    /// the user got past the setup warning, and a topic's repository, carry over.
    void reform(LauncherState& state, Engine const& engine)
    {
        auto const candidates = candidatesOf(engine, state.query);
        if (candidates.empty())
        {
            state.form.reset();
            return;
        }
        state.highlighted = std::min(state.highlighted, candidates.size() - 1);
        auto const& candidate = candidates[state.highlighted];
        auto const previous = std::move(state.form);
        auto const* const config = engine.config();
        if (std::holds_alternative<TopicRef>(candidate.item))
        {
            auto const repo = previous && std::holds_alternative<TopicRef>(previous->item) ? previous->repo
                                                                                           : std::string {};
            state.form = openTopicForm(engine, repo, candidate.title, state.session);
        }
        else
        {
            auto const* const task =
                config != nullptr ? config->firstTaskFor(appliesToOf(candidate.item)) : nullptr;
            state.form =
                task != nullptr
                    ? openDispatchForm(engine, candidate.repo, candidate.item, task->id, state.session)
                    : std::nullopt;
        }
        if (state.form && previous)
            state.form->hooks = previous->hooks;
        if (state.form)
            state.form->focus = settingsOf(*state.form).front();
    }

    /// Feeds @p code to the highlighted candidate's form, as the dispatch form's own keys.
    [[nodiscard]] std::optional<DispatchAction> toForm(core::tui::KeyCode code,
                                                       LauncherState& state,
                                                       Engine const& engine,
                                                       HooksWarning hooksAtInput,
                                                       std::string_view nearTab)
    {
        return applyFormKey(core::tui::KeyEvent { .key = code }, state.form, engine, hooksAtInput, nearTab);
    }
} // namespace

std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query)
{
    auto items = itemsOf(engine);
    auto const typed = trimmed(query);
    if (typed.empty())
        return items;

    auto const needle = folded(typed);
    auto exact = std::vector<Candidate> {};
    auto matching = std::vector<Candidate> {};
    for (auto& candidate: items)
    {
        if (namesExactly(candidate, typed))
            exact.push_back(std::move(candidate));
        else if (folded(std::format("{} {}", itemLabel(candidate.item), candidate.title)).contains(needle))
            matching.push_back(std::move(candidate));
    }
    auto candidates = std::move(exact);
    candidates.push_back(
        Candidate { .group = CandidateGroup::Topic, .item = TopicRef {}, .title = std::string { typed } });
    std::ranges::move(matching, std::back_inserter(candidates));
    return candidates;
}

std::optional<LauncherState> openLauncher(Engine const& engine, std::string query, std::string session)
{
    if (engine.config() == nullptr)
        return std::nullopt;
    auto state = LauncherState { .query = std::move(query), .session = std::move(session) };
    reform(state, engine);
    return state;
}

std::optional<LauncherState> openLauncherOn(Engine const& engine,
                                            std::string_view repo,
                                            ItemRef const& item,
                                            std::optional<std::string_view> task,
                                            std::optional<std::string_view> model,
                                            std::string session)
{
    auto state = openLauncher(engine, itemLabel(item), std::move(session));
    if (!state)
        return std::nullopt;
    // The label names the item exactly, so it stands first; another repository's item of the same number
    // stands beside it.
    auto const candidates = candidatesOf(engine, state->query);
    auto const at = std::ranges::find_if(candidates, [&](Candidate const& candidate) {
        return candidate.repo == repo && candidate.item == item;
    });
    if (at == candidates.end())
        return std::nullopt;
    state->highlighted = static_cast<std::size_t>(std::ranges::distance(candidates.begin(), at));
    reform(*state, engine);
    if (!state->form)
        return state;
    auto const* const config = engine.config();
    if (task)
    {
        auto opened = openDispatchForm(engine, repo, item, *task, state->session);
        if (opened)
        {
            opened->hooks = state->form->hooks;
            opened->focus = state->form->focus;
            state->form = std::move(opened);
        }
    }
    if (model)
        if (auto const found = std::ranges::find(config->models(), *model); found != config->models().end())
            state->form->model =
                static_cast<std::size_t>(std::ranges::distance(config->models().begin(), found));
    return state;
}

std::optional<DispatchAction> applyLauncherKey(core::tui::KeyEvent const& key,
                                               std::optional<LauncherState>& launcher,
                                               Engine const& engine,
                                               HooksWarning hooksAtInput,
                                               std::string_view nearTab)
{
    if (!launcher)
        return std::nullopt;
    auto& state = *launcher;
    using core::tui::KeyCode;
    using core::tui::Modifier;

    // A key with Alt is how an Esc reads when the next key follows it in the same read: the user pressed Esc.
    if (core::tui::hasModifier(key.modifiers, Modifier::Alt))
    {
        launcher.reset();
        return std::nullopt;
    }
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
    {
        // Ctrl+P arrives as `p` (or Ctrl+Shift+P as `P`) with the Ctrl modifier.
        if (key.codepoint == U'p' || key.codepoint == U'P' || key.key == static_cast<KeyCode>(U'p'))
            state.preview =
                state.preview == LauncherPreview::Short ? LauncherPreview::Full : LauncherPreview::Short;
        return std::nullopt;
    }

    auto const settingStep = [&](int step) {
        if (!state.form)
            return;
        auto const order = settingsOf(*state.form);
        auto const at = std::ranges::find(order, state.form->focus);
        auto const index = static_cast<std::ptrdiff_t>(std::ranges::distance(order.begin(), at)) + step;
        if (index < 0 || index >= static_cast<std::ptrdiff_t>(order.size()))
        {
            state.focus = LauncherFocus::List;
            state.form->focus = order.front();
            return;
        }
        state.form->focus = order[static_cast<std::size_t>(index)];
    };

    switch (key.key)
    {
        case KeyCode::Escape: launcher.reset(); return std::nullopt;
        case KeyCode::Tab: {
            auto const back = core::tui::hasModifier(key.modifiers, Modifier::Shift);
            if (state.focus == LauncherFocus::List)
            {
                if (state.form)
                {
                    state.focus = LauncherFocus::Settings;
                    auto const order = settingsOf(*state.form);
                    state.form->focus = back ? order.back() : order.front();
                }
            }
            else
                settingStep(back ? -1 : 1);
            return std::nullopt;
        }
        case KeyCode::Up:
        case KeyCode::Down: {
            if (state.focus == LauncherFocus::Settings)
            {
                state.focus = LauncherFocus::List;
                return std::nullopt;
            }
            auto const count = candidatesOf(engine, state.query).size();
            if (count == 0)
                return std::nullopt;
            state.highlighted = key.key == KeyCode::Up ? (state.highlighted + count - 1) % count
                                                       : (state.highlighted + 1) % count;
            reform(state, engine);
            return std::nullopt;
        }
        case KeyCode::Left:
        case KeyCode::Right:
            if (state.focus == LauncherFocus::Settings && state.form)
            {
                static_cast<void>(toForm(key.key, state, engine, hooksAtInput, nearTab));
            }
            return std::nullopt;
        case KeyCode::Backspace:
            state.focus = LauncherFocus::List;
            dropLastCodepoint(state.query);
            state.highlighted = 0;
            reform(state, engine);
            return std::nullopt;
        case KeyCode::Enter: {
            if (!state.form)
                return std::nullopt;
            auto action = toForm(KeyCode::Enter, state, engine, hooksAtInput, nearTab);
            if (action)
                launcher.reset();
            else if (!state.form)
                // The form closes only as it dispatches; it stays open for the launcher otherwise.
                reform(state, engine);
            return action;
        }
        default: break;
    }
    if (auto const printable = printableOf(key))
    {
        state.focus = LauncherFocus::List;
        appendUtf8(state.query, *printable);
        state.highlighted = 0;
        reform(state, engine);
    }
    return std::nullopt;
}

} // namespace orchestrator::view
