// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Launcher.hpp>

#include <core/Utils.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <array>
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
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

namespace
{
    using namespace std::string_literals;
    using namespace std::string_view_literals;

    // {{{ the candidates

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
        return query == candidate.url || equalsIgnoringCase(query, label)
               || (label.starts_with('#') && query == std::string_view { label }.substr(1));
    }

    // }}}
    // {{{ the state

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

    /// Builds the choices for the highlighted one of @p candidates afresh: they follow the row, and only a
    /// topic's repository carries over from the row before.
    /// @param task The task to start an item on while it applies to it; else the item's first.
    void reform(LauncherState& state,
                Engine const& engine,
                std::span<Candidate const> candidates,
                std::string_view task = {})
    {
        if (candidates.empty())
        {
            state.form.reset();
            return;
        }
        state.highlighted = std::min(state.highlighted, candidates.size() - 1);
        auto const& candidate = candidates[state.highlighted];
        auto const previous = std::move(state.form);
        if (std::holds_alternative<TopicRef>(candidate.item))
        {
            auto const repo = previous && std::holds_alternative<TopicRef>(previous->item) ? previous->repo
                                                                                           : std::string {};
            state.form = openTopicForm(engine, repo, candidate.title, state.session);
        }
        else
        {
            auto const* const config = engine.config();
            auto const* chosen = config != nullptr ? config->findTask(task) : nullptr;
            if (chosen == nullptr || chosen->appliesTo != appliesToOf(candidate.item))
                chosen = config != nullptr ? config->firstTaskFor(appliesToOf(candidate.item)) : nullptr;
            state.form =
                chosen != nullptr
                    ? openDispatchForm(engine, candidate.repo, candidate.item, chosen->id, state.session)
                    : std::nullopt;
        }
        if (state.form)
            state.setting = settingsOf(*state.form).front();
    }

    /// Re-lists for the query, highlights its first row and builds its choices, the keys back on the list.
    void requery(LauncherState& state, Engine const& engine)
    {
        state.focus = LauncherFocus::List;
        state.highlighted = 0;
        reform(state, engine, candidatesOf(engine, state.query));
    }

    /// @return Whether the keys go to an item's note: the one text field among the settings.
    [[nodiscard]] bool isOnNote(LauncherState const& state) noexcept
    {
        return state.focus == LauncherFocus::Settings && state.form && state.setting == FormField::Note;
    }

    /// Confirms the launcher: the dispatch of its choices, unless they cannot be confirmed, or the setup is
    /// incomplete and its warning was not confirmed by an earlier input.
    [[nodiscard]] std::optional<DispatchAction> confirm(LauncherState& state,
                                                        Engine const& engine,
                                                        HooksWarning hooksAtInput,
                                                        std::string_view nearTab)
    {
        if (!state.form || !canConfirm(*state.form, engine))
            return std::nullopt;
        if (isSetupIncomplete(engine))
        {
            // The first Enter confirms the warning; only an Enter of a later input dispatches, so a paste or
            // a held key cannot click through it.
            if (state.hooks != HooksWarning::Confirmed)
            {
                state.hooks = HooksWarning::Confirmed;
                return std::nullopt;
            }
            if (hooksAtInput != HooksWarning::Confirmed)
                return std::nullopt;
        }
        return actionOf(*state.form, engine, nearTab);
    }

    // }}}
} // namespace

std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query)
{
    auto items = itemsOf(engine);
    auto const typed = core::trim(query);
    if (typed.empty())
        return items;

    auto exact = std::vector<Candidate> {};
    auto matching = std::vector<Candidate> {};
    for (auto& candidate: items)
    {
        if (namesExactly(candidate, typed))
            exact.push_back(std::move(candidate));
        else if (containsIgnoringCase(itemLabel(candidate.item), typed)
                 || containsIgnoringCase(candidate.title, typed))
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
    auto state =
        LauncherState { .query = std::move(query),
                        .session = std::move(session),
                        .hooks = isSetupIncomplete(engine) ? HooksWarning::Shown : HooksWarning::None };
    requery(state, engine);
    return state;
}

std::expected<LauncherState, LauncherRefusal> openLauncherOn(Engine const& engine,
                                                             std::string_view repo,
                                                             ItemRef const& item,
                                                             std::string_view task,
                                                             std::string session)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return std::unexpected(LauncherRefusal::NotConfigured);
    if (config->firstTaskFor(appliesToOf(item)) == nullptr)
        return std::unexpected(LauncherRefusal::NoTask);
    auto state =
        LauncherState { .query = itemLabel(item),
                        .session = std::move(session),
                        .hooks = isSetupIncomplete(engine) ? HooksWarning::Shown : HooksWarning::None };
    // The label names the item exactly, so it stands first; another repository's item of the same number
    // stands beside it.
    auto const candidates = candidatesOf(engine, state.query);
    auto const at = std::ranges::find_if(candidates, [&](Candidate const& candidate) {
        return candidate.repo == repo && candidate.item == item;
    });
    if (at == candidates.end())
        return std::unexpected(LauncherRefusal::NotListed);
    state.highlighted = static_cast<std::size_t>(std::ranges::distance(candidates.begin(), at));
    reform(state, engine, candidates, task);
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
    launcher->query += text;
    requery(*launcher, engine);
}

void highlightCandidate(LauncherState& state, Engine const& engine, std::size_t index)
{
    state.focus = LauncherFocus::List;
    state.highlighted = index;
    reform(state, engine, candidatesOf(engine, state.query));
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
    if (isCtrl(key, U'p'))
    {
        state.preview =
            state.preview == LauncherPreview::Short ? LauncherPreview::Full : LauncherPreview::Short;
        return std::nullopt;
    }
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
        return std::nullopt;

    switch (key.key)
    {
        case KeyCode::Escape: launcher.reset(); return std::nullopt;
        case KeyCode::Tab: {
            if (!state.form)
                return std::nullopt;
            auto const order = settingsOf(*state.form);
            auto const back = core::tui::hasModifier(key.modifiers, Modifier::Shift);
            if (state.focus == LauncherFocus::List)
            {
                state.focus = LauncherFocus::Settings;
                state.setting = back ? order.back() : order.front();
                return std::nullopt;
            }
            auto const at = static_cast<std::ptrdiff_t>(
                std::ranges::distance(order.begin(), std::ranges::find(order, state.setting)));
            auto const next = at + (back ? -1 : 1);
            if (next < 0 || next >= std::ssize(order))
            {
                state.focus = LauncherFocus::List;
                state.setting = order.front();
            }
            else
                state.setting = order[static_cast<std::size_t>(next)];
            return std::nullopt;
        }
        case KeyCode::Up:
        case KeyCode::Down: {
            if (state.focus == LauncherFocus::Settings)
            {
                state.focus = LauncherFocus::List;
                return std::nullopt;
            }
            auto const candidates = candidatesOf(engine, state.query);
            if (candidates.empty())
                return std::nullopt;
            auto const count = candidates.size();
            state.highlighted = key.key == KeyCode::Up ? (state.highlighted + count - 1) % count
                                                       : (state.highlighted + 1) % count;
            reform(state, engine, candidates);
            return std::nullopt;
        }
        case KeyCode::Left:
        case KeyCode::Right:
            if (state.focus == LauncherFocus::Settings && state.form)
                changeSetting(*state.form, state.setting, key.key == KeyCode::Right ? 1 : -1, engine);
            return std::nullopt;
        case KeyCode::Backspace:
            if (isOnNote(state))
                dropLastCodepoint(state.form->note);
            else
            {
                dropLastCodepoint(state.query);
                requery(state, engine);
            }
            return std::nullopt;
        case KeyCode::Enter: {
            auto action = confirm(state, engine, hooksAtInput, nearTab);
            if (action)
                launcher.reset();
            return action;
        }
        default: break;
    }
    if (auto const printable = printableOf(key))
    {
        if (isOnNote(state))
            appendUtf8(state.form->note, *printable);
        else
        {
            appendUtf8(state.query, *printable);
            requery(state, engine);
        }
    }
    return std::nullopt;
}

namespace
{
    // {{{ drawing

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
        std::optional<std::size_t>
            candidate = {}; ///< The candidate it draws, for a click; none: no candidate.
    };

    /// How a group is headed; an empty heading heads nothing.
    struct GroupRow
    {
        CandidateGroup group;
        std::string_view heading;
    };

    /// Every group, in CandidateGroup's order.
    constexpr auto GroupRows = std::array {
        GroupRow { .group = CandidateGroup::Topic, .heading = ""sv },
        GroupRow { .group = CandidateGroup::NeedsAttention, .heading = "NEEDS ATTENTION"sv },
        GroupRow { .group = CandidateGroup::MyPullRequests, .heading = "MY PULL REQUESTS"sv },
        GroupRow { .group = CandidateGroup::Issues, .heading = "ISSUES WITHOUT A PR"sv },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, GroupRows.size()),
                                      [](std::size_t i) {
                                          return std::to_underlying(GroupRows[i].group) == i;
                                      }),
                  "GroupRows has one row per CandidateGroup, in its order");

    /// The cells a value's label takes on a line of its own.
    constexpr auto LabelColumns = 10;
    /// How far a value's continuation lines are indented past its first.
    constexpr auto ContinuationIndent = 2;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;

    /// @return One glyph of a table of looks, a blank where it has none.
    [[nodiscard]] Segment glyphOf(Look const& look)
    {
        return look.mark.empty() ? Segment { .text = " " }
                                 : Segment { .text = std::string { look.mark }, .role = look.role };
    }

    [[nodiscard]] int columnsOf(std::span<Segment const> segments)
    {
        auto columns = 0;
        for (auto const& segment: segments)
            columns += view::columnsOf(segment.text);
        return columns;
    }

    /// @return The line one candidate is listed on.
    [[nodiscard]] Line candidateLine(Candidate const& candidate, bool selected, int width)
    {
        auto const marker = Segment { .text = selected ? "▍"s : " "s, .role = Role::Accent };
        auto const textRole = selected ? Role::Selection : Role::Text;
        if (std::holds_alternative<TopicRef>(candidate.item))
        {
            auto right = std::vector<Segment> { { .text = "new branch", .role = Role::Dim } };
            auto const lead = Segment { .text = "✎ topic ", .role = Role::Accent };
            auto const room = width - 1 - view::columnsOf(lead.text) - columnsOf(right) - 1;
            return Line { .left = { marker,
                                    lead,
                                    { .text = cut(std::format("“{}”", candidate.title), room),
                                      .role = textRole } },
                          .right = std::move(right) };
        }
        auto right = std::vector<Segment> {};
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
        {
            right.push_back(glyphOf(entryOf(CiLooks, candidate.ci)));
            right.push_back({ .text = "  " });
            right.push_back(glyphOf(entryOf(ReviewLooks, candidate.review)));
        }
        right.push_back({ .text = "  " });
        right.push_back(candidate.activeRun.empty() ? Segment { .text = " " }
                                                    : Segment { .text = "⬢", .role = Role::Accent });
        auto const label = itemLabel(candidate.item);
        // The marker, the label and a blank, the title, a blank before the columns.
        auto const room = width - 1 - view::columnsOf(label) - 1 - columnsOf(right) - 1;
        return Line { .left = { marker,
                                { .text = label, .role = Role::Dim },
                                { .text = " " },
                                { .text = cut(candidate.title, std::max(room, 4)), .role = textRole } },
                      .right = std::move(right) };
    }

    /// @return What follows `checks failing`: the first failing check, and how many more fail.
    [[nodiscard]] std::string failingText(std::span<std::string const> checks)
    {
        if (checks.empty())
            return {};
        if (checks.size() == 1)
            return std::format(": {}", checks.front());
        return std::format(": {} and {} more", checks.front(), checks.size() - 1);
    }

    /// @return How the settings line shows an item's note: with a cursor while it is typed into.
    [[nodiscard]] std::string noteText(std::string_view note, bool focused)
    {
        if (focused)
            return std::format("{}▏", note);
        if (note.empty())
            return "none";
        return std::string { note };
    }

    /// @return What the launcher says about the highlighted item beyond its row: its checks, its review and
    ///         the agent on it.
    [[nodiscard]] std::string detailOf(Candidate const& candidate, int width)
    {
        if (std::holds_alternative<TopicRef>(candidate.item))
            return "an agent on what you typed, on a new branch";
        auto parts =
            std::vector<std::string> { std::format("{} · {}", itemLabel(candidate.item), candidate.repo) };
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
        {
            if (candidate.ci != CiState::None)
                parts.push_back(std::format("checks {}{}",
                                            entryOf(CiLooks, candidate.ci).word,
                                            failingText(candidate.failingChecks)));
            if (auto const review = entryOf(ReviewLooks, candidate.review).word; !review.empty())
                parts.emplace_back(review);
        }
        if (!candidate.activeRun.empty())
            parts.push_back(std::format("{} works on it", candidate.activeRun));
        auto const views =
            parts | std::views::transform([](std::string const& part) { return std::string_view { part }; })
            | std::ranges::to<std::vector>();
        return joinHints(views, width);
    }

    /// Appends a labelled value, wrapped, continuation lines indented under it.
    void appendValue(
        std::vector<Line>& lines, std::string_view label, std::string_view value, Role role, int width)
    {
        auto const wrapped = wrap(value, width - LabelColumns, width - LabelColumns - ContinuationIndent);
        for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
        {
            auto const lead = index == 0 ? std::format("{:<{}}", label, LabelColumns)
                                         : std::string(LabelColumns + ContinuationIndent, ' ');
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
        auto const isFocused = [&](FormField field) {
            return state.focus == LauncherFocus::Settings && state.setting == field;
        };
        auto const add = [&](std::string_view label, FormField field, std::string value) {
            if (!line.left.empty())
                line.left.push_back({ .text = "   " });
            line.left.push_back({ .text = std::format("{} ", label), .role = Role::Dim });
            line.left.push_back(
                { .text = std::move(value), .role = isFocused(field) ? Role::Selection : Role::Text });
        };
        auto const between = [](std::string_view value) {
            return std::format("‹ {} ›", value);
        };
        for (auto const field: settingsOf(form))
            switch (field)
            {
                case FormField::Repo: add("repo", field, between(form.repo)); break;
                case FormField::Task:
                    add("task",
                        field,
                        between(form.task < tasks.size() ? std::string_view { tasks[form.task].label }
                                                         : "?"sv));
                    break;
                case FormField::Model:
                    add("model",
                        field,
                        between(form.model < models.size() ? std::string_view { models[form.model] }
                                                           : "?"sv));
                    break;
                case FormField::Permissions:
                    add("perms",
                        field,
                        between(form.permissions < profiles.size()
                                    ? std::string_view { profiles[form.permissions].name }
                                    : "?"sv));
                    break;
                case FormField::Note:
                    add("note", field, noteText(form.note, isFocused(field)));
                    if (form.note.starts_with('-'))
                        line.left.back().role = Role::Failing;
                    break;
            }
        return line;
    }

    /// @return The hint line: what Enter does now, then the other keys, cut between hints.
    [[nodiscard]] Line hintLine(LauncherState const& state, bool confirmable, bool incomplete, int width)
    {
        auto hints = std::vector<std::string_view> {};
        // What Enter does while the setup is incomplete, indexed by HooksWarning.
        constexpr auto IncompleteEnterHints = std::array { "enter confirms the warning"sv,
                                                           "enter confirms the warning"sv,
                                                           "enter starts it anyway"sv };
        static_assert(IncompleteEnterHints.size() == std::to_underlying(HooksWarning::Confirmed) + 1U);
        if (confirmable)
            hints.push_back(incomplete ? IncompleteEnterHints[std::to_underlying(state.hooks)]
                                       : "enter start"sv);
        hints.insert(hints.end(), { "↑↓ choose"sv, "tab settings"sv, "ctrl+p more"sv, "esc close"sv });
        return Line { .left = { { .text = joinHints(hints, width), .role = Role::Dim } } };
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
                    : std::min(highlightedAt > rows / 2 ? highlightedAt - (rows / 2) : 0, list.size() - rows);
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
            auto const heading = GroupRows[std::to_underlying(candidate.group)].heading;
            if (!heading.empty() && (index == 0 || candidates[index - 1].group != candidate.group))
                layout.list.push_back(
                    Line { .left = { { .text = std::string { heading }, .role = Role::Dim } } });
            if (index == state.highlighted)
                layout.highlightedAt = layout.list.size();
            layout.list.push_back(candidateLine(candidate, index == state.highlighted, width));
            layout.list.back().candidate = index;
        }

        auto& tail = layout.tail;
        auto rule = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, std::max(width, 0)))
            rule += "─";
        tail.push_back(Line { .left = { { .text = std::move(rule), .role = Role::Border } } });
        tail.push_back(settingsLine(state, engine));
        if (state.highlighted < candidates.size())
            tail.push_back(Line {
                .left = { { .text = detailOf(candidates[state.highlighted], width), .role = Role::Dim } } });
        auto confirmable = false;
        if (state.form)
        {
            auto const preview = previewOf(*state.form, engine);
            confirmable =
                preview && !state.form->note.starts_with('-') && !state.form->topic.starts_with('-');
            if (preview)
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
            if (state.form->note.starts_with('-') || state.form->topic.starts_with('-'))
                tail.push_back(Line { .left = { { .text = std::holds_alternative<TopicRef>(state.form->item)
                                                              ? "a topic may not start with -"
                                                              : "a note may not start with -",
                                                  .role = Role::Failing } } });
        }
        else if (!candidates.empty())
            appendValue(tail, "refused", "no task applies to it", Role::Failing, width);
        auto const warnings = setupWarnings(engine);
        for (auto const& warning: warnings)
            for (auto const& part: wrap(warning, width, width))
                tail.push_back(Line { .left = { { .text = part, .role = Role::Warning } } });
        tail.emplace_back();
        tail.push_back(hintLine(state, confirmable, !warnings.empty(), width));
        return layout;
    }

    // }}}
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
    auto const shown = std::min(lines.size(), static_cast<std::size_t>(std::max(inside.height(), 0)));
    // The popup covers the dashboard; each candidate drawn takes a click.
    auto const hits = HitRecorder { _hits, { .x = screenBounds().x, .y = screenBounds().y } };
    hits.add(canvas.area(), Hit { .kind = HitKind::Popup });
    for (auto const index: std::views::iota(std::size_t { 0 }, shown))
    {
        auto const y = static_cast<int>(index);
        if (lines[index].candidate)
            hits.shifted(1, 1).add(
                core::tui::Rect { .x = 0, .y = y, .width = inside.width(), .height = 1 },
                Hit { .kind = HitKind::LauncherCandidate, .value = *lines[index].candidate });
        auto x = 1;
        for (auto const& segment: lines[index].left)
        {
            // Segments are sanitised already, and their blanks separate them: cutSanitised() keeps them.
            auto const text = cutSanitised(segment.text, width + 1 - x);
            inside.putString(y, x, text, styleOf(theme, segment.role));
            x += view::columnsOf(text);
        }
        auto const rightWidth = columnsOf(lines[index].right);
        if (lines[index].right.empty() || x + 1 + rightWidth > width + 1)
            continue;
        auto rx = width + 1 - rightWidth;
        for (auto const& segment: lines[index].right)
        {
            inside.putString(y, rx, segment.text, styleOf(theme, segment.role));
            rx += view::columnsOf(segment.text);
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
