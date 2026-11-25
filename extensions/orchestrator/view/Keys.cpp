// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Keys.hpp>

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
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <orchestrator/core/DashboardKeys.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/NavInput.hpp>
#include <orchestrator/view/Palette.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/TextEdit.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    // {{{ the keys

    // The first row of a command names its key on the bar and in the palette (keyNameOf()). The navigation
    // keys are not here: the keys in force map them to commands (listCommandOf()).
    constexpr auto KeyTable = std::array {
        KeyRow { .key = "enter"sv, .command = KeyCommand::Enter },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back },
        KeyRow { .key = "tab"sv, .command = KeyCommand::Tab },
        KeyRow { .key = "b"sv, .command = KeyCommand::BoardTab },
        KeyRow { .key = "t"sv, .command = KeyCommand::TranscriptTab },
        KeyRow { .key = "d"sv, .command = KeyCommand::DetailsTab },
        KeyRow { .key = "n"sv, .command = KeyCommand::NewTask },
        KeyRow { .key = "/"sv, .command = KeyCommand::Filter },
        KeyRow { .key = "e"sv, .command = KeyCommand::EndRun },
        KeyRow { .key = "o"sv, .command = KeyCommand::OpenLink },
        KeyRow { .key = "r"sv, .command = KeyCommand::Reveal },
        KeyRow { .key = "x"sv, .command = KeyCommand::RetryRemoval },
        KeyRow { .key = "del"sv, .command = KeyCommand::Dismiss },
        KeyRow { .key = "z"sv, .command = KeyCommand::Fold },
        KeyRow { .key = "pgup"sv, .command = KeyCommand::ScrollPageUp },
        KeyRow { .key = "pgdn"sv, .command = KeyCommand::ScrollPageDown },
        KeyRow { .key = "^k"sv, .command = KeyCommand::Palette },
        KeyRow { .key = "?"sv, .command = KeyCommand::Palette },
        KeyRow { .key = "w"sv, .command = KeyCommand::SwitchView },
        KeyRow { .key = "1"sv, .command = KeyCommand::Reply1 },
        KeyRow { .key = "2"sv, .command = KeyCommand::Reply2 },
        KeyRow { .key = "3"sv, .command = KeyCommand::Reply3 },
        KeyRow { .key = "4"sv, .command = KeyCommand::Reply4 },
        KeyRow { .key = "5"sv, .command = KeyCommand::Reply5 },
        KeyRow { .key = "6"sv, .command = KeyCommand::Reply6 },
        KeyRow { .key = "7"sv, .command = KeyCommand::Reply7 },
        KeyRow { .key = "8"sv, .command = KeyCommand::Reply8 },
        KeyRow { .key = "9"sv, .command = KeyCommand::Reply9 },
    };

    // The Work view's keys: the first row of a command names its key on the bar (workKeyNameOf()).
    constexpr auto WorkKeyTable = std::array {
        KeyRow { .key = "pgup"sv, .command = KeyCommand::ScrollPageUp },
        KeyRow { .key = "pgdn"sv, .command = KeyCommand::ScrollPageDown },
        KeyRow { .key = "enter"sv, .command = KeyCommand::Enter },
        KeyRow { .key = "z"sv, .command = KeyCommand::Fold },
        KeyRow { .key = "/"sv, .command = KeyCommand::Filter },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back },
        KeyRow { .key = "s"sv, .command = KeyCommand::Sort },
        KeyRow { .key = "o"sv, .command = KeyCommand::OpenLink },
        KeyRow { .key = "m"sv, .command = KeyCommand::RowMenu },
        KeyRow { .key = "n"sv, .command = KeyCommand::NewTask },
        KeyRow { .key = "w"sv, .command = KeyCommand::SwitchView },
        KeyRow { .key = "^k"sv, .command = KeyCommand::Palette },
        KeyRow { .key = "?"sv, .command = KeyCommand::Palette },
        KeyRow { .key = "tab"sv, .command = KeyCommand::Tab },
    };

    /// The commands that act on the selected row only where it offers them (Row::offers).
    constexpr auto RowCommands = std::array {
        KeyCommand::BoardTab, KeyCommand::TranscriptTab, KeyCommand::DetailsTab,
        KeyCommand::EndRun,   KeyCommand::RetryRemoval,  KeyCommand::Dismiss,
        KeyCommand::Reveal,   KeyCommand::OpenLink,      KeyCommand::Fold,
    };

    /// The Focus tab each tab command shows.
    struct TabCommand
    {
        KeyCommand command = KeyCommand::BoardTab; ///< The command.
        FocusTab tab = FocusTab::Board;            ///< The tab.
    };

    constexpr auto TabCommands = std::array {
        TabCommand { .command = KeyCommand::BoardTab, .tab = FocusTab::Board },
        TabCommand { .command = KeyCommand::TranscriptTab, .tab = FocusTab::Transcript },
        TabCommand { .command = KeyCommand::DetailsTab, .tab = FocusTab::Details },
    };

    /// @return Whether @p key was pressed with Alt, which no row of the table names.
    [[nodiscard]] bool hasAlt(core::tui::KeyEvent const& key) noexcept
    {
        return core::tui::hasModifier(key.modifiers, core::tui::Modifier::Alt);
    }

    /// @return Whether @p key was pressed with Ctrl or Alt: never text.
    [[nodiscard]] bool isChord(core::tui::KeyEvent const& key) noexcept
    {
        return core::tui::hasModifier(key.modifiers, core::tui::Modifier::Ctrl) || hasAlt(key);
    }

    /// @return What @p key does by @p table; std::nullopt for a key no row names, or one with Alt.
    [[nodiscard]] std::optional<KeyCommand> commandIn(std::span<KeyRow const> table,
                                                      core::tui::KeyEvent const& key)
    {
        if (hasAlt(key))
            return std::nullopt;
        auto const name = keyNameFor(key);
        auto const row = std::ranges::find(table, name, &KeyRow::key);
        return row != table.end() ? std::optional { row->command } : std::nullopt;
    }

    /// @return How the bar names the key @p command is bound to first: the first key in force of the
    ///         navigation action that does @p command in @p view, else @p table's first row; empty: none.
    [[nodiscard]] std::string keyNameIn(std::span<KeyRow const> table,
                                        View view,
                                        KeyCommand command,
                                        NavKeys const& keys)
    {
        for (auto const& action: navActionRows())
            if (listCommandOf(action.action, view) == command)
                return navKeyLabel(keys, action.action);
        auto const row = std::ranges::find(table, command, &KeyRow::command);
        return row != table.end() ? std::string { shownKeyName(row->key) } : std::string {};
    }

    // }}}
    // {{{ the commands

    /// Starts the Focus over for a row selected anew: its top, its transcript live on its last page.
    void restartFocus(Interaction& interaction) noexcept
    {
        interaction.scroll = 0;
        interaction.follow = Follow::Live;
        interaction.page = 0;
    }

    /// Moves the selection to @p index, kept within the rows that can be selected.
    void select(Interaction& interaction, KeyContext const& context, std::size_t index) noexcept
    {
        auto const chosen = context.selectable == 0 ? 0 : std::min(index, context.selectable - 1);
        if (chosen != interaction.selected)
            restartFocus(interaction);
        interaction.selected = chosen;
    }

    /// @return A run command on the selected row: dispatch() runs a handler only where its action applies.
    template <typename Action>
    [[nodiscard]] std::optional<UiAction> runAction(Interaction& /*interaction*/, KeyContext const& context)
    {
        return UiAction { Action { .run = context.selected->run } };
    }

    /// @return The selected row's run; nullptr when it stands for none the engine knows.
    [[nodiscard]] Run const* selectedRun(KeyContext const& context) noexcept
    {
        if (context.engine == nullptr || context.selected == nullptr || context.selected->run.empty())
            return nullptr;
        auto const runs = context.engine->runs();
        auto const run = std::ranges::find(runs, context.selected->run, &Run::id);
        return run != runs.end() ? &*run : nullptr;
    }

    /// @return The furthest the Focus's tab scrolls: its last line at the bottom.
    [[nodiscard]] std::size_t lastScroll(KeyContext const& context) noexcept
    {
        return context.lines > context.rows ? context.lines - context.rows : 0;
    }

    /// @return The lines a page key scrolls by: what the tab shows, less one that stays in view.
    [[nodiscard]] std::size_t pageOf(KeyContext const& context) noexcept
    {
        return context.rows > 1 ? context.rows - 1 : 1;
    }

    /// @return Whether the transcript shown follows the agent: it is the tab shown, live on its last page.
    [[nodiscard]] bool isFollowing(Interaction const& interaction, KeyContext const& context) noexcept
    {
        return context.tab == FocusTab::Transcript && interaction.follow == Follow::Live;
    }

    /// Scrolls the Focus's tab to @p line, kept within what it holds. The transcript follows the agent again
    /// once its end on its last page is reached, and stops following once it is scrolled away from it.
    void scrollTo(Interaction& interaction, KeyContext const& context, std::size_t line) noexcept
    {
        interaction.scroll = std::min(line, lastScroll(context));
        if (context.tab == FocusTab::Transcript)
            interaction.follow = interaction.page == 0 && interaction.scroll >= lastScroll(context)
                                     ? Follow::Live
                                     : Follow::Away;
    }

    /// @return The line the Focus's tab shows first: the end, while the transcript follows the agent.
    [[nodiscard]] std::size_t scrollOf(Interaction const& interaction, KeyContext const& context) noexcept
    {
        return isFollowing(interaction, context) ? lastScroll(context) : interaction.scroll;
    }

    /// Which way a half-page key moves.
    enum class Direction : uint8_t
    {
        Up = 0, ///< Towards the first row, the top.
        Down,   ///< Towards the last row, the end.
    };

    /// Moves the Fleet's selection by half the rows it showed, or scrolls the Focus's tab by half its height;
    /// at least by one.
    void moveHalfPage(Interaction& interaction, KeyContext const& context, Direction direction) noexcept
    {
        if (interaction.place == Place::Focus)
        {
            auto const half = static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, context.rows / 2));
            scrollFocus(interaction, context, direction == Direction::Up ? -half : half);
            return;
        }
        auto const half = std::max<std::size_t>(1, context.fleetRows / 2);
        auto const at = interaction.selected;
        select(interaction, context, direction == Direction::Up ? (at > half ? at - half : 0) : at + half);
    }

    /// Starts the selected finished run again: a topic's text in the `n` box, in its repository; an item in
    /// the start dialog, on its task.
    void runAgain(Interaction& interaction, KeyContext const& context)
    {
        auto const* const run = selectedRun(context);
        if (run == nullptr)
            return;
        if (std::holds_alternative<TopicRef>(run->item))
            openTaskBoxFor(interaction, *context.engine, run->repo, run->topic);
        else
            openStartDialogFor(interaction, *context.engine, run->repo, run->item, run->task);
    }

    /// Shows the `n` box opened, or says on the action bar why it did not open.
    void showTaskBox(Interaction& interaction, std::expected<TaskBoxState, StartRefusal> box)
    {
        if (!box)
        {
            interaction.notice = startRefusalNotice(box.error(), {});
            return;
        }
        interaction.taskBox = std::move(*box);
        interaction.overlay = Overlay::None;
    }

    /// Turns the transcript to @p page, within the pages it has; its last page follows the agent again.
    void turnTo(Interaction& interaction, KeyContext const& context, uint32_t page) noexcept
    {
        if (context.tab != FocusTab::Transcript || context.pages == 0)
            return;
        auto const target = std::clamp(page, uint32_t { 1 }, context.pages);
        interaction.scroll = 0;
        if (target == context.pages)
        {
            interaction.page = 0;
            interaction.follow = Follow::Live;
            return;
        }
        interaction.page = target;
        interaction.follow = Follow::Away;
    }

    /// @return The transcript's page shown: the one turned to, else its last.
    [[nodiscard]] uint32_t shownPage(Interaction const& interaction, KeyContext const& context) noexcept
    {
        return interaction.page != 0 ? interaction.page : context.pages;
    }

    /// Gives the Focus the keys, for a row it shows something of.
    void toFocus(Interaction& interaction, KeyContext const& context) noexcept
    {
        auto const* const row = context.selected;
        if (row != nullptr && (!row->run.empty() || row->kind == RowKind::Orphan))
            interaction.place = Place::Focus;
    }

    /// Folds or unfolds the selected row's group, and has the selection follow its heading.
    [[nodiscard]] std::optional<UiAction> toggleFold(Interaction& interaction, KeyContext const& context)
    {
        auto const* const row = context.selected;
        if (row == nullptr || row->group.empty())
            return std::nullopt;
        interaction.followGroup = row->group;
        return UiAction { ToggleFoldAction { .group = row->group } };
    }

    /// `HH:MM`, the engine's wall clock in the host's local time.
    [[nodiscard]] std::string clockOf(Engine const& engine)
    {
        constexpr auto MinuteMs = int64_t { 60'000 };
        constexpr auto DayMinutes = int64_t { 24 } * 60;
        auto const minutes =
            (static_cast<int64_t>(engine.wallMs()) / MinuteMs) + engine.host().utcOffsetMinutes;
        auto const ofDay = ((minutes % DayMinutes) + DayMinutes) % DayMinutes;
        return std::format("{:02}:{:02}", ofDay / 60, ofDay % 60);
    }

    /// Shows the Focus tab @p command names, for the selected row: the choice sticks for its run until the
    /// run's state changes.
    [[nodiscard]] std::optional<UiAction> chooseTab(Interaction& interaction,
                                                    KeyContext const& context,
                                                    KeyCommand command)
    {
        auto const tab = std::ranges::find(TabCommands, command, &TabCommand::command);
        if (tab == TabCommands.end())
            return std::nullopt;
        if (activeTabOf(interaction, context.selected) != tab->tab)
            restartFocus(interaction);
        interaction.chosenTab =
            TabChoice { .run = context.selected->run, .state = context.selected->state, .tab = tab->tab };
        return std::nullopt;
    }

    /// Which way ← and → step through the Focus's tabs.
    enum class TabStep : uint8_t
    {
        Previous = 0, ///< Towards the Board.
        Next,         ///< Towards the Details.
    };

    /// Shows the tab beside the one shown, in TabCommands' order, skipping a tab the selected row does not
    /// offer; at the first or the last tab nothing changes.
    void stepTab(Interaction& interaction, KeyContext const& context, TabStep step)
    {
        if (context.selected == nullptr)
            return;
        auto const offered = [&](TabCommand const& tab) {
            return std::ranges::contains(context.selected->offers, tab.command);
        };
        auto const pick = [&](auto&& tabs) {
            auto const from = std::ranges::find(tabs, context.tab, &TabCommand::tab);
            if (from == std::ranges::end(tabs))
                return;
            auto const rest = std::ranges::subrange(std::ranges::next(from), std::ranges::end(tabs));
            if (auto const to = std::ranges::find_if(rest, offered); to != std::ranges::end(rest))
                (void) chooseTab(interaction, context, to->command);
        };
        if (step == TabStep::Next)
            pick(TabCommands);
        else
            pick(TabCommands | std::views::reverse);
    }

    /// Opens the `n` box, empty, in the repository the user looks at: the Work view's repository chip's, else
    /// its selected item's; in the Agents view, the selected run's. dispatch() said already when the
    /// orchestrator is not configured.
    [[nodiscard]] std::optional<UiAction> openNewTask(Interaction& interaction, KeyContext const& context)
    {
        if (context.engine == nullptr)
            return std::nullopt;
        auto const& engine = *context.engine;
        auto chipRepo = std::string_view {};
        auto selectedRepo = std::string {};
        if (interaction.view == View::Work)
        {
            auto const& work = interaction.work;
            chipRepo = chipRepository(work, engine.repositories());
            auto const items = engine.workItems();
            auto const selected = std::ranges::find_if(
                items, [&](WorkItem const& item) { return workItemKey(item) == work.selectedKey; });
            if (selected != items.end())
                selectedRepo = selected->repo;
        }
        else if (auto const* const run = selectedRun(context))
            selectedRepo = run->repo;
        showTaskBox(interaction, openTaskBox(engine, chipRepo, selectedRepo, {}, {}));
        return std::nullopt;
    }

    /// Answers the selected agent that asks with the quick reply on @p key. A digit answers only while the
    /// selected run asks, its request then on screen in every band (the asking box, or its Needs you row's
    /// second line); else, and for a key no reply is on, it only says why.
    [[nodiscard]] std::optional<UiAction> answer(Interaction& interaction,
                                                 KeyContext const& context,
                                                 char key)
    {
        auto const* const row = context.selected;
        if (row == nullptr || row->rowState != RowState::Asking)
        {
            interaction.notice = "select the request first";
            return std::nullopt;
        }
        if (auto const& answered = interaction.answered; answered && answered->run == row->run)
        {
            interaction.notice = std::format("{} is answered · r opens its tab", row->name);
            return std::nullopt;
        }
        auto const* const reply = findQuickReply(row->replies, key);
        if (reply == nullptr)
        {
            interaction.notice = row->replies.empty()
                                     ? std::format("answer {} in its tab · r opens it", row->name)
                                     : std::format("no quick reply on {} · quick_replies sets them", key);
            return std::nullopt;
        }
        interaction.answered = AnsweredRequest { .run = row->run, .key = key, .atMs = context.nowMonoMs };
        interaction.notice = std::format("answered {}", reply->label);
        // A reply that awaits a reason opens the message box for it: the next keys say why.
        if (reply->then == AfterReply::Message)
            interaction.place = Place::Focus;
        return UiAction { QuickReplyAction { .run = row->run,
                                             .key = key,
                                             .label = reply->label,
                                             .press = reply->press,
                                             .then = reply->then } };
    }

    using Handler = std::optional<UiAction> (*)(Interaction&, KeyContext const&);

    /// One command and what it does.
    struct HandlerRow
    {
        KeyCommand command = KeyCommand::Up; ///< The command.
        Handler handler = nullptr;           ///< What it does.
    };

    constexpr auto Handlers = std::array {
        // In the Fleet the moves move the selection; in the Focus they scroll its tab.
        HandlerRow {
            .command = KeyCommand::Up,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                {
                    auto const from = scrollOf(interaction, context);
                    scrollTo(interaction, context, from == 0 ? 0 : from - 1);
                }
                else
                    select(interaction, context, interaction.selected == 0 ? 0 : interaction.selected - 1);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::Down,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    scrollTo(interaction, context, scrollOf(interaction, context) + 1);
                else
                    select(interaction, context, interaction.selected + 1);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::Home,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    scrollTo(interaction, context, 0);
                else
                    select(interaction, context, 0);
                return std::nullopt;
            } },
        // `end` in the Focus returns the transcript to the agent's last words.
        HandlerRow {
            .command = KeyCommand::End,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Fleet)
                {
                    select(interaction, context, context.selectable == 0 ? 0 : context.selectable - 1);
                    return std::nullopt;
                }
                if (context.tab == FocusTab::Transcript)
                {
                    restartFocus(interaction);
                    return std::nullopt;
                }
                scrollTo(interaction, context, lastScroll(context));
                return std::nullopt;
            } },
        // Half a page: the Fleet's selection by half the rows it shows, the Focus's tab by half its height.
        HandlerRow {
            .command = KeyCommand::HalfPageUp,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                moveHalfPage(interaction, context, Direction::Up);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::HalfPageDown,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                moveHalfPage(interaction, context, Direction::Down);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::ScrollPageUp,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const from = scrollOf(interaction, context);
                auto const page = pageOf(context);
                scrollTo(interaction, context, from > page ? from - page : 0);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::ScrollPageDown,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                scrollTo(interaction, context, scrollOf(interaction, context) + pageOf(context));
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::PreviousPage,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                // ← turns the transcript back a page; on another tab, or from its first page, it shows the
                // tab to the left.
                auto const page = shownPage(interaction, context);
                if (context.tab == FocusTab::Transcript && page > 1)
                    turnTo(interaction, context, page - 1);
                else
                    stepTab(interaction, context, TabStep::Previous);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::NextPage,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                // → turns the transcript on a page; on another tab, or from its last page, it shows the tab
                // to the right.
                auto const page = shownPage(interaction, context);
                if (context.tab == FocusTab::Transcript && page < context.pages)
                    turnTo(interaction, context, page + 1);
                else
                    stepTab(interaction, context, TabStep::Next);
                return std::nullopt;
            } },
        // `b`, `t` and `d` choose the Focus tab; the choice sticks for the run until its state changes.
        HandlerRow { .command = KeyCommand::BoardTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::BoardTab);
                         } },
        HandlerRow { .command = KeyCommand::TranscriptTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::TranscriptTab);
                         } },
        HandlerRow { .command = KeyCommand::DetailsTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::DetailsTab);
                         } },
        HandlerRow {
            .command = KeyCommand::Open,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                toFocus(interaction, context);
                return std::nullopt;
            } },
        HandlerRow {
            .command = KeyCommand::RunAgain,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                runAgain(interaction, context);
                return std::nullopt;
            } },
        HandlerRow { .command = KeyCommand::Back,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         if (interaction.place == Place::Focus)
                             backToFleet(interaction);
                         else
                             interaction.filter.clear();
                         return std::nullopt;
                     } },
        HandlerRow {
            .command = KeyCommand::Tab,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    backToFleet(interaction);
                else
                    toFocus(interaction, context);
                return std::nullopt;
            } },
        HandlerRow { .command = KeyCommand::Filter,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.filtering = Filtering::Typing;
                         interaction.place = Place::Fleet;
                         return std::nullopt;
                     } },
        // `^K` and `?` open the palette, with the query esc left in it.
        HandlerRow { .command = KeyCommand::Palette,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.palette =
                             PaletteState { .query = std::exchange(interaction.paletteQuery, {}) };
                         return std::nullopt;
                     } },
        // `w` shows the other view; each keeps where the user was in it.
        HandlerRow { .command = KeyCommand::SwitchView,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.view = interaction.view == View::Agents ? View::Work : View::Agents;
                         return std::nullopt;
                     } },
        // The Work view folds its groups; the Agents view has none of them to fold.
        HandlerRow { .command = KeyCommand::FoldOthers,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.notice = "fold the other groups · in the Work view only, w shows it";
                         return std::nullopt;
                     } },
        HandlerRow { .command = KeyCommand::Timeline,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.overlay = Overlay::Timeline;
                         return std::nullopt;
                     } },
        // `z` folds the group of the selected row and selects its heading: the run it was on may be gone.
        HandlerRow { .command = KeyCommand::Fold, .handler = toggleFold },
        HandlerRow { .command = KeyCommand::NewTask, .handler = openNewTask },
        HandlerRow { .command = KeyCommand::Refresh,
                     .handler = [](Interaction&, KeyContext const&) -> std::optional<UiAction> {
                         return UiAction { RefreshNowAction {} };
                     } },
        // The palette installs the plugins the agents lack into the user's own Claude Code: its entry names
        // them, and dispatch() refuses while none is missing or they are being installed.
        HandlerRow { .command = KeyCommand::InstallPlugins,
                     .handler = [](Interaction&, KeyContext const&) -> std::optional<UiAction> {
                         return UiAction { InstallPluginsAction {} };
                     } },
        // D14: ending a run asks once. The first `e` names the run on the action bar; a second on it ends it.
        HandlerRow {
            .command = KeyCommand::EndRun,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.pendingEnd != context.selected->run)
                {
                    interaction.pendingEnd = context.selected->run;
                    return std::nullopt;
                }
                interaction.pendingEnd.clear();
                return runAction<EndRunAction>(interaction, context);
            } },
        HandlerRow { .command = KeyCommand::RetryRemoval, .handler = runAction<RetryRemovalAction> },
        HandlerRow { .command = KeyCommand::Dismiss, .handler = runAction<DismissAction> },
        HandlerRow { .command = KeyCommand::Reveal, .handler = runAction<RevealAgentAction> },
        // `o` opens the run's link on the machine the user sits at; the guest says how it went.
        HandlerRow { .command = KeyCommand::OpenLink,
                     .handler = [](Interaction& /*interaction*/,
                                   KeyContext const& context) -> std::optional<UiAction> {
                         auto const* const run = selectedRun(context);
                         if (run == nullptr)
                             return std::nullopt;
                         return UiAction { OpenUrlAction { .url = run->url, .label = itemLabel(run->item) } };
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

void pasteInto(Interaction& interaction, KeyContext const& context, std::string_view text)
{
    if (interaction.startDialog)
    {
        pasteIntoStartDialog(interaction.startDialog, pastedText(text));
        return;
    }
    if (interaction.taskBox)
    {
        pasteIntoTaskBox(interaction.taskBox, pastedText(text));
        return;
    }
    if (interaction.palette)
    {
        interaction.palette->query += pastedText(text);
        interaction.palette->highlighted = 0;
        return;
    }
    // The Work view takes a paste into its filter while it is typed, and drops it anywhere else.
    if (interaction.view == View::Work)
    {
        if (interaction.work.filtering == Filtering::Typing)
            interaction.work.filter += pastedText(text);
        return;
    }
    if (interaction.place == Place::Focus && hasMessageBox(context.engine, context.selected))
    {
        // A message keeps its line feeds: the agent receives it as one paste. A CR LF is one line end, and
        // every other control character but a tab is dropped, as the host would refuse it.
        auto& draft = interaction.drafts[context.selected->run];
        auto previous = '\0';
        for (auto const c: text)
        {
            auto const isLineEnd = c == '\r' || c == '\n';
            if (c == '\n' && previous == '\r')
                continue;
            previous = c;
            if (isLineEnd)
                draft += '\n';
            else if (c == '\t' || (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f'))
                draft += c;
        }
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

std::span<KeyRow const> workKeyRows() noexcept
{
    return WorkKeyTable;
}

std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key) noexcept
{
    return commandIn(KeyTable, key);
}

std::optional<KeyCommand> workCommandFor(core::tui::KeyEvent const& key) noexcept
{
    return commandIn(WorkKeyTable, key);
}

std::string keyNameOf(KeyCommand command, NavKeys const& keys)
{
    return keyNameIn(KeyTable, View::Agents, command, keys);
}

std::string workKeyNameOf(KeyCommand command, NavKeys const& keys)
{
    return keyNameIn(WorkKeyTable, View::Work, command, keys);
}

bool isRowCommand(KeyCommand command) noexcept
{
    return std::ranges::contains(RowCommands, command);
}

FocusTab activeTabOf(Interaction const& interaction, Row const* row) noexcept
{
    if (row == nullptr || !row->attention)
        return FocusTab::Details;
    auto const& chosen = interaction.chosenTab;
    auto const tab = chosen && chosen->run == row->run && chosen->state == row->state
                         ? chosen->tab
                         : fleetGroupRow(*row->attention).opensOn;
    auto const command = std::ranges::find(TabCommands, tab, &TabCommand::tab)->command;
    return std::ranges::contains(row->offers, command) ? tab : FocusTab::Details;
}

bool hasMessageBox(Engine const* engine, Row const* row) noexcept
{
    return row != nullptr && row->kind == RowKind::Agent && row->rowState != RowState::Asking
           && hasAgentTab(engine, row->run);
}

void backToFleet(Interaction& interaction) noexcept
{
    interaction.place = Place::Fleet;
}

void selectRow(Interaction& interaction, KeyContext const& context, std::size_t index) noexcept
{
    auto const before = interaction.selected;
    select(interaction, context, index);
    if (interaction.selected != before)
        interaction.pendingEnd.clear();
    backToFleet(interaction);
}

void scrollFocus(Interaction& interaction, KeyContext const& context, std::ptrdiff_t lines) noexcept
{
    auto const from = static_cast<std::ptrdiff_t>(scrollOf(interaction, context));
    scrollTo(interaction, context, static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, from + lines)));
}

void openStartDialogFor(Interaction& interaction,
                        Engine const& engine,
                        std::string_view repo,
                        ItemRef const& item,
                        std::string_view task)
{
    if (engine.config() == nullptr)
    {
        interaction.notice = startRefusalNotice(StartRefusal::NotConfigured, {});
        return;
    }
    auto const items = engine.workItems();
    auto const listed = std::ranges::find_if(
        items, [&](WorkItem const& candidate) { return candidate.repo == repo && candidate.item == item; });
    auto dialog = listed != items.end() ? openStartDialog(engine, *listed, task, {})
                                        : std::unexpected(StartRefusal::NotListed);
    if (!dialog)
    {
        interaction.notice = startRefusalNotice(dialog.error(), itemLabel(item));
        return;
    }
    interaction.startDialog = std::move(*dialog);
    interaction.overlay = Overlay::None;
}

void openTaskBoxFor(Interaction& interaction, Engine const& engine, std::string_view repo, std::string text)
{
    showTaskBox(interaction, openTaskBox(engine, repo, {}, std::move(text), {}));
}

bool isCtrl(core::tui::KeyEvent const& key, char32_t letter) noexcept
{
    if (!core::tui::hasModifier(key.modifiers, core::tui::Modifier::Ctrl))
        return false;
    auto const upper = letter >= U'a' && letter <= U'z' ? letter - U'a' + U'A' : letter;
    return key.codepoint == letter || key.codepoint == upper
           || key.key == static_cast<core::tui::KeyCode>(letter);
}

bool hasAgentTab(Engine const* engine, std::string_view run) noexcept
{
    if (engine == nullptr)
        return false;
    auto const runs = engine->runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    return found != runs.end() && countsAsActive(*found) && found->pane.has_value();
}

std::optional<UiAction> dispatch(KeyCommand command, Interaction& interaction, KeyContext const& context)
{
    // Enter does the selected row's primary action, which the bar lists first.
    if (command == KeyCommand::Enter)
    {
        auto const primary = primaryAction(context.selected);
        if (!primary)
            return std::nullopt;
        command = *primary;
    }
    // A digit says why it does not answer in its own words.
    if (auto const key = replyDigitOf(command))
        return answer(interaction, context, *key);
    // An action that does not apply to the selected row, or that the engine does not allow now, says why.
    if (auto notice = refusalNotice(context.selected, command, context.engine); !notice.empty())
    {
        interaction.notice = std::move(notice);
        return std::nullopt;
    }
    auto const row = std::ranges::find(Handlers, command, &HandlerRow::command);
    return row != Handlers.end() ? row->handler(interaction, context) : std::nullopt;
}

bool isDispatched(KeyCommand command) noexcept
{
    return command == KeyCommand::Enter || replyDigitOf(command).has_value()
           || std::ranges::contains(Handlers, command, &HandlerRow::command);
}

namespace
{
    /// @return Whether the message box passes @p key on to the Focus: one of messagePassKeys(), which scroll
    ///         the Focus's tab or turn the transcript's pages, or `tab`, which leaves it.
    [[nodiscard]] bool passesMessageKey(core::tui::KeyEvent const& key)
    {
        auto const name = keyNameFor(key);
        return name == "tab" || std::ranges::contains(messagePassKeys(), name, &MessagePassKey::name);
    }

    /// Types @p key into the message box, while it has the keys.
    /// @return Nothing when it does not have them, or the box passes the key on (passesMessageKey()), so that
    ///         the Focus decides; else what the key asks: the message to send, or nothing.
    [[nodiscard]] std::optional<std::optional<UiAction>> typeIntoMessage(core::tui::KeyEvent const& key,
                                                                         Interaction& interaction,
                                                                         KeyContext const& context)
    {
        if (interaction.place != Place::Focus || !hasMessageBox(context.engine, context.selected))
            return std::nullopt;
        // `^K` opens the palette from the box too: a chord is never text.
        if (commandFor(key) == KeyCommand::Palette && isChord(key))
            return std::nullopt;
        // The box has no cursor to move yet: ← and → turn the transcript's pages, as in the Fleet.
        if (passesMessageKey(key))
            return std::nullopt;
        auto const& run = context.selected->run;
        auto& draft = interaction.drafts[run];
        // The line-editing keys come first: Alt+Backspace drops a word rather than reading as Esc.
        if (auto const edit = textEditOf(key))
        {
            applyTextEdit(draft, *edit);
            if (draft.empty())
                interaction.drafts.erase(run);
            return std::optional<UiAction> {};
        }
        auto const alt = core::tui::hasModifier(key.modifiers, core::tui::Modifier::Alt);
        // Alt+Enter adds a line. Any other key with Alt is how an Esc reads when the next key follows it in
        // the same read: back to the Fleet, the draft kept.
        if (alt && key.key == core::tui::KeyCode::Enter)
            draft += '\n';
        else if (alt || key.key == core::tui::KeyCode::Escape)
            backToFleet(interaction);
        else if (key.key == core::tui::KeyCode::Enter && !draft.empty())
        {
            auto text = std::move(draft);
            interaction.drafts.erase(run);
            interaction.sent = SentMessage { .run = run, .text = text, .clock = clockOf(*context.engine) };
            interaction.notice = std::format("sent to {}", context.selected->name);
            // The keys return to the Fleet; the selection stays on the run.
            backToFleet(interaction);
            return std::optional { UiAction { SendMessageAction { .run = run, .text = std::move(text) } } };
        }
        else if (auto const printable = printableOf(key))
            appendUtf8(draft, *printable);
        // A draft emptied is no draft: nothing is kept for the run.
        if (auto const kept = interaction.drafts.find(run);
            kept != interaction.drafts.end() && kept->second.empty())
            interaction.drafts.erase(kept);
        // Every other key is the box's too: no letter is a command while it has the keys.
        return std::optional<UiAction> {};
    }
} // namespace

FilterInput typeIntoFilter(core::tui::KeyEvent const& key, Filtering& filtering, std::string& filter)
{
    if (filtering != Filtering::Typing)
        return FilterInput::NotTaken;
    if (auto const edit = textEditOf(key))
    {
        applyTextEdit(filter, *edit);
        return FilterInput::Taken;
    }
    switch (key.key)
    {
        case core::tui::KeyCode::Escape:
            filter.clear();
            filtering = Filtering::Off;
            return FilterInput::Taken;
        case core::tui::KeyCode::Enter: filtering = Filtering::Off; return FilterInput::Taken;
        case core::tui::KeyCode::Delete: return FilterInput::Taken; // Typing, never a run command.
        default: break;
    }
    auto const printable = printableOf(key);
    if (!printable)
        return FilterInput::NotTaken;
    appendUtf8(filter, *printable);
    return FilterInput::Taken;
}

FilterInput typeIntoFilter(core::tui::KeyEvent const& key, Interaction& interaction)
{
    auto const before = interaction.filter.size();
    auto const taken = typeIntoFilter(key, interaction.filtering, interaction.filter);
    // A character typed starts the selection over at the top; an edit keeps it.
    if (interaction.filter.size() > before)
        interaction.selected = 0;
    return taken;
}

std::optional<UiAction> applyKey(core::tui::KeyEvent const& key,
                                 std::optional<NavAction> nav,
                                 Interaction& interaction,
                                 KeyContext const& context)
{
    if (interaction.palette)
        return applyPaletteKey(key, interaction, context);
    if (interaction.overlay == Overlay::Timeline)
    {
        // The timeline takes no command but the palette's: close, esc and ⏎ close it, as anything leaving it
        // does.
        auto const command = commandFor(key);
        if (nav == NavAction::Close || command == KeyCommand::Palette || command == KeyCommand::Back
            || command == KeyCommand::Enter || hasAlt(key))
            interaction.overlay = Overlay::None;
        return command == KeyCommand::Palette ? dispatch(*command, interaction, context) : std::nullopt;
    }
    auto const fieldContext = navContextOf(interaction, context);
    if (auto typed = typeIntoMessage(key, interaction, context))
        return std::move(*typed);
    if (typeIntoFilter(key, interaction) == FilterInput::Taken)
        return std::nullopt;
    // A key the message box or the filter passed on acts as it would with nothing typed: ← still turns the
    // transcript's pages, ↓ still moves the selection.
    if (fieldContext == NavContext::Typing)
    {
        auto none = std::string {};
        auto const again = navActionOf(key, NavContext::Navigating, keysInForce(context.engine), none);
        nav = again ? std::optional { *again } : std::nullopt;
    }
    auto const listCommand = nav ? listCommandOf(*nav, View::Agents) : std::nullopt;
    auto const command = listCommand ? listCommand : commandFor(key);
    if (!interaction.pendingEnd.empty() && command != KeyCommand::EndRun)
    {
        interaction.pendingEnd.clear();
        return std::nullopt;
    }
    return command ? dispatch(*command, interaction, context) : std::nullopt;
}

std::string_view chipRepository(WorkState const& state, std::span<std::string const> repos) noexcept
{
    return state.repoChip > 0 && state.repoChip <= repos.size()
               ? std::string_view { repos[state.repoChip - 1] }
               : std::string_view {};
}

} // namespace orchestrator::view
