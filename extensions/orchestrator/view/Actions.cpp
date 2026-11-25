// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Actions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    // {{{ the commands

    /// Why the `n` box does not open.
    [[nodiscard]] std::string_view whyNoTaskBox(Engine const* engine)
    {
        return engine == nullptr || engine->config() == nullptr
                   ? "the orchestrator is not configured: the line above says what is missing"sv
                   : ""sv;
    }

    /// Why the missing plugins are not installed now.
    [[nodiscard]] std::string_view whyNoInstall(Engine const* engine)
    {
        if (engine == nullptr)
            return "the orchestrator is not configured"sv;
        if (engine->installingPlugins())
            return "installing the plugins…"sv;
        if (engine->missingPlugins().empty())
            return "every plugin the tasks need is installed"sv;
        return {};
    }

    /// Why a digit no quick reply is on does not answer.
    constexpr auto NoReply = "no quick reply is on this key; quick_replies sets them"sv;

    // The row commands first, the global ones after: the palette lists them in this order.
    constexpr auto Commands = std::array {
        CommandRow { .command = KeyCommand::Open, .label = "open"sv },
        CommandRow { .command = KeyCommand::RunAgain, .label = "run again"sv },
        CommandRow { .command = KeyCommand::EndRun, .label = "end"sv, .unoffered = "it is not active"sv },
        CommandRow { .command = KeyCommand::Reveal,
                     .label = "agent tab"sv,
                     .unoffered = "its agent has no tab open"sv },
        CommandRow {
            .command = KeyCommand::OpenLink, .label = "open link"sv, .unoffered = "it has no link"sv },
        CommandRow { .command = KeyCommand::RetryRemoval,
                     .label = "retry removal"sv,
                     .unoffered = "its removal cannot be tried again now"sv },
        CommandRow { .command = KeyCommand::Dismiss,
                     .label = "dismiss"sv,
                     .unoffered = "it is still being checked"sv },
        CommandRow { .command = KeyCommand::BoardTab,
                     .label = "board"sv,
                     .unoffered = "its board went with its worktree"sv },
        CommandRow { .command = KeyCommand::TranscriptTab,
                     .label = "transcript"sv,
                     .unoffered = "it has no transcript"sv },
        CommandRow { .command = KeyCommand::DetailsTab, .label = "details"sv },
        CommandRow {
            .command = KeyCommand::Fold, .label = "fold its group"sv, .unoffered = "it is in no group"sv },
        CommandRow { .command = KeyCommand::NewTask,
                     .label = "new task"sv,
                     .scope = CommandScope::Global,
                     .unavailable = whyNoTaskBox },
        CommandRow {
            .command = KeyCommand::Filter, .label = "filter the agents"sv, .scope = CommandScope::Global },
        CommandRow {
            .command = KeyCommand::Refresh, .label = "refresh the lists"sv, .scope = CommandScope::Global },
        CommandRow { .command = KeyCommand::InstallPlugins,
                     .label = "install the missing plugins"sv,
                     .scope = CommandScope::Global,
                     .unavailable = whyNoInstall },
        CommandRow { .command = KeyCommand::Timeline,
                     .label = "show the last 24 hours"sv,
                     .scope = CommandScope::Global },
        CommandRow {
            .command = KeyCommand::SwitchView, .label = "switch view"sv, .scope = CommandScope::Global },
        CommandRow { .command = KeyCommand::FoldOthers,
                     .label = "fold the other groups"sv,
                     .scope = CommandScope::Global },
        // The replies: a row's chip names each by the configuration's label (actionLabel()).
        CommandRow { .command = KeyCommand::Reply1, .label = "answer 1"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply2, .label = "answer 2"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply3, .label = "answer 3"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply4, .label = "answer 4"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply5, .label = "answer 5"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply6, .label = "answer 6"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply7, .label = "answer 7"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply8, .label = "answer 8"sv, .unoffered = NoReply },
        CommandRow { .command = KeyCommand::Reply9, .label = "answer 9"sv, .unoffered = NoReply },
    };

    /// Which selected line a Work view hint is shown for.
    enum class HintedOn : uint8_t
    {
        AnyLine = 0, ///< Whatever is selected, or nothing.
        Item,        ///< A merge request's or a ticket's row.
        Heading,     ///< A group's heading.
    };

    /// One hint of the Work view's action bar.
    struct WorkHint
    {
        KeyCommand command = KeyCommand::Fold; ///< What its key does, and a click on it.
        std::string_view label = {};           ///< What the bar calls it.
        HintedOn on = HintedOn::AnyLine;       ///< Which selected line it is shown for.
    };

    /// The Work view's hints, most important first.
    constexpr auto WorkHints = std::array {
        WorkHint { .command = KeyCommand::Enter, .label = "start"sv, .on = HintedOn::Item },
        WorkHint { .command = KeyCommand::Enter, .label = "fold"sv, .on = HintedOn::Heading },
        WorkHint { .command = KeyCommand::NewTask, .label = "new task"sv },
        WorkHint { .command = KeyCommand::OpenLink, .label = "open"sv, .on = HintedOn::Item },
        WorkHint { .command = KeyCommand::RowMenu, .label = "menu"sv, .on = HintedOn::Item },
        WorkHint { .command = KeyCommand::Fold, .label = "fold"sv },
        WorkHint { .command = KeyCommand::Filter, .label = "filter"sv },
        WorkHint { .command = KeyCommand::Sort, .label = "sort"sv },
        WorkHint { .command = KeyCommand::RepoNext, .label = "repository"sv },
        WorkHint { .command = KeyCommand::SwitchView, .label = "agents"sv },
    };

    /// @return Whether a hint shown on @p on is shown while @p selected is.
    [[nodiscard]] bool isHintedOn(HintedOn on, std::optional<WorkLineKind> selected) noexcept
    {
        switch (on)
        {
            case HintedOn::Item: return selected == WorkLineKind::Item;
            case HintedOn::Heading: return selected == WorkLineKind::Heading;
            case HintedOn::AnyLine: break;
        }
        return true;
    }

    // }}}
    // {{{ the actions

    // Per state, the spec's ranked actions first, which the bar hints; then every other one that applies,
    // which the palette lists, so that a key keeps working where it did. `⏎` on a live run always writes to
    // it: the label changes, the action does not.
    constexpr auto Actions = std::array {
        // The bar hints an asking row's replies as one chip, `1 2 3 answer`, before ⏎ (actionHints()).
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Open, .label = "see request"sv },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply1, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply2, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply3, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply4, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply5, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply6, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply7, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply8, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reply9, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Reveal },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::EndRun },
        ActionRow {
            .when = RowState::Asking, .command = KeyCommand::TranscriptTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Asking, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Asking, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Asking, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Asking, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Waiting, .command = KeyCommand::Open, .label = "reply"sv },
        ActionRow { .when = RowState::Waiting, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Waiting, .command = KeyCommand::BoardTab },
        ActionRow { .when = RowState::Waiting, .command = KeyCommand::EndRun },
        ActionRow { .when = RowState::Waiting, .command = KeyCommand::Reveal, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Waiting, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Waiting, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Waiting, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Errored, .command = KeyCommand::Open, .label = "reply"sv },
        ActionRow { .when = RowState::Errored, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Errored, .command = KeyCommand::DetailsTab },
        ActionRow { .when = RowState::Errored, .command = KeyCommand::EndRun },
        ActionRow { .when = RowState::Errored, .command = KeyCommand::Reveal, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Errored, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Errored, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Errored, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Starting, .command = KeyCommand::Open, .label = "message"sv },
        ActionRow { .when = RowState::Starting, .command = KeyCommand::Reveal },
        ActionRow { .when = RowState::Starting, .command = KeyCommand::EndRun },
        ActionRow {
            .when = RowState::Starting, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Starting, .command = KeyCommand::TranscriptTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Starting, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Starting, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Starting, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Working, .command = KeyCommand::Open, .label = "message"sv },
        ActionRow { .when = RowState::Working, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Working, .command = KeyCommand::BoardTab },
        ActionRow { .when = RowState::Working, .command = KeyCommand::EndRun },
        ActionRow { .when = RowState::Working, .command = KeyCommand::Reveal, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Working, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Working, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Working, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Done, .command = KeyCommand::Open, .label = "follow up"sv },
        ActionRow { .when = RowState::Done, .command = KeyCommand::OpenLink },
        ActionRow { .when = RowState::Done, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Done, .command = KeyCommand::EndRun },
        ActionRow { .when = RowState::Done, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Done, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Done, .command = KeyCommand::Reveal, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Done, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Ending, .command = KeyCommand::Open, .label = "watch it end"sv },
        ActionRow { .when = RowState::Ending, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Ending, .command = KeyCommand::DetailsTab },
        ActionRow {
            .when = RowState::Ending, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Ending, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Ending, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Finished, .command = KeyCommand::RunAgain },
        ActionRow { .when = RowState::Finished, .command = KeyCommand::TranscriptTab },
        ActionRow { .when = RowState::Finished, .command = KeyCommand::Dismiss },
        ActionRow {
            .when = RowState::Finished, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Finished, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow {
            .when = RowState::Finished, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Finished, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::Kept, .command = KeyCommand::RetryRemoval },
        ActionRow { .when = RowState::Kept, .command = KeyCommand::DetailsTab },
        ActionRow { .when = RowState::Kept, .command = KeyCommand::Dismiss },
        ActionRow {
            .when = RowState::Kept, .command = KeyCommand::TranscriptTab, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Kept, .command = KeyCommand::BoardTab, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Kept, .command = KeyCommand::OpenLink, .hinted = Hinted::PaletteOnly },
        ActionRow { .when = RowState::Kept, .command = KeyCommand::Fold, .hinted = Hinted::PaletteOnly },

        ActionRow { .when = RowState::NewTask, .command = KeyCommand::NewTask },

        ActionRow { .when = RowState::GroupHeader, .command = KeyCommand::Fold, .label = "unfold"sv },

        ActionRow { .when = RowState::Leftover, .command = KeyCommand::Open, .label = "details"sv },
        ActionRow {
            .when = RowState::Leftover, .command = KeyCommand::DetailsTab, .hinted = Hinted::PaletteOnly },
    };

    // A state's actions are one run of the table, so that actionsFor() is a span of it.
    static_assert(
        std::ranges::all_of(std::views::iota(std::size_t { 1 }, Actions.size()), [](std::size_t at) {
            return Actions[at].when == Actions[at - 1].when
                   || std::ranges::none_of(Actions | std::views::take(at - 1), [&](ActionRow const& row) {
                          return row.when == Actions[at].when;
                      });
        }));

    /// Why a command does not apply to a row whose state lists no action for it, for the states that are not
    /// a run; a run's says `not while it is <its look's word>`.
    struct NotFor
    {
        RowState state = RowState::None; ///< The state.
        std::string_view reason = {};    ///< Why.
    };

    constexpr auto NotForRows = std::array {
        NotFor { .state = RowState::None, .reason = "no run selected"sv },
        NotFor { .state = RowState::NewTask, .reason = "no run selected"sv },
        NotFor { .state = RowState::GroupHeader, .reason = "not on a group heading"sv },
        NotFor { .state = RowState::Leftover, .reason = "not for a leftover worktree"sv },
    };

    /// @return The row of @p command in Commands; nullptr when it has none.
    [[nodiscard]] CommandRow const* commandRowOf(KeyCommand command) noexcept
    {
        auto const row = std::ranges::find(Commands, command, &CommandRow::command);
        return row != Commands.end() ? &*row : nullptr;
    }

    [[nodiscard]] RowState stateOf(Row const* row) noexcept
    {
        return row != nullptr ? row->rowState : RowState::None;
    }

    // }}}
} // namespace

std::span<CommandRow const> commandRows() noexcept
{
    return Commands;
}

std::span<ActionRow const> actionRows() noexcept
{
    return Actions;
}

std::span<ActionRow const> actionsFor(RowState state) noexcept
{
    auto const first = std::ranges::find(Actions, state, &ActionRow::when);
    auto const last =
        std::find_if(first, Actions.end(), [&](ActionRow const& row) { return row.when != state; });
    return { first, last };
}

std::string_view labelOf(KeyCommand command) noexcept
{
    auto const* const row = commandRowOf(command);
    return row != nullptr ? row->label : std::string_view {};
}

std::string_view labelOf(ActionRow const& action) noexcept
{
    return action.label.empty() ? labelOf(action.command) : action.label;
}

std::string_view actionLabel(Row const* row, ActionRow const& action) noexcept
{
    if (auto const key = replyDigitOf(action.command); key && row != nullptr)
        if (auto const* const reply = findQuickReply(row->replies, *key))
            return reply->label;
    return labelOf(action);
}

std::string replyChip(Row const& row)
{
    if (row.replies.empty())
        return {};
    auto keys = std::string {};
    for (auto const& reply: row.replies)
        keys += keys.empty() ? std::string(1, reply.key) : std::format(" {}", reply.key);
    return std::format("{} answer", keys);
}

std::optional<KeyCommand> primaryAction(Row const* row) noexcept
{
    auto const actions = actionsFor(stateOf(row));
    return actions.empty() ? std::nullopt : std::optional { actions.front().command };
}

std::string whyNot(Row const* row, KeyCommand command, Engine const* engine)
{
    auto const* const listed = commandRowOf(command);
    if (listed == nullptr)
        return {};
    auto const state = stateOf(row);
    // A global command the row's state makes its primary action, such as `+ new task`'s, is still global.
    if (listed->scope == CommandScope::Global)
        return listed->unavailable != nullptr ? std::string { listed->unavailable(engine) } : std::string {};
    if (!std::ranges::contains(actionsFor(state), command, &ActionRow::command))
    {
        if (auto const notFor = std::ranges::find(NotForRows, state, &NotFor::state);
            notFor != NotForRows.end())
            return std::string { notFor->reason };
        return std::format("not while it is {}", row->state);
    }
    if (isRowCommand(command) && !std::ranges::contains(row->offers, command))
        return std::string { listed->unoffered };
    if (auto const key = replyDigitOf(command); key && findQuickReply(row->replies, *key) == nullptr)
        return std::string { listed->unoffered };
    return {};
}

std::string refusalNotice(Row const* row, KeyCommand command, Engine const* engine)
{
    auto const why = whyNot(row, command, engine);
    if (why.empty())
        return {};
    auto const key = keyNameOf(command);
    return key.empty() ? std::format("{} · {}", labelOf(command), why)
                       : std::format("{} {} · {}", key, labelOf(command), why);
}

std::vector<std::string> actionHints(Row const* row)
{
    auto hints = std::vector<std::string> {};
    // The digits are the answer: an asking row's replies come first, as one chip.
    if (row != nullptr && row->rowState == RowState::Asking)
        if (auto chip = replyChip(*row); !chip.empty())
            hints.push_back(std::move(chip));
    for (auto const [rank, action]: std::views::enumerate(actionsFor(stateOf(row))))
    {
        // The primary action is `⏎`'s; any other is hinted only with a key of its own.
        auto const key = rank == 0 ? keyNameOf(KeyCommand::Enter) : keyNameOf(action.command);
        // A global command a state lists, such as `+ new task`'s, is hinted wherever the bar is drawn.
        auto const isRowScoped = commandRowOf(action.command)->scope == CommandScope::Row;
        if (key.empty() || action.hinted == Hinted::PaletteOnly
            || (isRowScoped && !whyNot(row, action.command, nullptr).empty()))
            continue;
        hints.push_back(std::format("{} {}", key, labelOf(action)));
    }
    return hints;
}

std::string barText(Row const* row, Place place, MessageKeys keys, int columns)
{
    auto const palette = std::format("{} all actions", keyNameOf(KeyCommand::Palette));
    auto hints = std::vector<std::string> {};
    auto const joined = [&](int room) {
        auto const views =
            hints | std::views::transform([](std::string const& hint) { return std::string_view { hint }; })
            | std::ranges::to<std::vector>();
        return joinHints(views, room);
    };
    // The message box's own keys, the palette among them while it fits: the box is a text field.
    if (place == Place::Focus && keys == MessageKeys::InBox)
    {
        hints = { "⏎ send", "alt⏎ line", "esc keep draft", palette, "pgup pgdn scroll" };
        return joined(columns);
    }
    hints = actionHints(row);
    if (place == Place::Focus)
        hints.push_back(std::format("{} back", keyNameOf(KeyCommand::Back)));
    if (!std::ranges::contains(actionsFor(stateOf(row)), KeyCommand::NewTask, &ActionRow::command))
        hints.push_back(std::format("{} {}", keyNameOf(KeyCommand::NewTask), labelOf(KeyCommand::NewTask)));
    // The palette always shows: whatever was cut is in it.
    auto const line = joined(columns - columnsOf(palette) - columnsOf(HintSeparator));
    if (line.empty())
        return cut(palette, columns);
    return std::format("{}{}{}", line, HintSeparator, palette);
}

std::vector<BarHint> workBarHints(std::optional<WorkLineKind> selected, int columns)
{
    auto palette = BarHint { .command = KeyCommand::Palette,
                             .text = std::format("{} all actions", keyNameOf(KeyCommand::Palette)) };
    // The palette always shows: whatever was cut is in it. The hints before it take what is left, whole.
    auto const room = columns - columnsOf(palette.text) - columnsOf(HintSeparator);
    auto hints = std::vector<BarHint> {};
    auto used = 0;
    for (auto const& hint: WorkHints)
    {
        if (!isHintedOn(hint.on, selected))
            continue;
        auto text = std::format("{} {}", workKeyNameOf(hint.command), hint.label);
        auto const next = used + (hints.empty() ? 0 : columnsOf(HintSeparator)) + columnsOf(text);
        if (next > room)
            break;
        used = next;
        hints.push_back(BarHint { .command = hint.command, .text = std::move(text) });
    }
    if (hints.empty())
        palette.text = cut(palette.text, columns);
    hints.push_back(std::move(palette));
    return hints;
}

std::string workBarText(std::optional<WorkLineKind> selected, int columns)
{
    auto line = std::string {};
    for (auto const& hint: workBarHints(selected, columns))
        line += line.empty() ? hint.text : std::format("{}{}", HintSeparator, hint.text);
    return line;
}

} // namespace orchestrator::view
