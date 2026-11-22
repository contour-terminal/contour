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

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

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
    constexpr auto ItemSettings =
        std::array { FormField::Task, FormField::Model, FormField::Permissions, FormField::Note };
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

    /// @return Whether the keys go to an item's note: the one text field among the settings.
    [[nodiscard]] bool isOnNote(LauncherState const& state) noexcept
    {
        return state.focus == LauncherFocus::Settings && state.form && state.form->focus == FormField::Note;
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

void pasteIntoLauncher(std::optional<LauncherState>& launcher, Engine const& engine, std::string_view text)
{
    if (!launcher)
        return;
    if (isOnNote(*launcher))
    {
        launcher->form->note += text;
        return;
    }
    launcher->focus = LauncherFocus::List;
    launcher->query += text;
    launcher->highlighted = 0;
    reform(*launcher, engine);
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
            if (isOnNote(state))
            {
                static_cast<void>(toForm(key.key, state, engine, hooksAtInput, nearTab));
                return std::nullopt;
            }
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
    if (isOnNote(state))
    {
        // The note is a text field: what is typed goes into it.
        static_cast<void>(applyFormKey(key, state.form, engine, hooksAtInput, nearTab));
        return std::nullopt;
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

namespace
{
    using namespace std::string_literals;
    using namespace std::string_view_literals;

    /// One run of text drawn in one role.
    struct Segment
    {
        std::string text;
        Role role = Role::Text;
    };

    /// One drawn line: segments from the left, and a few from the right edge.
    struct Line
    {
        std::vector<Segment> left = {};
        std::vector<Segment> right = {};
    };

    /// What each group is headed by; the topic row has no heading.
    constexpr auto GroupHeadings = std::array {
        ""sv,
        "NEEDS ATTENTION"sv,
        "MY PULL REQUESTS"sv,
        "ISSUES WITHOUT A PR"sv,
    };

    /// The cells a value's label takes on a line of its own.
    constexpr auto LabelColumns = 10;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;

    /// A pull request's checks as one glyph.
    [[nodiscard]] Segment ciGlyph(CiState ci)
    {
        switch (ci)
        {
            case CiState::Failing: return { .text = "✗", .role = Role::Failing };
            case CiState::Passing: return { .text = "✓", .role = Role::Passing };
            case CiState::Pending: return { .text = "●", .role = Role::Warning };
            case CiState::None: break;
        }
        return { .text = " " };
    }

    /// A pull request's review as one glyph.
    [[nodiscard]] Segment reviewGlyph(ReviewState review)
    {
        switch (review)
        {
            case ReviewState::ChangesRequested: return { .text = "△", .role = Role::Warning };
            case ReviewState::Approved: return { .text = "✓", .role = Role::Passing };
            case ReviewState::Required: return { .text = "?", .role = Role::Dim };
            case ReviewState::None: break;
        }
        return { .text = " " };
    }

    /// @return The line one candidate is listed on.
    [[nodiscard]] Line candidateLine(Candidate const& candidate, bool selected, int width)
    {
        auto const marker = Segment { .text = selected ? "▍"s : " "s, .role = Role::Accent };
        auto const textRole = selected ? Role::Selection : Role::Text;
        if (std::holds_alternative<TopicRef>(candidate.item))
            return Line { .left = { marker,
                                    { .text = "✎ topic ", .role = Role::Accent },
                                    { .text = cut(std::format("“{}”", candidate.title), width - 24),
                                      .role = textRole } },
                          .right = { { .text = "new branch", .role = Role::Dim } } };
        auto const label = itemLabel(candidate.item);
        auto right = std::vector<Segment> {};
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
        {
            right.push_back(ciGlyph(candidate.ci));
            right.push_back({ .text = "  " });
            right.push_back(reviewGlyph(candidate.review));
        }
        right.push_back({ .text = "  " });
        right.push_back(candidate.activeRun.empty() ? Segment { .text = " " }
                                                    : Segment { .text = "⬢", .role = Role::Accent });
        // Label, a blank, the title; the columns take 7 cells and the marker 1.
        auto const room = width - 1 - columnsOf(label) - 1 - 8;
        return Line { .left = { marker,
                                { .text = label, .role = Role::Dim },
                                { .text = " " },
                                { .text = cut(candidate.title, std::max(room, 4)), .role = textRole } },
                      .right = std::move(right) };
    }

    /// @return What the launcher says about the highlighted item beyond its row: its checks, its review and
    ///         the agent on it.
    [[nodiscard]] std::string detailOf(Candidate const& candidate)
    {
        if (std::holds_alternative<TopicRef>(candidate.item))
            return "an agent on what you typed, on a new branch";
        auto parts =
            std::vector<std::string> { std::format("{} · {}", itemLabel(candidate.item), candidate.repo) };
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
        {
            switch (candidate.ci)
            {
                case CiState::Failing:
                    parts.push_back(candidate.failingChecks.empty()
                                        ? "checks fail"s
                                        : std::format("checks fail: {}{}",
                                                      candidate.failingChecks.front(),
                                                      candidate.failingChecks.size() > 1
                                                          ? std::format(" and {} more",
                                                                        candidate.failingChecks.size() - 1)
                                                          : std::string {}));
                    break;
                case CiState::Passing: parts.emplace_back("checks pass"); break;
                case CiState::Pending: parts.emplace_back("checks running"); break;
                case CiState::None: break;
            }
            if (candidate.review == ReviewState::ChangesRequested)
                parts.emplace_back("changes requested");
            else if (candidate.review == ReviewState::Approved)
                parts.emplace_back("approved");
        }
        if (!candidate.activeRun.empty())
            parts.push_back(std::format("{} works on it", candidate.activeRun));
        auto text = std::string {};
        for (auto const& part: parts)
            text += text.empty() ? part : std::format(" · {}", part);
        return text;
    }

    /// Appends a labelled value, wrapped, continuation lines indented under it.
    void appendValue(
        std::vector<Line>& lines, std::string_view label, std::string_view value, Role role, int width)
    {
        auto const wrapped = wrap(value, width - LabelColumns, width - LabelColumns - 2);
        for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
        {
            auto const lead =
                index == 0 ? std::format("{:<{}}", label, LabelColumns) : std::string(LabelColumns + 2, ' ');
            lines.push_back(Line {
                .left = { { .text = lead, .role = Role::Dim }, { .text = wrapped[index], .role = role } } });
        }
    }

    /// @return The settings line: each setting's value between arrows, the one with the focus selected.
    [[nodiscard]] Line settingsLine(LauncherState const& state, Engine const& engine)
    {
        auto line = Line {};
        if (!state.form)
            return line;
        auto const& form = *state.form;
        auto const* const config = engine.config();
        auto const tasks = config != nullptr ? config->tasks() : std::span<TaskDef const> {};
        auto const models = config != nullptr ? config->models() : std::span<std::string const> {};
        auto const profiles = config != nullptr ? config->profiles() : std::span<PermissionProfile const> {};
        auto const add = [&](std::string_view label, FormField field, std::string_view value) {
            auto const focused = state.focus == LauncherFocus::Settings && form.focus == field;
            if (!line.left.empty())
                line.left.push_back({ .text = "   " });
            line.left.push_back({ .text = std::format("{} ", label), .role = Role::Dim });
            line.left.push_back(
                { .text = std::format("‹ {} ›", value), .role = focused ? Role::Selection : Role::Text });
        };
        if (std::holds_alternative<TopicRef>(form.item))
            add("repo", FormField::Repo, form.repo);
        add("task",
            FormField::Task,
            form.task < tasks.size() ? std::string_view { tasks[form.task].label } : "?"sv);
        add("model",
            FormField::Model,
            form.model < models.size() ? std::string_view { models[form.model] } : "?"sv);
        add("perms",
            FormField::Permissions,
            form.permissions < profiles.size() ? std::string_view { profiles[form.permissions].name }
                                               : "?"sv);
        if (!std::holds_alternative<TopicRef>(form.item))
        {
            auto const focused = state.focus == LauncherFocus::Settings && form.focus == FormField::Note;
            line.left.push_back({ .text = "   note ", .role = Role::Dim });
            line.left.push_back({ .text = focused             ? std::format("{}▏", form.note)
                                          : form.note.empty() ? "none"s
                                                              : form.note,
                                  .role = focused                      ? Role::Selection
                                          : form.note.starts_with('-') ? Role::Failing
                                                                       : Role::Text });
        }
        return line;
    }

    /// @return The hint line: what Enter does now, then the other keys.
    [[nodiscard]] Line hintLine(LauncherState const& state, Engine const& engine)
    {
        auto enter = "enter start"sv;
        if (!state.form || !previewOf(*state.form, engine))
            enter = ""sv;
        else if (isSetupIncomplete(engine) && state.form->hooks != HooksWarning::Confirmed)
            enter = "enter confirms the warning"sv;
        else if (isSetupIncomplete(engine))
            enter = "enter starts it anyway"sv;
        auto text = std::string { enter };
        for (auto const hint: { "↑↓ choose"sv, "tab settings"sv, "ctrl+p more"sv, "esc close"sv })
            text += text.empty() ? std::string { hint } : std::format(" · {}", hint);
        return Line { .left = { { .text = text, .role = Role::Dim } } };
    }

    /// The launcher's lines in three parts: the query above the list, every row of the list, and what follows
    /// it.
    struct Layout
    {
        std::vector<Line> head = {};
        std::vector<Line> list = {};
        std::size_t highlightedAt = 0; ///< The highlighted candidate's row in @ref list.
        std::vector<Line> tail = {};

        /// @return Every line, the list cut to @p rows rows around the highlight, and a row saying how many
        ///         more there are when it is cut.
        [[nodiscard]] std::vector<Line> lines(std::size_t rows) const
        {
            auto all = head;
            rows = std::max<std::size_t>(rows, 1);
            // A cut list gives its last row to the line that says how many more there are.
            if (list.size() > rows && rows > 1)
                --rows;
            auto const first =
                list.size() <= rows
                    ? std::size_t { 0 }
                    : std::min(highlightedAt > rows / 2 ? highlightedAt - rows / 2 : 0, list.size() - rows);
            for (auto const& line: list | std::views::drop(first) | std::views::take(rows))
                all.push_back(line);
            if (list.size() > first + rows)
                all.push_back(
                    Line { .left = { { .text = std::format("  ↓ {} more", list.size() - first - rows),
                                       .role = Role::Dim } } });
            std::ranges::copy(tail, std::back_inserter(all));
            return all;
        }
    };

    [[nodiscard]] Layout layoutOf(LauncherState const& state, Engine const& engine, int width)
    {
        auto layout = Layout {};
        layout.head.push_back(Line {
            .left = { { .text = "› ", .role = Role::Accent },
                      state.query.empty() && state.focus == LauncherFocus::List
                          ? Segment { .text = "▏type to filter, or describe a topic", .role = Role::Dim }
                          : Segment { .text = state.focus == LauncherFocus::List
                                                  ? std::format("{}▏", state.query)
                                                  : state.query } },
            .right = { { .text = "pull requests · issues · a topic", .role = Role::Dim } } });
        layout.head.emplace_back();

        // The list: each candidate, with a heading where its group starts.
        auto const candidates = candidatesOf(engine, state.query);
        for (auto const index: std::views::iota(std::size_t { 0 }, candidates.size()))
        {
            auto const& candidate = candidates[index];
            if (candidate.group != CandidateGroup::Topic
                && (index == 0 || candidates[index - 1].group != candidate.group))
                layout.list.push_back(Line {
                    .left = { { .text = std::string { GroupHeadings[std::to_underlying(candidate.group)] },
                                .role = Role::Dim } } });
            if (index == state.highlighted)
                layout.highlightedAt = layout.list.size();
            layout.list.push_back(candidateLine(candidate, index == state.highlighted, width));
        }

        auto& tail = layout.tail;
        auto rule = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, std::max(width, 0)))
            rule += "─";
        tail.push_back(Line { .left = { { .text = std::move(rule), .role = Role::Border } } });
        tail.push_back(settingsLine(state, engine));
        if (state.highlighted < candidates.size())
            tail.push_back(Line { .left = { { .text = cut(detailOf(candidates[state.highlighted]), width),
                                              .role = Role::Dim } } });
        if (state.form)
        {
            if (auto const preview = previewOf(*state.form, engine))
            {
                appendValue(tail, "checkout", checkoutText(preview->run), Role::Text, width);
                appendValue(
                    tail, "worktree", withoutStamp(preview->run.worktree, preview->run), Role::Text, width);
                appendValue(tail,
                            "perms",
                            permissionsText(preview->run.permissions, preview->run.settings),
                            Role::Text,
                            width);
                if (state.preview == LauncherPreview::Full)
                {
                    appendValue(tail, "prompt", preview->run.prompt, Role::Text, width);
                    if (auto const shown = commandShown(preview->run))
                        appendValue(tail, "command", shellQuoted(*shown), Role::Dim, width);
                }
            }
            else
                appendValue(tail, "refused", refusalText(preview.error()), Role::Failing, width);
        }
        else if (!candidates.empty())
            appendValue(tail, "refused", "no task applies to it", Role::Failing, width);
        if (state.form && state.form->note.starts_with('-'))
            tail.push_back(Line { .left = { { .text = std::holds_alternative<TopicRef>(state.form->item)
                                                          ? "a topic may not start with -"
                                                          : "a note may not start with -",
                                              .role = Role::Failing } } });
        for (auto const& warning: setupWarnings(engine))
            for (auto const& part: wrap(warning, width, width))
                tail.push_back(Line { .left = { { .text = part, .role = Role::Warning } } });
        tail.emplace_back();
        tail.push_back(hintLine(state, engine));
        return layout;
    }
} // namespace

void LauncherView::render(core::tui::Canvas& canvas)
{
    if (!_launcher)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "✦ New agent");
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - SideColumns;
    auto const layout = layoutOf(*_launcher, _engine, width);
    // The list takes what the rest leaves.
    auto const others = static_cast<int>(layout.head.size() + layout.tail.size());
    auto const listRows = std::clamp(inside.height() - others, 1, MaxLauncherListRows);
    auto const lines = layout.lines(static_cast<std::size_t>(listRows));
    for (auto const index:
         std::views::iota(std::size_t { 0 },
                          std::min(lines.size(), static_cast<std::size_t>(std::max(inside.height(), 0)))))
    {
        auto const y = static_cast<int>(index);
        auto x = 1;
        for (auto const& segment: lines[index].left)
        {
            // Segments are sanitised already, and their blanks separate them: cutSanitised() keeps them.
            auto const text = cutSanitised(segment.text, width + 1 - x);
            inside.putString(y, x, text, styleOf(theme, segment.role));
            x += columnsOf(text);
        }
        auto rightWidth = 0;
        for (auto const& segment: lines[index].right)
            rightWidth += columnsOf(segment.text);
        if (lines[index].right.empty() || x + 1 + rightWidth > width + 1)
            continue;
        auto rx = width + 1 - rightWidth;
        for (auto const& segment: lines[index].right)
        {
            inside.putString(y, rx, segment.text, styleOf(theme, segment.role));
            rx += columnsOf(segment.text);
        }
    }
}

int LauncherView::heightAt(int width) const
{
    if (!_launcher)
        return 0;
    auto const inner = width - 2 - SideColumns;
    // The box's two rows around its lines.
    return static_cast<int>(layoutOf(*_launcher, _engine, inner).lines(MaxLauncherListRows).size()) + 2;
}

} // namespace orchestrator::view
