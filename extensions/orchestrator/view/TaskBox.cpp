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

std::optional<DispatchAction> applyTaskBoxKey(core::tui::KeyEvent const& key,
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

    // The line-editing keys come first: Alt+Backspace drops a word rather than reading as Esc.
    if (auto const edit = textEditOf(key))
    {
        applyTextEdit(state.text, *edit);
        return std::nullopt;
    }
    // A key with Alt is how an Esc reads when the next key follows it in the same read: the user pressed Esc.
    if (core::tui::hasModifier(key.modifiers, Modifier::Alt))
    {
        box.reset();
        return std::nullopt;
    }
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
        return std::nullopt;

    switch (key.key)
    {
        case KeyCode::Escape: box.reset(); return std::nullopt;
        case KeyCode::Tab:
            state.focus =
                nextField(state.focus, core::tui::hasModifier(key.modifiers, Modifier::Shift) ? -1 : 1);
            return std::nullopt;
        case KeyCode::Left:
        case KeyCode::Right:
            if (auto const setting = FieldRows[std::to_underlying(state.focus)].setting)
                changeSetting(state.form, *setting, key.key == KeyCode::Right ? 1 : -1, engine);
            return std::nullopt;
        case KeyCode::Enter: {
            auto action = confirm(state, engine, hooksAtInput, nearTab);
            if (action)
                box.reset();
            return action;
        }
        default: break;
    }
    if (auto const printable = printableOf(key))
        appendUtf8(state.text, *printable);
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

    /// Appends text of its own, wrapped to the width.
    void appendText(std::vector<Line>& lines, std::string_view text, Role role, int width)
    {
        for (auto const& part: wrap(text, width, width))
            lines.push_back(Line { Segment { .text = part, .role = role } });
    }

    /// @return What a setting shows: the configured value the form holds, for the worker with its active
    /// runs.
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
        lines.push_back(Line { Segment { .text = "What should the agent do?", .role = Role::Dim } });
        auto const typing = box.focus == TaskBoxField::Text;
        auto const textRole = box.text.starts_with('-') ? Role::Failing
                              : typing                  ? Role::Selection
                                                        : Role::Text;
        auto const textLine = lines.size();
        appendText(lines, typing ? std::format("{}▏", box.text) : box.text, textRole, width);
        // An empty text keeps its line, so the settings below do not move as it is left.
        if (lines.size() == textLine)
            lines.emplace_back();
        lines.emplace_back();

        // The settings it starts with, each stepped with ←/→ while it has the keys.
        for (auto const& row: FieldRows)
            if (row.setting)
                lines.push_back(Line {
                    Segment { .text = std::format("{:<{}}", row.label, LabelColumns), .role = Role::Dim },
                    Segment { .text = cut(std::format("‹ {} ›", settingText(*row.setting, box, engine)),
                                          std::max(width - LabelColumns, 4)),
                              .role = box.focus == row.field ? Role::Selection : Role::Text } });

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

        // What Enter does now, then the other keys.
        auto hints = std::vector<std::string_view> {};
        if (!blank && canConfirm(form, engine))
            hints.push_back(enterHint(form, engine, box.hooks));
        hints.insert(hints.end(), { "tab settings"sv, "←→ change"sv, "esc close"sv });
        lines.push_back(Line { Segment { .text = joinHints(hints, width), .role = Role::Dim } });
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
    auto const shown = std::min(lines.size(), static_cast<std::size_t>(std::max(inside.height(), 0)));
    for (auto const index: std::views::iota(std::size_t { 0 }, shown))
    {
        auto x = 1;
        for (auto const& segment: lines[index])
        {
            // Segments are sanitised already, and their blanks separate them: cutSanitised() keeps them.
            auto const text = cutSanitised(segment.text, width + 1 - x);
            inside.putString(static_cast<int>(index), x, text, styleOf(theme, segment.role));
            x += view::columnsOf(text);
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
