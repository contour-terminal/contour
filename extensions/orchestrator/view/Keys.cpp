// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Keys.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    // {{{ the keys

    // In the order the hint line shows them: the line is cut at the tab's edge, so the keys that find
    // everything else come first.
    constexpr auto KeyTable = std::array {
        KeyRow { .key = "up"sv, .command = KeyCommand::Up, .hint = "↑↓ move"sv },
        KeyRow { .key = "k"sv, .command = KeyCommand::Up, .hint = ""sv },
        KeyRow { .key = "down"sv, .command = KeyCommand::Down, .hint = ""sv },
        KeyRow { .key = "j"sv, .command = KeyCommand::Down, .hint = ""sv },
        KeyRow { .key = "home"sv, .command = KeyCommand::Home, .hint = ""sv },
        KeyRow { .key = "end"sv, .command = KeyCommand::End, .hint = ""sv },
        KeyRow { .key = "enter"sv, .command = KeyCommand::Enter, .hint = "enter open"sv },
        KeyRow { .key = "d"sv, .command = KeyCommand::Dispatch, .hint = "d dispatch"sv },
        KeyRow { .key = "?"sv, .command = KeyCommand::Help, .hint = "? help"sv },
        KeyRow { .key = "/"sv, .command = KeyCommand::Filter, .hint = "/ filter"sv },
        KeyRow { .key = "R"sv, .command = KeyCommand::Refresh, .hint = "R refresh"sv },
        KeyRow { .key = "e"sv, .command = KeyCommand::EndRun, .hint = "e end run"sv },
        KeyRow { .key = "x"sv, .command = KeyCommand::RetryRemoval, .hint = "x retry removal"sv },
        KeyRow { .key = "del"sv, .command = KeyCommand::Dismiss, .hint = "del dismiss"sv },
        KeyRow {
            .key = "tab"sv, .command = KeyCommand::Tab, .hint = "tab details"sv, .hiddenIn = Band::Wide },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back, .hint = "esc back"sv },
    };

    /// The name of a key that is not a character.
    struct SpecialKey
    {
        core::tui::KeyCode code = core::tui::KeyCode::None; ///< The key.
        std::string_view name = {};                         ///< What KeyRow::key calls it.
    };

    constexpr auto SpecialKeys = std::array {
        SpecialKey { .code = core::tui::KeyCode::Up, .name = "up"sv },
        SpecialKey { .code = core::tui::KeyCode::Down, .name = "down"sv },
        SpecialKey { .code = core::tui::KeyCode::Home, .name = "home"sv },
        SpecialKey { .code = core::tui::KeyCode::End, .name = "end"sv },
        SpecialKey { .code = core::tui::KeyCode::Enter, .name = "enter"sv },
        SpecialKey { .code = core::tui::KeyCode::Escape, .name = "esc"sv },
        SpecialKey { .code = core::tui::KeyCode::Tab, .name = "tab"sv },
        SpecialKey { .code = core::tui::KeyCode::Delete, .name = "del"sv },
    };

    /// @return Whether @p key was pressed with Ctrl or Alt, which no row of the table names.
    [[nodiscard]] bool isChord(core::tui::KeyEvent const& key) noexcept
    {
        return core::tui::hasModifier(key.modifiers, core::tui::Modifier::Ctrl)
               || core::tui::hasModifier(key.modifiers, core::tui::Modifier::Alt);
    }

    /// @return The name KeyRow::key gives @p key; empty for a key no name is given.
    [[nodiscard]] std::string nameOf(core::tui::KeyEvent const& key)
    {
        if (auto const special = std::ranges::find(SpecialKeys, key.key, &SpecialKey::code);
            special != SpecialKeys.end())
            return std::string { special->name };
        auto name = std::string {};
        if (auto const printable = printableOf(key))
            appendUtf8(name, *printable);
        return name;
    }

    // }}}
    // {{{ the commands

    /// Moves the selection to @p index, kept within the rows that can be selected.
    void select(Interaction& interaction, KeyContext const& context, std::size_t index) noexcept
    {
        interaction.selected = context.selectable == 0 ? 0 : std::min(index, context.selectable - 1);
    }

    /// @return The run command @p command on the selected row, when the row accepts it.
    template <typename Action>
    [[nodiscard]] std::optional<UiAction> runAction(KeyCommand command, KeyContext const& context)
    {
        if (context.selected == nullptr || !std::ranges::contains(context.selected->offers, command))
            return std::nullopt;
        return UiAction { Action { .run = context.selected->run } };
    }

    /// Opens the dispatch form on the selected item, on @p task; or says why it cannot.
    void openFormOn(Interaction& interaction, KeyContext const& context, TaskDef const* task)
    {
        auto const* const row = context.selected;
        if (row == nullptr || !row->item || context.engine == nullptr)
            return;
        if (task == nullptr)
        {
            interaction.notice = "no task applies to this item";
            return;
        }
        // Its session id is minted by the dashboard, which holds the source of them.
        interaction.form = openDispatchForm(*context.engine, row->repo, *row->item, task->id, {});
        if (!interaction.form)
            interaction.notice = std::format("{} does not apply to this item", task->label);
    }

    /// @return The first task that applies to the selected item, as Enter and `d` offer it; nullptr when
    /// none.
    [[nodiscard]] TaskDef const* firstTaskOf(KeyContext const& context) noexcept
    {
        auto const* const config = context.engine != nullptr ? context.engine->config() : nullptr;
        if (config == nullptr || context.selected == nullptr || !context.selected->item)
            return nullptr;
        return config->firstTaskFor(appliesToOf(*context.selected->item));
    }

    using Handler = std::optional<UiAction> (*)(Interaction&, KeyContext const&);

    /// One command and what it does.
    struct CommandRow
    {
        KeyCommand command = KeyCommand::Up; ///< The command.
        Handler handler = nullptr;           ///< What it does.
    };

    constexpr auto Commands = std::array {
        CommandRow {
            .command = KeyCommand::Up,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                select(interaction, context, interaction.selected == 0 ? 0 : interaction.selected - 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Down,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                select(interaction, context, interaction.selected + 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Home,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                select(interaction, context, 0);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::End,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                select(interaction, context, context.selectable == 0 ? 0 : context.selectable - 1);
                return std::nullopt;
            } },
        // Enter acts on what the row stands for: an agent is revealed, an item dispatched on (or its agent
        // revealed), a finished run's or an orphan's details opened. Tab shows any row's details.
        CommandRow {
            .command = KeyCommand::Enter,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const row = context.selected;
                if (row == nullptr)
                    return std::nullopt;
                switch (row->kind)
                {
                    case RowKind::Agent: return UiAction { RevealAgentAction { .run = row->run } };
                    case RowKind::PullRequest:
                    case RowKind::Issue:
                        if (auto const* const run = context.engine != nullptr && row->item
                                                        ? context.engine->activeRunOn(row->repo, *row->item)
                                                        : nullptr)
                            return UiAction { RevealAgentAction { .run = run->id } };
                        openFormOn(interaction, context, firstTaskOf(context));
                        return std::nullopt;
                    default: break;
                }
                // An orphan's details hold a command line, which wants the whole width.
                if (context.band != Band::Wide || row->kind == RowKind::Orphan)
                    interaction.pane = Pane::Details;
                if (row->onOpen == OpenEffect::MarkSeen)
                    return UiAction { MarkSeenAction { .run = row->run } };
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Dispatch,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                openFormOn(interaction, context, firstTaskOf(context));
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Task,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const config = context.engine != nullptr ? context.engine->config() : nullptr;
                if (config == nullptr || !context.task || *context.task >= config->tasks().size())
                    return std::nullopt;
                openFormOn(interaction, context, &config->tasks()[*context.task]);
                return std::nullopt;
            } },
        CommandRow { .command = KeyCommand::Back,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         if (interaction.overlay != Overlay::None)
                             interaction.overlay = Overlay::None;
                         else if (interaction.pane == Pane::Details)
                             interaction.pane = Pane::List;
                         else
                             interaction.filter.clear();
                         return std::nullopt;
                     } },
        CommandRow {
            .command = KeyCommand::Tab,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (context.band != Band::Wide)
                    interaction.pane = interaction.pane == Pane::List ? Pane::Details : Pane::List;
                return std::nullopt;
            } },
        CommandRow { .command = KeyCommand::Filter,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.filtering = Filtering::Typing;
                         interaction.pane = Pane::List;
                         return std::nullopt;
                     } },
        CommandRow { .command = KeyCommand::Help,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.overlay =
                             interaction.overlay == Overlay::Help ? Overlay::None : Overlay::Help;
                         return std::nullopt;
                     } },
        CommandRow { .command = KeyCommand::Refresh,
                     .handler = [](Interaction&, KeyContext const&) -> std::optional<UiAction> {
                         return UiAction { RefreshNowAction {} };
                     } },
        // D14: ending a run asks once. The first `e` names the run on the summary line; a second on it ends
        // it.
        CommandRow {
            .command = KeyCommand::EndRun,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto action = runAction<EndRunAction>(KeyCommand::EndRun, context);
                if (!action)
                    return std::nullopt;
                if (interaction.pendingEnd != context.selected->run)
                {
                    interaction.pendingEnd = context.selected->run;
                    return std::nullopt;
                }
                interaction.pendingEnd.clear();
                return action;
            } },
        CommandRow { .command = KeyCommand::RetryRemoval,
                     .handler =
                         [](Interaction&, KeyContext const& context) {
                             return runAction<RetryRemovalAction>(KeyCommand::RetryRemoval, context);
                         } },
        CommandRow { .command = KeyCommand::Dismiss,
                     .handler =
                         [](Interaction&, KeyContext const& context) {
                             return runAction<DismissAction>(KeyCommand::Dismiss, context);
                         } },
    };

    // }}}
} // namespace

std::optional<char32_t> printableOf(core::tui::KeyEvent const& key) noexcept
{
    if (key.codepoint < U' ' || key.codepoint == U'\x7f' || isChord(key))
        return std::nullopt;
    return key.codepoint;
}

void dropLastCodepoint(std::string& text) noexcept
{
    constexpr auto ContinuationMask = uint8_t { 0b1100'0000 };
    constexpr auto ContinuationBits = uint8_t { 0b1000'0000 };
    while (!text.empty() && (static_cast<uint8_t>(text.back()) & ContinuationMask) == ContinuationBits)
        text.pop_back();
    if (!text.empty())
        text.pop_back();
}

std::string pastedText(std::string_view text)
{
    auto line = std::string {};
    line.reserve(text.size());
    auto previous = '\0';
    for (auto const c: text)
    {
        if (c == '\n' && previous == '\r') // one line end, spelt CR LF
            continue;
        previous = c;
        if (c == '\r' || c == '\n' || c == '\t')
            line += ' ';
        else if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f')
            line += c;
    }
    return line;
}

void pasteInto(Interaction& interaction, std::string_view text)
{
    if (interaction.form)
    {
        if (interaction.form->focus == FormField::Note)
            interaction.form->note += pastedText(text);
        return;
    }
    if (interaction.filtering == Filtering::Typing)
    {
        interaction.filter += pastedText(text);
        interaction.selected = 0;
    }
}

std::span<KeyRow const> keyRows() noexcept
{
    return KeyTable;
}

std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key) noexcept
{
    if (isChord(key))
        return std::nullopt;
    auto const name = nameOf(key);
    auto const row = std::ranges::find(KeyTable, std::string_view { name }, &KeyRow::key);
    return row != KeyTable.end() ? std::optional { row->command } : std::nullopt;
}

std::optional<UiAction> dispatch(KeyCommand command, Interaction& interaction, KeyContext const& context)
{
    auto const row = std::ranges::find(Commands, command, &CommandRow::command);
    return row != Commands.end() ? row->handler(interaction, context) : std::nullopt;
}

bool isDispatched(KeyCommand command) noexcept
{
    return std::ranges::contains(Commands, command, &CommandRow::command);
}

FilterInput typeIntoFilter(core::tui::KeyEvent const& key, Interaction& interaction)
{
    if (interaction.filtering != Filtering::Typing)
        return FilterInput::NotTaken;
    switch (key.key)
    {
        case core::tui::KeyCode::Escape:
            interaction.filter.clear();
            interaction.filtering = Filtering::Off;
            return FilterInput::Taken;
        case core::tui::KeyCode::Enter: interaction.filtering = Filtering::Off; return FilterInput::Taken;
        case core::tui::KeyCode::Backspace: dropLastCodepoint(interaction.filter); return FilterInput::Taken;
        case core::tui::KeyCode::Delete: return FilterInput::Taken; // Typing, never a run command.
        default: break;
    }
    auto const printable = printableOf(key);
    if (!printable)
        return FilterInput::NotTaken;
    appendUtf8(interaction.filter, *printable);
    interaction.selected = 0;
    return FilterInput::Taken;
}

std::optional<UiAction> applyKey(core::tui::KeyEvent const& key,
                                 Interaction& interaction,
                                 KeyContext const& context)
{
    if (typeIntoFilter(key, interaction) == FilterInput::Taken)
        return std::nullopt;
    auto command = commandFor(key);
    auto taskContext = context;
    // A task's key, from the configuration: the table refuses one the dashboard binds (dashboardKeys()).
    if (auto const* const config = context.engine != nullptr ? context.engine->config() : nullptr;
        !command && config != nullptr)
        if (auto const printable = printableOf(key))
        {
            auto const tasks = config->tasks();
            auto const task = std::ranges::find(tasks, *printable, [](TaskDef const& def) {
                return static_cast<char32_t>(static_cast<unsigned char>(def.key));
            });
            if (task != tasks.end())
            {
                command = KeyCommand::Task;
                taskContext.task = static_cast<std::size_t>(std::ranges::distance(tasks.begin(), task));
            }
        }
    if (!interaction.pendingEnd.empty() && command != KeyCommand::EndRun)
    {
        interaction.pendingEnd.clear();
        return std::nullopt;
    }
    return command ? dispatch(*command, interaction, taskContext) : std::nullopt;
}

} // namespace orchestrator::view
