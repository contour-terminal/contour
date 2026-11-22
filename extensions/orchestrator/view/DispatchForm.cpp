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
    };
    static_assert(RefusalTexts.size() == std::to_underlying(DispatchRefusal::BadPrompt) + 1U,
                  "RefusalTexts has one row per DispatchRefusal");

    /// What Enter does, indexed by HooksWarning: once the warning is confirmed, it asks for a second Enter.
    constexpr auto EnterHints =
        std::array { "enter dispatch"sv, "enter dispatch"sv, "enter again to dispatch"sv };
    static_assert(EnterHints.size() == std::to_underlying(HooksWarning::Confirmed) + 1U);

    /// The form's other keys, in the order the hint line shows them after Enter's.
    constexpr auto OtherHints = std::array { "tab next field"sv, "esc cancel"sv };

    /// Why a note, or a topic, is refused: the agent would read it as an option.
    constexpr auto LeadingDashText = "a note may not start with -"sv;
    constexpr auto TopicLeadingDashText = "a topic may not start with -"sv;

    /// The arguments the command line shows by their placeholder, being long: lines of their own say what
    /// they hold.
    constexpr auto ShownByPlaceholder = std::array { "board"sv, "settings"sv };

    /// @return The agent's command line for @p run as the form shows it: the template substituted, with the
    ///         arguments ShownByPlaceholder names left as their placeholders.
    [[nodiscard]] std::optional<std::vector<std::string>> commandShown(Run const& run)
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

    /// What a topic form says instead of a refusal while its topic is still empty.
    constexpr auto TypeATopicText = "type a topic"sv;

    /// The cells a line's label takes, its padding included.
    constexpr auto LabelColumns = 10;
    /// How far a value's continuation lines are indented past its first.
    constexpr auto ContinuationIndent = 2;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;

    /// @return Whether the agents' setup lacks something a run needs: the status hooks, or a plugin. The form
    ///         then says so, and asks for a second Enter.
    [[nodiscard]] bool isSetupIncomplete(Engine const& engine) noexcept
    {
        return engine.hooks() == HooksState::Missing || !engine.missingPlugins().empty();
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

    // }}}
    // {{{ the lines

    /// One drawn line of the form.
    struct FormLine
    {
        std::string_view label = {}; ///< Its label on a value's first line; empty otherwise.
        std::string text = {};       ///< Sanitised and fitted to its cells.
        Role role = Role::Text;      ///< How it is drawn.
        int start = LabelColumns;    ///< The cells it starts past the box's padding: 0 for a line that is
                                     ///< no field's value.
    };

    /// Appends a value, word-wrapped to the cells it has, continuation lines indented.
    /// @param start Where its first line starts past the box's padding: LabelColumns for a field's value, 0
    /// for
    ///        a line of its own.
    /// @param width The cells the box's padding leaves.
    void appendWrapped(std::vector<FormLine>& lines,
                       std::string_view label,
                       std::string_view value,
                       Role role,
                       int start,
                       int width)
    {
        auto wrapped = wrap(value, width - start, width - start - ContinuationIndent);
        if (wrapped.empty())
            wrapped.emplace_back();
        for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
            lines.push_back(FormLine { .label = index == 0 ? label : ""sv,
                                       .text = std::move(wrapped[index]),
                                       .role = role,
                                       .start = index == 0 ? start : start + ContinuationIndent });
    }

    /// @return @p text with the run's stamp, which is known only once the form is confirmed, shown as `…`.
    [[nodiscard]] std::string withoutStamp(std::string_view text, Run const& run)
    {
        auto const slug = itemSlug(run.item);
        auto const at = text.find(run.id);
        if (!slug || at == std::string_view::npos)
            return std::string { text };
        return std::format("{}{}-…{}", text.substr(0, at), *slug, text.substr(at + run.id.size()));
    }

    /// @return How the form checks the item out into the run's worktree.
    [[nodiscard]] std::string checkoutText(Run const& run)
    {
        auto const branch = withoutStamp(run.branch, run);
        if (auto const* const request = std::get_if<ChangeRequestRef>(&run.item))
            return std::format("gh pr checkout {} --branch {}", request->number, branch);
        return std::format("new branch {}", branch);
    }

    /// @return A field's value between the arrows that say ←/→ change it.
    [[nodiscard]] std::string choice(std::string_view value)
    {
        return std::format("‹ {} ›", value);
    }

    /// @return The task field: its label between the arrows, then the key that opens the form on it.
    [[nodiscard]] std::string taskChoice(TaskDef const& task)
    {
        return std::format("{} {}", choice(task.label), task.key);
    }

    /// @return The role a field's value is drawn in: the selection's while it has the focus.
    [[nodiscard]] Role fieldRole(DispatchFormState const& form,
                                 FormField field,
                                 Role otherwise = Role::Text) noexcept
    {
        return form.focus == field ? Role::Selection : otherwise;
    }

    /// The form's lines above its hint line.
    struct FormBody
    {
        std::vector<FormLine> lines;  ///< Every line, in order.
        std::size_t detailsBegin = 0; ///< The first line of what the dispatch runs: the part cut first.
        std::size_t detailsEnd = 0;   ///< One past its last.
    };

    /// @return The form's lines above its hint line, in @p width cells.
    [[nodiscard]] FormBody bodyLines(DispatchFormState const& form, Engine const& engine, int width)
    {
        auto const columns = width - LabelColumns;
        auto lines = std::vector<FormLine> {};
        auto const* const config = engine.config();
        auto const tasks = config != nullptr ? config->tasks() : std::span<TaskDef const> {};
        auto const models = config != nullptr ? config->models() : std::span<std::string const> {};

        if (isTopicForm(form))
            lines.push_back(FormLine { .label = "repo"sv,
                                       .text = cut(choice(form.repo), columns),
                                       .role = fieldRole(form, FormField::Repo) });
        else
            lines.push_back(
                FormLine { .label = "item"sv,
                           .text = cut(std::format("{} · {}", itemLabel(form.item), form.title), columns) });
        // A topic's text comes before the task: it is what the form is opened for.
        auto const textLine = [&] {
            auto const label = isTopicForm(form) ? "topic"sv : "note"sv;
            auto const noteRole = startsWithDash(form) ? Role::Failing : Role::Text;
            if (form.note.empty() && form.focus != FormField::Note)
                lines.push_back(FormLine { .label = label,
                                           .text = isTopicForm(form)
                                                       ? std::string { TypeATopicText }
                                                       : std::string { "optional; tab to type one" },
                                           .role = isTopicForm(form) ? Role::Warning : Role::Dim });
            else
                appendWrapped(lines,
                              label,
                              form.focus == FormField::Note ? std::format("{}▏", form.note) : form.note,
                              fieldRole(form, FormField::Note, noteRole),
                              LabelColumns,
                              width);
        };
        if (isTopicForm(form))
            textLine();
        lines.push_back(FormLine {
            .label = "task"sv,
            .text = cut(form.task < tasks.size() ? taskChoice(tasks[form.task]) : choice("?"), columns),
            .role = fieldRole(form, FormField::Task) });
        lines.push_back(FormLine {
            .label = "model"sv,
            .text = cut(choice(form.model < models.size() ? std::string_view { models[form.model] } : "?"sv),
                        columns),
            .role = fieldRole(form, FormField::Model) });
        if (!isTopicForm(form))
            textLine();

        auto const detailsBegin = lines.size();
        auto const preview = previewOf(form, engine);
        if (preview)
        {
            appendWrapped(lines, "prompt"sv, preview->run.prompt, Role::Text, LabelColumns, width);
            appendWrapped(lines, "checkout"sv, checkoutText(preview->run), Role::Text, LabelColumns, width);
            appendWrapped(lines,
                          "worktree"sv,
                          withoutStamp(preview->run.worktree, preview->run),
                          Role::Text,
                          LabelColumns,
                          width);
            // The board's instruction is a paragraph: the command line names it by its placeholder, and a
            // line of its own says what it asks for.
            appendWrapped(lines,
                          "board"sv,
                          preview->run.board.empty() ? "none: status_board is off"sv
                                                     : "STATUS.md, asked for in the agent's system prompt"sv,
                          Role::Text,
                          LabelColumns,
                          width);
            // So are the permissions: a JSON document, named by its placeholder, and summed up on its own
            // line.
            appendWrapped(lines,
                          "perms"sv,
                          permissionsText(preview->run.permissions, preview->run.settings),
                          Role::Text,
                          LabelColumns,
                          width);
            if (auto const shown = commandShown(preview->run))
                appendWrapped(lines, "command"sv, shellQuoted(*shown), Role::Dim, LabelColumns, width);
        }
        else if (isTopicForm(form) && form.note.empty())
            appendWrapped(lines, "refused"sv, TypeATopicText, Role::Dim, LabelColumns, width);
        else
            appendWrapped(
                lines, "refused"sv, refusalText(preview.error()), Role::Failing, LabelColumns, width);

        auto const detailsEnd = lines.size();
        lines.emplace_back();
        if (startsWithDash(form))
            lines.push_back(
                FormLine { .text = std::string { isTopicForm(form) ? TopicLeadingDashText : LeadingDashText },
                           .role = Role::Failing,
                           .start = 0 });
        if (config != nullptr)
            lines.push_back(
                FormLine { .text = std::format("{} of {} running", engine.activeRuns(), config->maxAgents()),
                           .role = Role::Dim,
                           .start = 0 });
        if (engine.hooks() == HooksState::Missing)
            appendWrapped(lines, ""sv, HooksMissingWarning, Role::Warning, 0, width);
        for (auto const& plugin: engine.missingPlugins())
            appendWrapped(lines,
                          ""sv,
                          std::format("Claude Code lacks the plugin {}: a prompt naming its skills fails "
                                      "(I on the dashboard installs it)",
                                      plugin.id),
                          Role::Warning,
                          0,
                          width);
        return FormBody { .lines = std::move(lines), .detailsBegin = detailsBegin, .detailsEnd = detailsEnd };
    }

    /// @return The lines of @p body that fit @p room rows: what the dispatch runs is cut first, from its end,
    ///         so that the fields, the limits and a warning stay; a cut part's last line ends in `…`.
    [[nodiscard]] std::vector<FormLine> fitted(FormBody body, std::size_t room)
    {
        auto& lines = body.lines;
        if (lines.size() <= room)
            return std::move(lines);
        auto const details = body.detailsEnd - body.detailsBegin;
        auto const dropped = std::min(lines.size() - room, details > 0 ? details - 1 : 0);
        if (dropped > 0)
        {
            auto const keptEnd = body.detailsEnd - dropped;
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keptEnd),
                        lines.begin() + static_cast<std::ptrdiff_t>(body.detailsEnd));
            lines[keptEnd - 1].text += "…";
        }
        lines.resize(std::min(lines.size(), room));
        return std::move(lines);
    }

    /// @return Whether Enter does something now: not while the note starts with `-` or the engine would
    /// refuse.
    [[nodiscard]] bool enterActs(DispatchFormState const& form, Engine const& engine)
    {
        return !startsWithDash(form) && previewOf(form, engine).has_value();
    }

    /// @return The hint line in @p columns cells: what Enter does, while it does something, then the form's
    ///         other keys.
    [[nodiscard]] std::string hintLine(DispatchFormState const& form, Engine const& engine, int columns)
    {
        auto hints = std::vector<std::string_view> {};
        if (enterActs(form, engine))
            hints.push_back(EnterHints[std::to_underlying(form.hooks)]);
        hints.insert(hints.end(), OtherHints.begin(), OtherHints.end());
        return joinHints(hints, columns);
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
            // A task comes with its own model.
            auto const models = config->models();
            if (auto const model = std::ranges::find(models, tasks[state.task].model); model != models.end())
                state.model = static_cast<std::size_t>(std::ranges::distance(models.begin(), model));
        }
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

void DispatchFormView::render(core::tui::Canvas& canvas)
{
    if (!_form)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "Dispatch an agent");
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - SideColumns;
    // The hint line stays on the last row; the body is cut to fit above it.
    auto const room = static_cast<std::size_t>(std::max(0, inside.height() - 1));
    auto const lines = fitted(bodyLines(*_form, _engine, width), room);
    for (auto const index: std::views::iota(std::size_t { 0 }, lines.size()))
    {
        auto const& line = lines[index];
        auto const y = static_cast<int>(index);
        auto const text = cut(line.text, width - line.start);
        auto const x = 1 + line.start;
        inside.putString(y, 1, line.label, styleOf(theme, Role::Dim));
        inside.putString(y, x, text, styleOf(theme, line.role));
    }
    if (inside.height() > 0)
        inside.putString(inside.height() - 1, 1, hintLine(*_form, _engine, width), styleOf(theme, Role::Dim));
}

int DispatchFormView::heightAt(int width) const
{
    if (!_form)
        return 0;
    auto const inner = width - 2 - SideColumns;
    // The box's two rows and the hint line.
    return static_cast<int>(bodyLines(*_form, _engine, inner).lines.size()) + 3;
}

} // namespace orchestrator::view
