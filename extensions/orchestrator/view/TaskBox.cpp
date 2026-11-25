// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/TaskBox.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/NavInput.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/TextEdit.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

namespace
{
    using namespace std::string_view_literals;

    // {{{ the state

    /// One place the box's keys go, in Tab's order.
    struct FieldRow
    {
        TaskBoxField field = TaskBoxField::Text; ///< The place.
        std::optional<FormField> setting = {};   ///< What ←/→ step there; none: the text.
        std::string_view label = {};             ///< Its label; empty: the text's.
    };

    constexpr auto FieldRows = std::array {
        FieldRow { .field = TaskBoxField::Text, .setting = std::nullopt, .label = {} },
        FieldRow { .field = TaskBoxField::Repo, .setting = FormField::Repo, .label = "repo"sv },
        FieldRow { .field = TaskBoxField::Worker, .setting = FormField::Worker, .label = "worker"sv },
        FieldRow { .field = TaskBoxField::Model, .setting = FormField::Model, .label = "model"sv },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, FieldRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(FieldRows[index].field) == index;
                                      }),
                  "FieldRows is indexed by the TaskBoxField's value, in Tab's order");

    /// @return The place after (or before, by @p step) @p from, wrapping around.
    [[nodiscard]] TaskBoxField nextField(TaskBoxField from, int step) noexcept
    {
        auto const count = FieldRows.size();
        auto const index = std::to_underlying(from);
        return FieldRows[step > 0 ? (index + 1) % count : (index + count - 1) % count].field;
    }

    /// @return The place after (or before, by @p step) @p from, staying at either end.
    [[nodiscard]] TaskBoxField steppedField(TaskBoxField from, int step) noexcept
    {
        auto const index = std::to_underlying(from);
        if (step > 0)
            return index + 1 < FieldRows.size() ? FieldRows[index + 1].field : from;
        return index > 0 ? FieldRows[index - 1].field : from;
    }

    /// Moves to @p field by a navigation key: a move off the text stops typing it; one onto it only selects
    /// it.
    void moveTo(TaskBoxState& box, TaskBoxField field) noexcept
    {
        if (field != box.focus)
            box.mode = FieldMode::Navigating;
        box.focus = field;
    }

    /// @return Whether @p text says nothing: empty, or blanks only.
    [[nodiscard]] bool isBlank(std::string_view text) noexcept
    {
        return std::ranges::all_of(text, [](char c) { return c == ' '; });
    }

    /// Confirms the box: the dispatch of its text, unless it is blank, cannot be confirmed, or the setup is
    /// incomplete and its warning was not confirmed by an earlier input.
    [[nodiscard]] std::optional<DispatchAction> confirm(TaskBoxState& box,
                                                        Engine const& engine,
                                                        HooksWarning hooksAtInput,
                                                        std::string_view nearTab)
    {
        if (isBlank(box.text))
            return std::nullopt;
        auto const form = formOf(box);
        if (!canConfirm(form, engine)
            || passSetupWarning(form, engine, box.hooks, hooksAtInput) == SetupGate::Held)
            return std::nullopt;
        return actionOf(form, engine, nearTab);
    }

    // }}}
} // namespace

std::optional<FormField> taskBoxSettingOf(TaskBoxField field) noexcept
{
    auto const index = std::to_underlying(field);
    return index < FieldRows.size() ? FieldRows[index].setting : std::nullopt;
}

std::expected<TaskBoxState, StartRefusal> openTaskBox(Engine const& engine,
                                                      std::string_view chipRepo,
                                                      std::string_view selectedRepo,
                                                      std::string text,
                                                      std::string session)
{
    auto const* const config = engine.config();
    if (config == nullptr || config->repos().empty())
        return std::unexpected(StartRefusal::NotConfigured);
    // The chip's repository, else the selected row's; openTopicForm() starts one the configuration lacks on
    // the first.
    auto const repo = config->findRepo(chipRepo) != nullptr ? chipRepo : selectedRepo;
    auto form = openTopicForm(engine, repo, {}, std::move(session));
    if (!form)
        return std::unexpected(StartRefusal::NotConfigured);
    auto box = TaskBoxState { .text = std::move(text), .form = std::move(*form) };
    box.hooks = setupWarningOf(box.form, engine);
    return box;
}

DispatchFormState formOf(TaskBoxState const& box)
{
    auto form = box.form;
    form.topic = box.text;
    return form;
}

void pasteIntoTaskBox(std::optional<TaskBoxState>& box, std::string_view text)
{
    if (box)
        box->text += text;
}

void pickTaskBoxField(TaskBoxState& box, TaskBoxField field) noexcept
{
    box.focus = field;
    box.mode = field == TaskBoxField::Text ? FieldMode::Typing : FieldMode::Navigating;
}

namespace
{
    /// Applies a navigation action to the open box.
    /// @return The dispatch, when the action confirmed it.
    [[nodiscard]] std::optional<DispatchAction> applyNavAction(NavAction action,
                                                               core::tui::KeyEvent const& key,
                                                               std::optional<TaskBoxState>& box,
                                                               Engine const& engine,
                                                               HooksWarning hooksAtInput,
                                                               std::string_view nearTab)
    {
        auto& state = *box;
        switch (action)
        {
            case NavAction::MoveUp:
            case NavAction::MoveDown:
                moveTo(state, steppedField(state.focus, action == NavAction::MoveDown ? 1 : -1));
                break;
            case NavAction::First: moveTo(state, FieldRows.front().field); break;
            case NavAction::Last: moveTo(state, FieldRows.back().field); break;
            case NavAction::PreviousValue:
            case NavAction::NextValue:
                if (auto const setting = FieldRows[std::to_underlying(state.focus)].setting)
                    changeSetting(state.form, *setting, action == NavAction::NextValue ? 1 : -1, engine);
                break;
            case NavAction::EnterField:
                // `⏎` types the text only on its line; elsewhere it starts, as it always has.
                if (key.key == core::tui::KeyCode::Enter && state.focus != TaskBoxField::Text)
                {
                    auto dispatch = confirm(state, engine, hooksAtInput, nearTab);
                    if (dispatch)
                        box.reset();
                    return dispatch;
                }
                pickTaskBoxField(state, TaskBoxField::Text);
                break;
            // The settings are what the keys go to once the text is left.
            case NavAction::LeaveField: pickTaskBoxField(state, TaskBoxField::Repo); break;
            case NavAction::Close: box.reset(); break;
            case NavAction::HalfPageUp:
            case NavAction::HalfPageDown: break;
        }
        return std::nullopt;
    }
} // namespace

std::optional<DispatchAction> applyTaskBoxKey(core::tui::KeyEvent const& key,
                                              std::optional<NavAction> nav,
                                              std::optional<TaskBoxState>& box,
                                              Engine const& engine,
                                              HooksWarning hooksAtInput,
                                              std::string_view nearTab)
{
    if (!box)
        return std::nullopt;
    auto& state = *box;
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto const typing = state.mode == FieldMode::Typing;
    auto const& keys = keysInForce(engine);

    if (nav == NavAction::Close)
    {
        box.reset();
        return std::nullopt;
    }
    // The line-editing keys come first: Alt+Backspace drops a word rather than reading as Esc. While
    // navigating there is no text to edit, and the key does nothing unless a navigation action holds it.
    if (auto const edit = textEditOf(key); edit && (typing || !nav))
    {
        if (typing)
            applyTextEdit(state.text, *edit);
        return std::nullopt;
    }
    // A key with Alt is how an Esc reads when the next key follows it in the same read: it does what Esc
    // does there (Decision 11).
    if (core::tui::hasModifier(key.modifiers, Modifier::Alt))
    {
        if (auto const asEsc = keys.actionFor("esc", typing ? NavContext::Typing : NavContext::Navigating))
            return applyNavAction(*asEsc, key, box, engine, hooksAtInput, nearTab);
        if (!typing)
            box.reset();
        return std::nullopt;
    }
    if (nav)
        return applyNavAction(*nav, key, box, engine, hooksAtInput, nearTab);
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
        return std::nullopt;

    switch (key.key)
    {
        // Esc closes it while navigating even where `close` is bound elsewhere, as it closes every popup.
        case KeyCode::Escape:
            if (!typing)
            {
                box.reset();
                return std::nullopt;
            }
            break;
        case KeyCode::Tab:
            pickTaskBoxField(
                state,
                nextField(state.focus, core::tui::hasModifier(key.modifiers, Modifier::Shift) ? -1 : 1));
            return std::nullopt;
        case KeyCode::Enter: {
            auto action = confirm(state, engine, hooksAtInput, nearTab);
            if (action)
                box.reset();
            return action;
        }
        default: break;
    }
    if (!typing)
        return std::nullopt;
    if (auto const printable = printableOf(key))
    {
        appendUtf8(state.text, *printable);
        return std::nullopt;
    }
    // A plain special key the text does not take acts as it would with nothing typed.
    if (auto const passed = passedKeyAction(key, keys))
        return applyNavAction(*passed, key, box, engine, hooksAtInput, nearTab);
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

    /// @return What a click on a place's label, or on the text, records: it picks the place.
    [[nodiscard]] Hit placeHit(TaskBoxField field) noexcept
    {
        return Hit { .kind = HitKind::DialogSetting, .value = std::to_underlying(field) };
    }

    /// One drawn line: segments from the left.
    using Line = std::vector<Segment>;

    /// The cells a setting's label takes.
    constexpr auto LabelColumns = 10;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;

    /// Appends a labelled value, wrapped to the width.
    void appendValue(
        std::vector<Line>& lines, std::string_view label, std::string_view value, Role role, int width)
    {
        auto const wrapped = wrap(value, width - LabelColumns, width - LabelColumns);
        for (auto const [index, part]: std::views::enumerate(wrapped))
            lines.push_back(
                Line { Segment { .text = std::format("{:<{}}", index == 0 ? label : ""sv, LabelColumns),
                                 .role = Role::Dim },
                       Segment { .text = part, .role = role } });
    }

    /// Appends text of its own, wrapped to the width; each line records @p hit, where there is one.
    void appendText(std::vector<Line>& lines,
                    std::string_view text,
                    Role role,
                    int width,
                    std::optional<Hit> hit = std::nullopt)
    {
        for (auto const& part: wrap(text, width, width))
            lines.push_back(Line { Segment { .text = part, .role = role, .hit = hit } });
    }

    /// @return What a setting shows: the configured value the form holds, for the worker with its active
    ///         runs.
    [[nodiscard]] std::string settingText(FormField setting, TaskBoxState const& box, Engine const& engine)
    {
        auto const* const config = engine.config();
        if (config == nullptr)
            return "?";
        auto const& form = box.form;
        switch (setting)
        {
            case FormField::Repo: return form.repo;
            case FormField::Model:
                return form.model < config->models().size() ? config->models()[form.model] : "?";
            case FormField::Worker: {
                auto const* const worker = workerOf(form, engine);
                if (worker == nullptr)
                    return "?";
                auto text =
                    std::format("{} {}", worker->id, activeRunsOn(*config, engine.runs(), worker->id));
                if (isSetupIncomplete(engine, *worker))
                    text += " (setup)";
                return text;
            }
            case FormField::Task:
            case FormField::Note:
            case FormField::Permissions: break;
        }
        return "?";
    }

} // namespace

/// The box's lines at one width.
struct TaskBoxLayout
{
    std::vector<Line> lines = {};
};

namespace
{
    [[nodiscard]] TaskBoxLayout layoutOf(TaskBoxState const& box, Engine const& engine, int width)
    {
        auto layout = TaskBoxLayout {};
        auto& lines = layout.lines;

        // The text, with a cursor while it has the keys.
        lines.push_back(Line { Segment {
            .text = "What should the agent do?", .role = Role::Dim, .hit = placeHit(TaskBoxField::Text) } });
        auto const typing = box.mode == FieldMode::Typing;
        auto const textRole = box.text.starts_with('-')         ? Role::Failing
                              : box.focus == TaskBoxField::Text ? Role::Selection
                                                                : Role::Text;
        auto const textLine = lines.size();
        appendText(lines,
                   typing ? std::format("{}▏", box.text) : box.text,
                   textRole,
                   width,
                   placeHit(TaskBoxField::Text));
        // An empty text keeps its line, so the settings below do not move as it is left.
        if (lines.size() == textLine)
            lines.emplace_back();
        lines.emplace_back();

        // The settings it starts with, each stepped with ←/→ while it has the keys, or a click: on its first
        // arrow back, on its value or its second arrow on.
        for (auto const& row: FieldRows)
            if (row.setting)
            {
                auto const role = box.focus == row.field ? Role::Selection : Role::Text;
                auto const step = [&row](std::size_t forward) {
                    return Hit { .kind = HitKind::DialogStep,
                                 .value = (2 * std::to_underlying(row.field)) + forward };
                };
                lines.push_back(
                    Line { Segment { .text = std::format("{:<{}}", row.label, LabelColumns),
                                     .role = Role::Dim,
                                     .hit = placeHit(row.field) },
                           Segment { .text = "‹ ", .role = role, .hit = step(0) },
                           Segment { .text = cut(std::format("{} ›", settingText(*row.setting, box, engine)),
                                                 std::max(width - LabelColumns - 2, 4)),
                                     .role = role,
                                     .hit = step(1) } });
            }

        // What it starts, or why the engine would refuse it: only once there is something to start.
        auto const form = formOf(box);
        auto const blank = isBlank(box.text);
        auto const preview = blank ? std::nullopt : std::optional { previewOf(form, engine) };
        if (preview)
        {
            auto rule = std::string {};
            for ([[maybe_unused]] auto const _: std::views::iota(0, std::max(width, 0)))
                rule += "─";
            lines.push_back(Line { Segment { .text = std::move(rule), .role = Role::Border } });
            if (*preview)
            {
                appendValue(lines,
                            "branch",
                            withoutStamp((*preview)->run.branch, (*preview)->run),
                            Role::Text,
                            width);
                appendValue(lines,
                            "worktree",
                            withoutStamp((*preview)->run.worktree, (*preview)->run),
                            Role::Text,
                            width);
            }
            else
                appendValue(lines, "refused", refusalText(preview->error()), Role::Failing, width);
        }
        if (box.text.starts_with('-'))
            appendText(lines, "a topic may not start with -", Role::Failing, width);
        auto const* const worker = workerOf(form, engine);
        for (auto const& warning:
             worker != nullptr ? setupWarnings(engine, *worker) : std::vector<std::string> {})
            appendText(lines, warning, Role::Warning, width);
        lines.emplace_back();

        // What Enter does now, then the other keys, each named by its first key in force (a hint whose key is
        // unbound is left out).
        auto const& keys = keysInForce(engine);
        auto const label = [&](NavAction action) {
            return navKeyLabel(keys, action);
        };
        auto hints = std::vector<std::string> {};
        auto const add = [&](std::string const& key, std::string_view does) {
            if (!key.empty())
                hints.push_back(std::format("{} {}", key, does));
        };
        // `⏎` on the Text line types it, when it is an enter_field key there.
        if (!typing && box.focus == TaskBoxField::Text
            && std::ranges::contains(keys.keysOf(NavAction::EnterField), "enter"sv))
            add(std::string { shownKeyName("enter") }, "text"sv);
        else if (!blank && canConfirm(form, engine))
            hints.emplace_back(enterHint(form, engine, box.hooks));
        if (typing)
            add(label(NavAction::LeaveField), "settings"sv);
        else
        {
            add(label(NavAction::MoveUp) + label(NavAction::MoveDown), "move"sv);
            add(label(NavAction::PreviousValue) + label(NavAction::NextValue), "change"sv);
            add(label(NavAction::EnterField), "text"sv);
            add(label(NavAction::Close), "close"sv);
        }
        auto const views = std::vector<std::string_view> { hints.begin(), hints.end() };
        lines.push_back(Line { Segment { .text = joinHints(views, width), .role = Role::Dim } });
        return layout;
    }

    // }}}
} // namespace

void TaskBoxView::render(core::tui::Canvas& canvas)
{
    if (!_box)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "+ New task");
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

int TaskBoxView::heightAt(int width) const
{
    if (!_box)
        return 0;
    // The box's two rows around its lines.
    return static_cast<int>(layoutAt(width - 2 - SideColumns).lines.size()) + 2;
}

/// The last layout the box drew, and what it was built from.
struct TaskBoxView::LayoutCache
{
    TaskBoxState state;
    uint64_t revision = 0;
    int width = 0;
    TaskBoxLayout layout;
};

TaskBoxView::TaskBoxView(Engine const& engine, std::optional<TaskBoxState> const& box, HitMap& hits):
    _engine { engine }, _box { box }, _hits { hits }
{
}

TaskBoxView::~TaskBoxView() = default;

TaskBoxLayout const& TaskBoxView::layoutAt(int width) const
{
    // A frame asks for the height and then draws at it: one layout serves both, and the frames that change
    // neither the box, nor the engine, nor the width.
    if (!_cache || _cache->width != width || _cache->revision != _engine.revision() || _cache->state != *_box)
        _cache = std::make_unique<LayoutCache>(LayoutCache { .state = *_box,
                                                             .revision = _engine.revision(),
                                                             .width = width,
                                                             .layout = layoutOf(*_box, _engine, width) });
    return _cache->layout;
}

} // namespace orchestrator::view
