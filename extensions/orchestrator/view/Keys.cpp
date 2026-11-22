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
        KeyRow { .key = "b"sv, .command = KeyCommand::Board, .hint = "b board"sv },
        KeyRow { .key = "t"sv, .command = KeyCommand::Transcript, .hint = "t transcript"sv },
        KeyRow { .key = "/"sv, .command = KeyCommand::Filter, .hint = "/ filter"sv },
        KeyRow { .key = "z"sv, .command = KeyCommand::Fold, .hint = "z fold"sv },
        KeyRow { .key = "R"sv, .command = KeyCommand::Refresh, .hint = "R refresh"sv },
        KeyRow { .key = "e"sv, .command = KeyCommand::EndRun, .hint = "e end run"sv },
        KeyRow { .key = "n"sv, .command = KeyCommand::NewTopic, .hint = "n topic"sv },
        KeyRow { .key = "I"sv, .command = KeyCommand::InstallPlugins, .hint = "I install plugins"sv },
        KeyRow { .key = "x"sv, .command = KeyCommand::RetryRemoval, .hint = "x retry removal"sv },
        KeyRow { .key = "del"sv, .command = KeyCommand::Dismiss, .hint = "del dismiss"sv },
        KeyRow {
            .key = "tab"sv, .command = KeyCommand::Tab, .hint = "tab details"sv, .hiddenIn = Band::Wide },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back, .hint = "esc back"sv },
        // The status board: read-only, so its keys scroll, reload and leave.
        KeyRow { .key = "up"sv, .command = KeyCommand::Up, .hint = "↑↓ scroll"sv, .on = ScreenSet::Board },
        KeyRow { .key = "k"sv, .command = KeyCommand::Up, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "down"sv, .command = KeyCommand::Down, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "j"sv, .command = KeyCommand::Down, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "pgup"sv, .command = KeyCommand::ScrollPageUp, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow {
            .key = "pgdn"sv, .command = KeyCommand::ScrollPageDown, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "home"sv, .command = KeyCommand::Home, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "end"sv, .command = KeyCommand::End, .hint = ""sv, .on = ScreenSet::Board },
        KeyRow { .key = "enter"sv,
                 .command = KeyCommand::Enter,
                 .hint = "enter agent tab"sv,
                 .on = ScreenSet::Board },
        KeyRow { .key = "t"sv,
                 .command = KeyCommand::Transcript,
                 .hint = "t transcript"sv,
                 .on = ScreenSet::Board },
        KeyRow {
            .key = "d"sv, .command = KeyCommand::Dispatch, .hint = "d new run"sv, .on = ScreenSet::Board },
        KeyRow { .key = "b"sv, .command = KeyCommand::Board, .hint = "b reload"sv, .on = ScreenSet::Board },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back, .hint = "esc back"sv, .on = ScreenSet::Board },
        // The transcript: read-only, a page at a time, so its keys page, scroll, switch to the board and
        // leave.
        KeyRow { .key = "left"sv,
                 .command = KeyCommand::PreviousPage,
                 .hint = "←→ page"sv,
                 .on = ScreenSet::Transcript },
        KeyRow {
            .key = "right"sv, .command = KeyCommand::NextPage, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow {
            .key = "up"sv, .command = KeyCommand::Up, .hint = "↑↓ scroll"sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "k"sv, .command = KeyCommand::Up, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "down"sv, .command = KeyCommand::Down, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "j"sv, .command = KeyCommand::Down, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow {
            .key = "pgup"sv, .command = KeyCommand::ScrollPageUp, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "pgdn"sv,
                 .command = KeyCommand::ScrollPageDown,
                 .hint = ""sv,
                 .on = ScreenSet::Transcript },
        KeyRow {
            .key = "home"sv, .command = KeyCommand::FirstPage, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "end"sv, .command = KeyCommand::LastPage, .hint = ""sv, .on = ScreenSet::Transcript },
        KeyRow {
            .key = "b"sv, .command = KeyCommand::Board, .hint = "b board"sv, .on = ScreenSet::Transcript },
        KeyRow { .key = "d"sv,
                 .command = KeyCommand::Dispatch,
                 .hint = "d new run"sv,
                 .on = ScreenSet::Transcript },
        KeyRow { .key = "enter"sv,
                 .command = KeyCommand::Enter,
                 .hint = "enter agent tab"sv,
                 .on = ScreenSet::Transcript },
        KeyRow {
            .key = "esc"sv, .command = KeyCommand::Back, .hint = "esc back"sv, .on = ScreenSet::Transcript },
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
        SpecialKey { .code = core::tui::KeyCode::PageUp, .name = "pgup"sv },
        SpecialKey { .code = core::tui::KeyCode::PageDown, .name = "pgdn"sv },
        SpecialKey { .code = core::tui::KeyCode::Left, .name = "left"sv },
        SpecialKey { .code = core::tui::KeyCode::Right, .name = "right"sv },
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

    /// @return The furthest a board or a transcript scrolls: its last line at the bottom of the tab.
    [[nodiscard]] std::size_t lastScroll(KeyContext const& context) noexcept
    {
        return context.lines > context.rows ? context.lines - context.rows : 0;
    }

    /// Scrolls a board or a transcript to @p line, kept within what it holds.
    void scrollTo(Interaction& interaction, KeyContext const& context, std::size_t line) noexcept
    {
        interaction.scroll = std::min(line, lastScroll(context));
    }

    /// @return The lines a page key scrolls by: what the tab shows, less one that stays in view.
    [[nodiscard]] std::size_t pageOf(KeyContext const& context) noexcept
    {
        return context.rows > 1 ? context.rows - 1 : 1;
    }

    /// Opens the dispatch form on a run's item and task (openFormFor()); does nothing for a run the engine
    /// no longer knows.
    void openFormOnRun(Interaction& interaction, KeyContext const& context, std::string_view runId)
    {
        if (context.engine == nullptr)
            return;
        auto const runs = context.engine->runs();
        auto const run = std::ranges::find(runs, runId, &Run::id);
        if (run != runs.end())
            openFormFor(interaction, *context.engine, run->repo, run->item, run->task, run->topic);
    }

    /// @return How many pages the shown transcript has, as its last page read says; 0 while none was read.
    [[nodiscard]] uint32_t pagesOf(Interaction const& interaction, KeyContext const& context) noexcept
    {
        auto const* const view = context.engine != nullptr
                                     ? context.engine->transcript(context.tab, interaction.subject)
                                     : nullptr;
        return view != nullptr && view->content ? view->content->pages : 0;
    }

    /// Turns the transcript to @p page, kept within the pages it has; a page it shows or asked for already
    /// is not asked for again.
    /// @return What the engine is asked: the page; std::nullopt when the page does not change.
    [[nodiscard]] std::optional<UiAction> turnTo(Interaction& interaction,
                                                 KeyContext const& context,
                                                 uint32_t page) noexcept
    {
        auto const pages = pagesOf(interaction, context);
        // Before a page was read, the number of pages is not known: only the first is surely there.
        auto const target = std::clamp(page, uint32_t { 1 }, std::max(pages, uint32_t { 1 }));
        if (interaction.screen != Screen::Transcript || target == interaction.page)
            return std::nullopt;
        interaction.page = target;
        interaction.scroll = 0;
        return UiAction { ReadTranscriptAction { .run = interaction.subject, .page = target } };
    }

    /// Leaves a board or a transcript for the screen it was opened from; one opened from the dashboard
    /// returns there. The screen returned to returns to the dashboard in turn.
    /// @return What the engine is asked: a board returned to is read again now, and a transcript page the
    ///         engine no longer keeps for the tab is asked for again.
    [[nodiscard]] std::optional<UiAction> goBack(Interaction& interaction, KeyContext const& context)
    {
        auto const back = std::exchange(interaction.back, Screen::Dashboard);
        if (back == Screen::Dashboard || back == interaction.screen)
        {
            leaveSubject(interaction);
            return std::nullopt;
        }
        interaction.screen = back;
        interaction.scroll = 0;
        if (back == Screen::Board)
            return UiAction { ShowBoardAction { .run = interaction.subject } };
        auto const* const view = context.engine != nullptr
                                     ? context.engine->transcript(context.tab, interaction.subject)
                                     : nullptr;
        if (view != nullptr && view->page == interaction.page)
            return std::nullopt;
        return UiAction { ReadTranscriptAction { .run = interaction.subject, .page = interaction.page } };
    }

    using Handler = std::optional<UiAction> (*)(Interaction&, KeyContext const&);

    /// One command and what it does.
    struct CommandRow
    {
        KeyCommand command = KeyCommand::Up; ///< The command.
        Handler handler = nullptr;           ///< What it does.
    };

    constexpr auto Commands = std::array {
        // On a board or a transcript the moves scroll it; on the dashboard they move the selection.
        CommandRow {
            .command = KeyCommand::Up,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen != Screen::Dashboard)
                    scrollTo(interaction, context, interaction.scroll == 0 ? 0 : interaction.scroll - 1);
                else
                    select(interaction, context, interaction.selected == 0 ? 0 : interaction.selected - 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Down,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen != Screen::Dashboard)
                    scrollTo(interaction, context, interaction.scroll + 1);
                else
                    select(interaction, context, interaction.selected + 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Home,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen != Screen::Dashboard)
                    scrollTo(interaction, context, 0);
                else
                    select(interaction, context, 0);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::End,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen != Screen::Dashboard)
                    scrollTo(interaction, context, lastScroll(context));
                else
                    select(interaction, context, context.selectable == 0 ? 0 : context.selectable - 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::ScrollPageUp,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const page = pageOf(context);
                scrollTo(interaction, context, interaction.scroll > page ? interaction.scroll - page : 0);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::ScrollPageDown,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                scrollTo(interaction, context, interaction.scroll + pageOf(context));
                return std::nullopt;
            } },
        // On the dashboard, `b` shows the selected run's board; on the board, it reads the board again now.
        CommandRow {
            .command = KeyCommand::Board,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen == Screen::Board)
                {
                    if (!hasBoardToShow(context.engine, interaction.subject))
                        return std::nullopt;
                    return UiAction { ShowBoardAction { .run = interaction.subject } };
                }
                // From a transcript, the board of its run while its worktree is there; Esc then returns to
                // the transcript.
                auto run = interaction.subject;
                if (interaction.screen == Screen::Dashboard)
                {
                    auto const* const row = context.selected;
                    if (row == nullptr || !std::ranges::contains(row->offers, KeyCommand::Board))
                        return std::nullopt;
                    run = row->run;
                }
                else if (!hasBoardToShow(context.engine, run))
                    return std::nullopt;
                interaction.back = interaction.screen;
                interaction.screen = Screen::Board;
                interaction.subject = run;
                interaction.scroll = 0;
                interaction.overlay = Overlay::None;
                return UiAction { ShowBoardAction { .run = std::move(run) } };
            } },
        // `t` shows the transcript of the selected run, or of the board's, from its first page.
        CommandRow {
            .command = KeyCommand::Transcript,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto run = interaction.subject;
                if (interaction.screen == Screen::Dashboard)
                {
                    auto const* const row = context.selected;
                    if (row == nullptr || !std::ranges::contains(row->offers, KeyCommand::Transcript))
                        return std::nullopt;
                    run = row->run;
                }
                // Esc returns to where it was opened from: the dashboard, or the board.
                interaction.back = interaction.screen;
                interaction.screen = Screen::Transcript;
                interaction.subject = run;
                interaction.page = 1;
                interaction.scroll = 0;
                interaction.overlay = Overlay::None;
                return UiAction { ReadTranscriptAction { .run = std::move(run), .page = 1 } };
            } },
        CommandRow { .command = KeyCommand::PreviousPage,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return turnTo(interaction, context, interaction.page - 1);
                         } },
        CommandRow { .command = KeyCommand::NextPage,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return turnTo(interaction, context, interaction.page + 1);
                         } },
        CommandRow { .command = KeyCommand::FirstPage,
                     .handler = [](Interaction& interaction,
                                   KeyContext const& context) { return turnTo(interaction, context, 1); } },
        CommandRow { .command = KeyCommand::LastPage,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return turnTo(interaction, context, pagesOf(interaction, context));
                         } },
        // Enter acts on what the row stands for: an agent is revealed, an item dispatched on (or its agent
        // revealed), a finished run's or an orphan's details opened. Tab shows any row's details.
        CommandRow {
            .command = KeyCommand::Enter,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                // On a board or a transcript, Enter goes to the agent's tab while the run has one.
                if (interaction.screen != Screen::Dashboard)
                {
                    if (!hasAgentTab(context.engine, interaction.subject))
                        return std::nullopt;
                    return UiAction { RevealAgentAction { .run = interaction.subject } };
                }
                auto const* const row = context.selected;
                if (row == nullptr)
                    return std::nullopt;
                switch (row->kind)
                {
                    case RowKind::Group:
                        interaction.followGroup = row->group;
                        return UiAction { ToggleFoldAction { .group = row->group } };
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
        // `d` dispatches on the selected item; on a finished run, a board or a transcript, a new run on the
        // run's item.
        CommandRow {
            .command = KeyCommand::Dispatch,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const row = context.selected;
                if (interaction.screen != Screen::Dashboard)
                    openFormOnRun(interaction, context, interaction.subject);
                else if (row != nullptr && std::ranges::contains(row->offers, KeyCommand::Dispatch))
                    openFormOnRun(interaction, context, row->run);
                else
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
        CommandRow {
            .command = KeyCommand::Back,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                // A board or a transcript returns to where it was opened from (a board left is no
                // longer read: see Dashboard).
                if (interaction.screen != Screen::Dashboard)
                    return goBack(interaction, context);
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
        // `z` folds the group of the selected row, an item's or its header's, and selects the header: the
        // item
        // it was on is gone from the list.
        CommandRow {
            .command = KeyCommand::Fold,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const row = context.selected;
                if (interaction.screen != Screen::Dashboard || row == nullptr || row->group.empty())
                    return std::nullopt;
                interaction.followGroup = row->group;
                return UiAction { ToggleFoldAction { .group = row->group } };
            } },
        // `n` starts an agent on a topic the user types, in the repository the selection is in.
        CommandRow {
            .command = KeyCommand::NewTopic,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.screen != Screen::Dashboard || context.engine == nullptr)
                    return std::nullopt;
                auto repo = std::string_view {};
                if (auto const* const row = context.selected; row != nullptr)
                {
                    repo = row->repo;
                    auto const runs = context.engine->runs();
                    if (auto const run = std::ranges::find(runs, row->run, &Run::id);
                        repo.empty() && run != runs.end())
                        repo = run->repo;
                }
                interaction.form = openTopicForm(*context.engine, repo, {}, {});
                if (!interaction.form)
                    interaction.notice = "no repository is configured";
                return std::nullopt;
            } },
        CommandRow { .command = KeyCommand::Refresh,
                     .handler = [](Interaction&, KeyContext const&) -> std::optional<UiAction> {
                         return UiAction { RefreshNowAction {} };
                     } },
        // `I` installs the plugins the agents lack into the user's own Claude Code; it asks once first.
        CommandRow {
            .command = KeyCommand::InstallPlugins,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (context.engine == nullptr)
                    return std::nullopt;
                auto const missing = context.engine->missingPlugins();
                if (context.engine->installingPlugins())
                {
                    interaction.notice = "installing the plugins…";
                    return std::nullopt;
                }
                if (missing.empty())
                {
                    interaction.notice = "every plugin the tasks need is installed";
                    return std::nullopt;
                }
                if (interaction.pendingInstall != InstallConfirm::Asked)
                {
                    auto ids = std::string {};
                    for (auto const& plugin: missing)
                        ids += (ids.empty() ? "" : ", ") + plugin.id;
                    interaction.pendingInstall = InstallConfirm::Asked;
                    interaction.notice = std::format("I again installs {} with claude plugin install", ids);
                    return std::nullopt;
                }
                interaction.pendingInstall = InstallConfirm::None;
                return UiAction { InstallPluginsAction {} };
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

std::optional<KeyCommand> commandFor(core::tui::KeyEvent const& key, Screen screen) noexcept
{
    if (isChord(key))
        return std::nullopt;
    auto const name = nameOf(key);
    for (auto const& row: KeyTable)
        if (row.key == name && holds(row.on, screen))
            return row.command;
    return std::nullopt;
}

void leaveSubject(Interaction& interaction) noexcept
{
    interaction.screen = Screen::Dashboard;
    interaction.subject.clear();
    interaction.back = Screen::Dashboard;
    interaction.page = 1;
    interaction.scroll = 0;
}

void openFormFor(Interaction& interaction,
                 Engine const& engine,
                 std::string_view repo,
                 ItemRef const& item,
                 std::string_view task,
                 std::string_view topic)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return;
    if (std::holds_alternative<TopicRef>(item))
    {
        // A topic is listed nowhere: the form opens on its text, in its repository.
        interaction.form = openTopicForm(engine, repo, std::string { topic }, {});
        if (interaction.form)
            interaction.overlay = Overlay::None;
        else
            interaction.notice = "no repository is configured";
        return;
    }
    auto const tasks = config->tasks();
    auto const kind = appliesToOf(item);
    auto const* chosen = config->firstTaskFor(kind);
    // The run's own task while the configuration still has it for such items; else the item's first.
    if (auto const named = std::ranges::find(tasks, task, &TaskDef::id);
        named != tasks.end() && named->appliesTo == kind)
        chosen = &*named;
    if (chosen == nullptr)
    {
        interaction.notice = "no task applies to this item";
        return;
    }
    interaction.form = openDispatchForm(engine, repo, item, chosen->id, {});
    if (interaction.form)
        interaction.overlay = Overlay::None;
    else
        interaction.notice = std::format("{} is not listed any more", itemLabel(item));
}

bool hasAgentTab(Engine const* engine, std::string_view run) noexcept
{
    if (engine == nullptr)
        return false;
    auto const runs = engine->runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    return found != runs.end() && countsAsActive(*found) && found->pane.has_value();
}

bool hasBoardToShow(Engine const* engine, std::string_view run) noexcept
{
    if (engine == nullptr)
        return false;
    auto const runs = engine->runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    return found != runs.end() && keepsWorktree(*found);
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
    auto command = commandFor(key, interaction.screen);
    auto taskContext = context;
    // A task's key, from the configuration: the table refuses one the dashboard binds (dashboardKeys()). Only
    // the dashboard dispatches.
    if (auto const* const config = context.engine != nullptr ? context.engine->config() : nullptr;
        !command && config != nullptr && interaction.screen == Screen::Dashboard)
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
    if (interaction.pendingInstall == InstallConfirm::Asked && command != KeyCommand::InstallPlugins)
    {
        interaction.pendingInstall = InstallConfirm::None;
        return std::nullopt;
    }
    return command ? dispatch(*command, interaction, taskContext) : std::nullopt;
}

} // namespace orchestrator::view
