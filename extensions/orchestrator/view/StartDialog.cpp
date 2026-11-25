// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/StartDialog.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
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
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/NavInput.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/TextEdit.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

namespace
{
    using namespace std::string_literals;
    using namespace std::string_view_literals;

    // {{{ the state

    /// The settings Tab moves through, in order.
    constexpr auto SettingOrder = std::array {
        FormField::Task, FormField::Worker, FormField::Model, FormField::Permissions, FormField::Note
    };

    /// @return Whether the form's worker takes a permission profile; true while there is no configuration.
    [[nodiscard]] bool takesProfile(DispatchFormState const& form, Engine const& engine) noexcept
    {
        auto const* const worker = workerOf(form, engine);
        return worker == nullptr || agentRowOf(worker->agent).settings == SettingsProfile::Taken;
    }

    /// @return Whether Tab stops at @p field: not at the profile of a worker that takes none.
    [[nodiscard]] bool isOffered(FormField field,
                                 DispatchFormState const& form,
                                 Engine const& engine) noexcept
    {
        return field != FormField::Permissions || takesProfile(form, engine);
    }

    /// What a step past the first or the last setting does.
    enum class AtTheEnds : uint8_t
    {
        Stop = 0, ///< It stays there, as the navigation keys do.
        Wrap,     ///< It goes round to the other end, as Tab does.
    };

    /// @return The setting after (or before, by @p step) @p from in SettingOrder, passing over the ones not
    ///         offered; past an end as @p ends says.
    [[nodiscard]] FormField nextSetting(FormField from,
                                        int step,
                                        AtTheEnds ends,
                                        DispatchFormState const& form,
                                        Engine const& engine) noexcept
    {
        auto const count = SettingOrder.size();
        auto const found = std::ranges::find(SettingOrder, from);
        auto index = static_cast<std::size_t>(std::ranges::distance(SettingOrder.begin(), found)) % count;
        for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 0 }, count))
        {
            auto const atEnd = step > 0 ? index + 1 == count : index == 0;
            if (atEnd && ends == AtTheEnds::Stop)
                return from;
            index = step > 0 ? (index + 1) % count : (index + count - 1) % count;
            if (isOffered(SettingOrder[index], form, engine))
                return SettingOrder[index];
        }
        return from;
    }

    /// Moves to @p setting by a navigation key: a move off the note stops typing it; one onto it only
    /// selects it.
    void moveTo(StartDialogState& dialog, FormField setting) noexcept
    {
        if (setting != dialog.setting)
            dialog.mode = FieldMode::Navigating;
        dialog.setting = setting;
    }

    /// Confirms the dialog: the dispatch of its choices, unless they cannot be confirmed, or the setup is
    /// incomplete and its warning was not confirmed by an earlier input.
    [[nodiscard]] std::optional<DispatchAction> confirm(StartDialogState& dialog,
                                                        Engine const& engine,
                                                        HooksWarning hooksAtInput,
                                                        std::string_view nearTab)
    {
        if (!canConfirm(dialog.form, engine)
            || passSetupWarning(dialog.form, engine, dialog.hooks, hooksAtInput) == SetupGate::Held)
            return std::nullopt;
        return actionOf(dialog.form, engine, nearTab);
    }

    /// The url and title of a Work board row: its merge request's, else its ticket's.
    struct Listed
    {
        std::string url;
        std::string title;
    };

    [[nodiscard]] Listed listedOf(WorkItem const& item)
    {
        if (item.changeRequest)
            return Listed { .url = item.changeRequest->url, .title = item.changeRequest->title };
        if (item.ticket)
            return Listed { .url = item.ticket->url, .title = item.ticket->title };
        return {};
    }

    // }}}
} // namespace

std::string startRefusalNotice(StartRefusal refusal, std::string_view label)
{
    // Each formats with a literal: a run-time format string would throw on a malformed one.
    switch (refusal)
    {
        case StartRefusal::NotConfigured: return "the orchestrator is not configured"s;
        case StartRefusal::NotListed: return std::format("{} is not listed any more", label);
        case StartRefusal::NoTask: return "no task applies to this item"s;
    }
    return {};
}

std::expected<StartDialogState, StartRefusal> openStartDialog(Engine const& engine,
                                                              WorkItem const& item,
                                                              std::string_view task,
                                                              std::string session)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return std::unexpected(StartRefusal::NotConfigured);
    auto const key = workItemKey(item);
    if (!std::ranges::any_of(engine.workItems(),
                             [&](WorkItem const& listed) { return workItemKey(listed) == key; }))
        return std::unexpected(StartRefusal::NotListed);

    // A task asked for, while it applies, else the one the row's state suggests.
    auto suggestion = suggestTask(*config, item);
    if (auto const* const asked = config->findTask(task);
        asked != nullptr && asked->appliesTo == appliesToOf(item.item) && asked != suggestion.task)
        suggestion = TaskSuggestion { .task = asked, .reason = "the task it ran with"s };
    if (suggestion.task == nullptr)
        return std::unexpected(StartRefusal::NoTask);

    auto listed = listedOf(item);
    auto form = openItemForm(engine,
                             item.repo,
                             item.item,
                             std::move(listed.url),
                             std::move(listed.title),
                             suggestion.task->id,
                             std::move(session));
    if (!form)
        return std::unexpected(StartRefusal::NoTask);
    auto dialog = StartDialogState { .form = std::move(*form), .reason = std::move(suggestion.reason) };
    dialog.hooks = setupWarningOf(dialog.form, engine);
    return dialog;
}

void pasteIntoStartDialog(std::optional<StartDialogState>& dialog, std::string_view text)
{
    if (dialog)
        dialog->form.note += text;
}

void pickStartDialogSetting(StartDialogState& dialog, FormField setting) noexcept
{
    dialog.setting = setting;
    dialog.mode = setting == FormField::Note ? FieldMode::Typing : FieldMode::Navigating;
}

namespace
{
    /// Applies a navigation action to the open dialog.
    /// @return The dispatch, when the action confirmed it.
    [[nodiscard]] std::optional<DispatchAction> applyNavAction(NavAction action,
                                                               core::tui::KeyEvent const& key,
                                                               std::optional<StartDialogState>& dialog,
                                                               Engine const& engine,
                                                               HooksWarning hooksAtInput,
                                                               std::string_view nearTab)
    {
        auto& state = *dialog;
        switch (action)
        {
            case NavAction::MoveUp:
            case NavAction::MoveDown:
                moveTo(state,
                       nextSetting(state.setting,
                                   action == NavAction::MoveDown ? 1 : -1,
                                   AtTheEnds::Stop,
                                   state.form,
                                   engine));
                break;
            case NavAction::First: moveTo(state, SettingOrder.front()); break;
            case NavAction::Last: moveTo(state, SettingOrder.back()); break;
            case NavAction::PreviousValue:
            case NavAction::NextValue:
                changeSetting(state.form, state.setting, action == NavAction::NextValue ? 1 : -1, engine);
                break;
            case NavAction::EnterField:
                // `⏎` types the note only on its line; elsewhere it confirms, as it always has.
                if (key.key == core::tui::KeyCode::Enter && state.setting != FormField::Note)
                {
                    auto dispatch = confirm(state, engine, hooksAtInput, nearTab);
                    if (dispatch)
                        dialog.reset();
                    return dispatch;
                }
                pickStartDialogSetting(state, FormField::Note);
                break;
            case NavAction::LeaveField: state.mode = FieldMode::Navigating; break;
            case NavAction::Close: dialog.reset(); break;
            case NavAction::HalfPageUp:
            case NavAction::HalfPageDown: break;
        }
        return std::nullopt;
    }
} // namespace

std::optional<DispatchAction> applyStartDialogKey(core::tui::KeyEvent const& key,
                                                  std::optional<NavAction> nav,
                                                  std::optional<StartDialogState>& dialog,
                                                  Engine const& engine,
                                                  HooksWarning hooksAtInput,
                                                  std::string_view nearTab)
{
    if (!dialog)
        return std::nullopt;
    auto& state = *dialog;
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto const typing = state.mode == FieldMode::Typing;
    auto const& keys = keysInForce(engine);

    if (nav == NavAction::Close)
    {
        dialog.reset();
        return std::nullopt;
    }
    // The line-editing keys come first while typing: Alt+Backspace drops a word rather than reading as Esc.
    if (auto const edit = textEditOf(key); edit && typing)
    {
        applyTextEdit(state.form.note, *edit);
        return std::nullopt;
    }
    // A key with Alt is how an Esc reads when the next key follows it in the same read: it does what Esc
    // does there (Decision 11).
    if (core::tui::hasModifier(key.modifiers, Modifier::Alt))
    {
        auto const asEsc = keys.actionFor("esc", typing ? NavContext::Typing : NavContext::Navigating);
        return asEsc ? applyNavAction(*asEsc, key, dialog, engine, hooksAtInput, nearTab) : std::nullopt;
    }
    if (isCtrl(key, U'p'))
    {
        state.preview = state.preview == DialogPreview::Short ? DialogPreview::Full : DialogPreview::Short;
        return std::nullopt;
    }
    if (nav)
        return applyNavAction(*nav, key, dialog, engine, hooksAtInput, nearTab);
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
        return std::nullopt;

    switch (key.key)
    {
        case KeyCode::Tab:
            pickStartDialogSetting(
                state,
                nextSetting(state.setting,
                            core::tui::hasModifier(key.modifiers, Modifier::Shift) ? -1 : 1,
                            AtTheEnds::Wrap,
                            state.form,
                            engine));
            return std::nullopt;
        case KeyCode::Enter: {
            auto action = confirm(state, engine, hooksAtInput, nearTab);
            if (action)
                dialog.reset();
            return action;
        }
        default: break;
    }
    if (!typing)
        return std::nullopt;
    if (auto const printable = printableOf(key))
    {
        appendUtf8(state.form.note, *printable);
        return std::nullopt;
    }
    // A plain special key the note does not take acts as it would with nothing typed.
    if (auto const passed = passedKeyAction(key, keys))
        return applyNavAction(*passed, key, dialog, engine, hooksAtInput, nearTab);
    return std::nullopt;
}

namespace
{
    // {{{ drawing

    /// One run of text drawn in one role, and what a click on it lands on.
    struct Segment
    {
        std::string text;
        Role role = Role::Text;
        std::optional<Hit> hit = std::nullopt; ///< What it records; none: nothing of its own.
    };

    /// One drawn line: segments from the left.
    using Line = std::vector<Segment>;

    /// The cells a setting's label takes.
    constexpr auto LabelColumns = 10;
    /// How far a value's continuation lines are indented past its first.
    constexpr auto ContinuationIndent = 2;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;

    [[nodiscard]] int columnsOf(Line const& line)
    {
        auto columns = 0;
        for (auto const& segment: line)
            columns += view::columnsOf(segment.text);
        return columns;
    }

    [[nodiscard]] Segment labelOf(std::string_view label)
    {
        return Segment { .text = std::format("{:<{}}", label, LabelColumns), .role = Role::Dim };
    }

    /// @return A setting's label: a click on it picks the setting.
    [[nodiscard]] Segment settingLabel(std::string_view label, FormField field)
    {
        auto segment = labelOf(label);
        segment.hit = Hit { .kind = HitKind::DialogSetting, .value = std::to_underlying(field) };
        return segment;
    }

    [[nodiscard]] Segment blankLabel(int indent = 0)
    {
        return Segment { .text = std::string(static_cast<std::size_t>(LabelColumns + indent), ' ') };
    }

    /// Appends a labelled value, wrapped, continuation lines indented under it.
    void appendValue(
        std::vector<Line>& lines, std::string_view label, std::string_view value, Role role, int width)
    {
        auto const wrapped = wrap(value, width - LabelColumns, width - LabelColumns - ContinuationIndent);
        for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
            lines.push_back(Line { index == 0 ? labelOf(label) : blankLabel(ContinuationIndent),
                                   Segment { .text = wrapped[index], .role = role } });
    }

    /// Appends text of its own, wrapped to the width.
    void appendText(std::vector<Line>& lines, std::string_view text, Role role, int width)
    {
        for (auto const& part: wrap(text, width, width))
            lines.push_back(Line { Segment { .text = part, .role = role } });
    }

    /// @return How the note shows: with a cursor while it is typed into.
    [[nodiscard]] std::string noteText(std::string_view note, bool focused)
    {
        if (focused)
            return std::format("{}▏", note);
        if (note.empty())
            return "none";
        return std::string { note };
    }

    /// Appends a stepped setting's value to @p line: between arrows, selected while it has the keys; a click
    /// on the first arrow steps it back, one on the value or the second arrow steps it on.
    void appendStepped(Line& line, std::string_view value, FormField field, bool focused)
    {
        auto const role = focused ? Role::Selection : Role::Text;
        auto const step = [field](std::size_t forward) {
            return Hit { .kind = HitKind::DialogStep, .value = (2 * std::to_underlying(field)) + forward };
        };
        line.push_back(Segment { .text = "‹ ", .role = role, .hit = step(0) });
        line.push_back(Segment { .text = std::format("{} ›", value), .role = role, .hit = step(1) });
    }

    /// @return A worker's chip: its id and active runs, and what keeps it from a plain start.
    [[nodiscard]] std::string chipText(Engine const& engine,
                                       Config const& config,
                                       TaskDef const* task,
                                       WorkerDef const& worker)
    {
        auto text = std::format("{} {}", worker.id, activeRunsOn(config, engine.runs(), worker.id));
        if (isSetupIncomplete(engine, worker))
            text += " (setup)";
        if (task != nullptr && takesNoProfileFor(*task, worker))
            text += " (no profile)";
        return text;
    }

    /// Appends the Worker setting: every worker as a chip, wrapped under its label; the chosen one in the
    /// selection's style while the setting has the keys, else in the accent's.
    void appendWorkers(std::vector<Line>& lines,
                       StartDialogState const& dialog,
                       Engine const& engine,
                       Config const& config,
                       int width)
    {
        auto const tasks = config.tasks();
        auto const* const task = dialog.form.task < tasks.size() ? &tasks[dialog.form.task] : nullptr;
        auto const workers = config.workers();
        auto line = Line { settingLabel("worker", FormField::Worker) };
        for (auto const index: std::views::iota(std::size_t { 0 }, workers.size()))
        {
            auto const chosen = index == dialog.form.worker;
            auto chip = Segment { .text = std::format(" {} ", chipText(engine, config, task, workers[index])),
                                  .role = !chosen                               ? Role::Dim
                                          : dialog.setting == FormField::Worker ? Role::Selection
                                                                                : Role::Accent,
                                  .hit = Hit { .kind = HitKind::WorkerChip, .value = index } };
            auto const room = width - columnsOf(line) - (line.size() > 1 ? 1 : 0);
            if (line.size() > 1 && view::columnsOf(chip.text) > room)
            {
                lines.push_back(std::move(line));
                line = Line { blankLabel() };
            }
            if (line.size() > 1)
                line.push_back(Segment { .text = " " });
            line.push_back(std::move(chip));
        }
        lines.push_back(std::move(line));
    }

    /// @return The hint line: what Enter does now, then the other keys, each named by its first key in force
    ///         (a hint whose key is unbound is left out), cut between hints.
    [[nodiscard]] Line hintLine(StartDialogState const& dialog,
                                Engine const& engine,
                                bool confirmable,
                                int width)
    {
        auto const& keys = keysInForce(engine);
        auto const label = [&](NavAction action) {
            return navKeyLabel(keys, action);
        };
        auto hints = std::vector<std::string> {};
        auto const add = [&](std::string const& key, std::string_view does) {
            if (!key.empty())
                hints.push_back(std::format("{} {}", key, does));
        };
        auto const typing = dialog.mode == FieldMode::Typing;
        // `⏎` on the Note line types it, when it is an enter_field key there.
        if (!typing && dialog.setting == FormField::Note
            && std::ranges::contains(keys.keysOf(NavAction::EnterField), "enter"sv))
            add(std::string { shownKeyName("enter") }, "note"sv);
        else if (confirmable)
            hints.emplace_back(enterHint(dialog.form, engine, dialog.hooks));
        if (typing)
            add(label(NavAction::LeaveField), "settings"sv);
        else
        {
            add(label(NavAction::MoveUp) + label(NavAction::MoveDown), "settings"sv);
            add(label(NavAction::PreviousValue) + label(NavAction::NextValue), "change"sv);
            add(label(NavAction::EnterField), "note"sv);
        }
        hints.emplace_back(dialog.preview == DialogPreview::Short ? "ctrl+p more"sv : "ctrl+p less"sv);
        if (!typing)
            add(label(NavAction::Close), "close"sv);
        auto const views = std::vector<std::string_view> { hints.begin(), hints.end() };
        return Line { Segment { .text = joinHints(views, width), .role = Role::Dim } };
    }

} // namespace

/// The dialog's lines at one width.
struct StartDialogLayout
{
    std::vector<Line> lines = {};
};

namespace
{
    [[nodiscard]] StartDialogLayout layoutOf(StartDialogState const& dialog, Engine const& engine, int width)
    {
        auto layout = StartDialogLayout {};
        auto& lines = layout.lines;
        auto const& form = dialog.form;
        auto const* const config = engine.config();

        // The item: its label, its repository and its title.
        auto const head = std::format("{} · {} · ", itemLabel(form.item), form.repo);
        lines.push_back(
            Line { Segment { .text = head, .role = Role::Dim },
                   Segment { .text = cut(form.title, std::max(width - view::columnsOf(head), 4)) } });
        lines.emplace_back();

        if (config != nullptr)
        {
            auto const tasks = config->tasks();
            auto const models = config->models();
            auto const profiles = config->profiles();
            auto const isFocused = [&](FormField field) {
                return dialog.setting == field;
            };

            // The task, and why it was suggested: beside it where it fits, else under it.
            auto task = Line { settingLabel("task", FormField::Task) };
            appendStepped(task,
                          form.task < tasks.size() ? std::string_view { tasks[form.task].label } : "?"sv,
                          FormField::Task,
                          isFocused(FormField::Task));
            auto const reason = Segment { .text = std::format("  {}", dialog.reason), .role = Role::Dim };
            auto const besides = columnsOf(task) + view::columnsOf(reason.text) <= width;
            if (!dialog.reason.empty() && besides)
                task.push_back(reason);
            lines.push_back(std::move(task));
            if (!dialog.reason.empty() && !besides)
                for (auto const& part: wrap(dialog.reason, width - LabelColumns, width - LabelColumns))
                    lines.push_back(Line { blankLabel(), Segment { .text = part, .role = Role::Dim } });

            appendWorkers(lines, dialog, engine, *config, width);
            auto model = Line { settingLabel("model", FormField::Model) };
            appendStepped(model,
                          form.model < models.size() ? std::string_view { models[form.model] } : "?"sv,
                          FormField::Model,
                          isFocused(FormField::Model));
            lines.push_back(std::move(model));
            // A worker that takes no profile runs without one: there is nothing to choose.
            if (takesProfile(form, engine))
            {
                auto perms = Line { settingLabel("perms", FormField::Permissions) };
                appendStepped(perms,
                              form.permissions < profiles.size()
                                  ? std::string_view { profiles[form.permissions].name }
                                  : "?"sv,
                              FormField::Permissions,
                              isFocused(FormField::Permissions));
                lines.push_back(std::move(perms));
            }
            else
                lines.push_back(Line { labelOf("perms"), Segment { .text = "none", .role = Role::Dim } });
            auto note = Line { settingLabel("note", FormField::Note),
                               Segment { .text = cut(noteText(form.note, dialog.mode == FieldMode::Typing),
                                                     std::max(width - LabelColumns, 4)),
                                         .role = isFocused(FormField::Note) ? Role::Selection : Role::Text,
                                         .hit = Hit { .kind = HitKind::DialogSetting,
                                                      .value = std::to_underlying(FormField::Note) } } };
            if (form.note.starts_with('-'))
                note.back().role = Role::Failing;
            lines.push_back(std::move(note));
        }

        auto rule = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, std::max(width, 0)))
            rule += "─";
        lines.push_back(Line { Segment { .text = std::move(rule), .role = Role::Border } });

        // What it runs, or why the engine would refuse it.
        auto const preview = previewOf(form, engine);
        auto const confirmable = preview && !form.note.starts_with('-');
        if (preview)
        {
            appendValue(lines, "checkout", checkoutText(preview->run), Role::Text, width);
            appendValue(
                lines, "worktree", withoutStamp(preview->run.worktree, preview->run), Role::Text, width);
            if (dialog.preview == DialogPreview::Full)
            {
                appendValue(lines, "prompt", preview->run.prompt, Role::Text, width);
                if (auto const shown = commandShown(preview->run))
                    appendValue(lines, "command", shellQuoted(*shown), Role::Dim, width);
            }
        }
        else
            appendValue(lines, "refused", refusalText(preview.error()), Role::Failing, width);
        if (form.note.starts_with('-'))
            appendText(lines, "a note may not start with -", Role::Failing, width);
        // The agent on the item, which the engine's refusal above stands for.
        if (auto const* const run = engine.activeRunOn(form.repo, form.item))
            appendText(
                lines, std::format("⬢ {} already works on this", workerIdOf(*run)), Role::Accent, width);
        auto const* const worker = workerOf(form, engine);
        auto const warnings =
            worker != nullptr ? setupWarnings(engine, *worker) : std::vector<std::string> {};
        for (auto const& warning: warnings)
            appendText(lines, warning, Role::Warning, width);
        lines.emplace_back();
        lines.push_back(hintLine(dialog, engine, confirmable, width));
        return layout;
    }

    // }}}
} // namespace

void StartDialogView::render(core::tui::Canvas& canvas)
{
    if (!_dialog)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "▶ Start agent");
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - SideColumns;
    auto const& lines = layoutAt(width).lines;
    // The popup covers the dashboard.
    auto const hits = HitRecorder { _hits, { .x = screenBounds().x, .y = screenBounds().y } };
    hits.add(canvas.area(), Hit { .kind = HitKind::Popup });
    auto const insideHits = hits.shifted(1, 1);
    auto const shown = std::min(lines.size(), static_cast<std::size_t>(std::max(inside.height(), 0)));
    for (auto const index: std::views::iota(std::size_t { 0 }, shown))
    {
        auto x = 1;
        for (auto const& segment: lines[index])
        {
            // Segments are sanitised already, and their blanks separate them: cutSanitised() keeps them.
            auto const text = cutSanitised(segment.text, width + 1 - x);
            auto const columns = view::columnsOf(text);
            inside.putString(static_cast<int>(index), x, text, styleOf(theme, segment.role));
            if (segment.hit)
                insideHits.add(
                    core::tui::Rect { .x = x, .y = static_cast<int>(index), .width = columns, .height = 1 },
                    *segment.hit);
            x += columns;
        }
    }
}

int StartDialogView::heightAt(int width) const
{
    if (!_dialog)
        return 0;
    // The box's two rows around its lines.
    return static_cast<int>(layoutAt(width - 2 - SideColumns).lines.size()) + 2;
}

/// The last layout the dialog drew, and what it was built from.
struct StartDialogView::LayoutCache
{
    StartDialogState state;
    uint64_t revision = 0;
    int width = 0;
    StartDialogLayout layout;
};

StartDialogView::StartDialogView(Engine const& engine,
                                 std::optional<StartDialogState> const& dialog,
                                 HitMap& hits):
    _engine { engine }, _dialog { dialog }, _hits { hits }
{
}

StartDialogView::~StartDialogView() = default;

StartDialogLayout const& StartDialogView::layoutAt(int width) const
{
    // A frame asks for the height and then draws at it: one layout serves both, and the frames that change
    // neither the dialog, nor the engine, nor the width.
    if (!_cache || _cache->width != width || _cache->revision != _engine.revision()
        || _cache->state != *_dialog)
        _cache = std::make_unique<LayoutCache>(LayoutCache { .state = *_dialog,
                                                             .revision = _engine.revision(),
                                                             .width = width,
                                                             .layout = layoutOf(*_dialog, _engine, width) });
    return _cache->layout;
}

} // namespace orchestrator::view
