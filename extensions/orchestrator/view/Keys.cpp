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

#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    // {{{ the keys

    // In the order the action bar shows them: the line is cut at the tab's edge, so the keys that find
    // everything else come first.
    constexpr auto KeyTable = std::array {
        KeyRow { .key = "up"sv, .command = KeyCommand::Up, .hint = "↑↓ move"sv, .on = PlaceSet::Fleet },
        KeyRow { .key = "up"sv, .command = KeyCommand::Up, .hint = "↑↓ scroll"sv, .on = PlaceSet::Focus },
        KeyRow { .key = "k"sv, .command = KeyCommand::Up },
        KeyRow { .key = "down"sv, .command = KeyCommand::Down },
        KeyRow { .key = "j"sv, .command = KeyCommand::Down },
        KeyRow { .key = "home"sv, .command = KeyCommand::Home },
        KeyRow { .key = "end"sv, .command = KeyCommand::End },
        KeyRow { .key = "enter"sv, .command = KeyCommand::Enter, .hint = "⏎ open"sv, .on = PlaceSet::Fleet },
        KeyRow { .key = "esc"sv, .command = KeyCommand::Back, .hint = "esc back"sv, .on = PlaceSet::Focus },
        KeyRow { .key = "tab"sv, .command = KeyCommand::Tab, .hint = "tab focus"sv, .on = PlaceSet::Fleet },
        KeyRow { .key = "b"sv, .command = KeyCommand::BoardTab, .hint = "b board"sv },
        KeyRow { .key = "t"sv, .command = KeyCommand::TranscriptTab, .hint = "t transcript"sv },
        KeyRow { .key = "d"sv, .command = KeyCommand::DetailsTab, .hint = "d details"sv },
        KeyRow { .key = "n"sv, .command = KeyCommand::NewAgent, .hint = "n new agent"sv },
        KeyRow { .key = "/"sv, .command = KeyCommand::Filter, .hint = "/ filter"sv, .on = PlaceSet::Fleet },
        KeyRow { .key = "e"sv, .command = KeyCommand::EndRun, .hint = "e end"sv },
        KeyRow { .key = "o"sv, .command = KeyCommand::OpenLink, .hint = "o copy link"sv },
        KeyRow { .key = "r"sv, .command = KeyCommand::Reveal, .hint = "r agent tab"sv },
        KeyRow { .key = "x"sv, .command = KeyCommand::RetryRemoval, .hint = "x retry removal"sv },
        KeyRow { .key = "del"sv, .command = KeyCommand::Dismiss, .hint = "del dismiss"sv },
        KeyRow { .key = "z"sv, .command = KeyCommand::Fold, .hint = "z fold"sv, .on = PlaceSet::Fleet },
        KeyRow { .key = "pgup"sv, .command = KeyCommand::ScrollPageUp },
        KeyRow { .key = "pgdn"sv, .command = KeyCommand::ScrollPageDown },
        KeyRow { .key = "left"sv, .command = KeyCommand::PreviousPage },
        KeyRow { .key = "right"sv, .command = KeyCommand::NextPage },
        KeyRow { .key = "?"sv, .command = KeyCommand::Help, .hint = "? help"sv },
        // The palette's commands from phase 3; keys until then, so that they stay reachable.
        KeyRow { .key = "R"sv, .command = KeyCommand::Refresh, .hint = "R refresh"sv },
        KeyRow { .key = "I"sv, .command = KeyCommand::InstallPlugins, .hint = "I install plugins"sv },
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

    /// @return Whether the selected row offers @p command.
    [[nodiscard]] bool offers(KeyContext const& context, KeyCommand command) noexcept
    {
        return context.selected != nullptr && std::ranges::contains(context.selected->offers, command);
    }

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

    /// @return The run command @p command on the selected row, when the row accepts it.
    template <typename Action>
    [[nodiscard]] std::optional<UiAction> runAction(KeyCommand command, KeyContext const& context)
    {
        if (!offers(context, command))
            return std::nullopt;
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

    /// Opens the launcher on a run's item and task (openLauncherFor()), as running it again does.
    void openLauncherOnRun(Interaction& interaction, KeyContext const& context)
    {
        if (auto const* const run = selectedRun(context))
            openLauncherFor(interaction, *context.engine, run->repo, run->item, run->task, run->topic);
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
        if (tab == TabCommands.end() || !offers(context, command))
            return std::nullopt;
        if (activeTabOf(interaction, context.selected) != tab->tab)
            restartFocus(interaction);
        interaction.chosenTab =
            TabChoice { .run = context.selected->run, .state = context.selected->state, .tab = tab->tab };
        return std::nullopt;
    }

    /// Opens the launcher, empty; or says what is missing while the orchestrator is not configured.
    [[nodiscard]] std::optional<UiAction> openNewAgent(Interaction& interaction, KeyContext const& context)
    {
        if (context.engine == nullptr)
            return std::nullopt;
        interaction.launcher = openLauncher(*context.engine, {}, {});
        if (!interaction.launcher)
            interaction.notice = "the orchestrator is not configured: the line above says what is missing";
        else
            interaction.overlay = Overlay::None;
        return std::nullopt;
    }

    using Handler = std::optional<UiAction> (*)(Interaction&, KeyContext const&);

    /// One command and what it does.
    struct CommandRow
    {
        KeyCommand command = KeyCommand::Up; ///< The command.
        Handler handler = nullptr;           ///< What it does.
    };

    constexpr auto Commands = std::array {
        // In the Fleet the moves move the selection; in the Focus they scroll its tab.
        CommandRow {
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
        CommandRow {
            .command = KeyCommand::Down,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    scrollTo(interaction, context, scrollOf(interaction, context) + 1);
                else
                    select(interaction, context, interaction.selected + 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::Home,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    scrollTo(interaction, context, 0);
                else
                    select(interaction, context, 0);
                return std::nullopt;
            } },
        // `end` in the Focus returns the transcript to the agent's last words.
        CommandRow {
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
        CommandRow {
            .command = KeyCommand::ScrollPageUp,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const from = scrollOf(interaction, context);
                auto const page = pageOf(context);
                scrollTo(interaction, context, from > page ? from - page : 0);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::ScrollPageDown,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                scrollTo(interaction, context, scrollOf(interaction, context) + pageOf(context));
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::PreviousPage,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const page = shownPage(interaction, context);
                turnTo(interaction, context, page > 1 ? page - 1 : 1);
                return std::nullopt;
            } },
        CommandRow {
            .command = KeyCommand::NextPage,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                turnTo(interaction, context, shownPage(interaction, context) + 1);
                return std::nullopt;
            } },
        // `b`, `t` and `d` choose the Focus tab; the choice sticks for the run until its state changes.
        CommandRow { .command = KeyCommand::BoardTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::BoardTab);
                         } },
        CommandRow { .command = KeyCommand::TranscriptTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::TranscriptTab);
                         } },
        CommandRow { .command = KeyCommand::DetailsTab,
                     .handler =
                         [](Interaction& interaction, KeyContext const& context) {
                             return chooseTab(interaction, context, KeyCommand::DetailsTab);
                         } },
        // Enter does the selected row's primary action: the table-driven version is phase 3's.
        CommandRow {
            .command = KeyCommand::Enter,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const row = context.selected;
                if (row == nullptr)
                    return std::nullopt;
                switch (row->kind)
                {
                    case RowKind::Heading: return toggleFold(interaction, context);
                    case RowKind::NewAgent: return openNewAgent(interaction, context);
                    case RowKind::Finished:
                        // A kept worktree is checked again; any other finished run is run again.
                        if (offers(context, KeyCommand::RetryRemoval))
                            return runAction<RetryRemovalAction>(KeyCommand::RetryRemoval, context);
                        openLauncherOnRun(interaction, context);
                        return std::nullopt;
                    case RowKind::Agent:
                    case RowKind::Orphan: toFocus(interaction, context); return std::nullopt;
                    case RowKind::Notice: break;
                }
                return std::nullopt;
            } },
        CommandRow { .command = KeyCommand::Back,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         if (interaction.place == Place::Focus)
                             backToFleet(interaction);
                         else if (interaction.overlay != Overlay::None)
                             interaction.overlay = Overlay::None;
                         else
                             interaction.filter.clear();
                         return std::nullopt;
                     } },
        CommandRow {
            .command = KeyCommand::Tab,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                if (interaction.place == Place::Focus)
                    backToFleet(interaction);
                else
                    toFocus(interaction, context);
                return std::nullopt;
            } },
        CommandRow { .command = KeyCommand::Filter,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.filtering = Filtering::Typing;
                         interaction.place = Place::Fleet;
                         return std::nullopt;
                     } },
        CommandRow { .command = KeyCommand::Help,
                     .handler = [](Interaction& interaction, KeyContext const&) -> std::optional<UiAction> {
                         interaction.overlay =
                             interaction.overlay == Overlay::Help ? Overlay::None : Overlay::Help;
                         return std::nullopt;
                     } },
        // `z` folds the group of the selected row and selects its heading: the run it was on may be gone.
        CommandRow { .command = KeyCommand::Fold, .handler = toggleFold },
        CommandRow { .command = KeyCommand::NewAgent, .handler = openNewAgent },
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
        // D14: ending a run asks once. The first `e` names the run on the action bar; a second on it ends it.
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
        CommandRow { .command = KeyCommand::Reveal,
                     .handler =
                         [](Interaction&, KeyContext const& context) {
                             return runAction<RevealAgentAction>(KeyCommand::Reveal, context);
                         } },
        // `o` copies the run's link with OSC 52 until the frontend can open it (phase 6), and says so.
        CommandRow {
            .command = KeyCommand::OpenLink,
            .handler = [](Interaction& interaction, KeyContext const& context) -> std::optional<UiAction> {
                auto const* const run =
                    offers(context, KeyCommand::OpenLink) ? selectedRun(context) : nullptr;
                if (run == nullptr)
                    return std::nullopt;
                interaction.clipboard = run->url;
                interaction.notice =
                    std::format("copied {}'s link · ctrl+click it to open", itemLabel(run->item));
                return std::nullopt;
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
    if (interaction.launcher)
    {
        if (context.engine != nullptr)
            pasteIntoLauncher(interaction.launcher, *context.engine, pastedText(text));
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
    auto const row = std::ranges::find(KeyTable, name, &KeyRow::key);
    return row != KeyTable.end() ? std::optional { row->command } : std::nullopt;
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
    return row != nullptr && row->kind == RowKind::Agent && hasAgentTab(engine, row->run);
}

void backToFleet(Interaction& interaction) noexcept
{
    interaction.place = Place::Fleet;
}

void openLauncherFor(Interaction& interaction,
                     Engine const& engine,
                     std::string_view repo,
                     ItemRef const& item,
                     std::string_view task,
                     std::string_view topic)
{
    // What each refusal leaves on the action bar, indexed by LauncherRefusal; `{}` is the item's label.
    constexpr auto RefusalNotices = std::array {
        "the orchestrator is not configured"sv, // NotConfigured
        "{} is not listed any more"sv,          // NotListed
        "no task applies to this item"sv,       // NoTask
    };
    static_assert(RefusalNotices.size() == std::to_underlying(LauncherRefusal::NoTask) + 1U);

    if (std::holds_alternative<TopicRef>(item))
    {
        // A topic is listed nowhere: the launcher opens on its text, which its topic row then holds.
        auto launcher = openLauncher(engine, std::string { topic }, {});
        if (!launcher)
        {
            interaction.notice = RefusalNotices[std::to_underlying(LauncherRefusal::NotConfigured)];
            return;
        }
        interaction.launcher = std::move(launcher);
        interaction.overlay = Overlay::None;
        return;
    }
    auto launcher = openLauncherOn(engine, repo, item, task, {});
    if (!launcher)
    {
        auto const label = itemLabel(item);
        interaction.notice =
            std::vformat(RefusalNotices[std::to_underlying(launcher.error())], std::make_format_args(label));
        return;
    }
    interaction.launcher = std::move(*launcher);
    interaction.overlay = Overlay::None;
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
    auto const row = std::ranges::find(Commands, command, &CommandRow::command);
    return row != Commands.end() ? row->handler(interaction, context) : std::nullopt;
}

bool isDispatched(KeyCommand command) noexcept
{
    return std::ranges::contains(Commands, command, &CommandRow::command);
}

namespace
{
    /// Types @p key into the message box, while it has the keys.
    /// @return Nothing when it does not have them, or the key scrolls the Focus's tab (`pgup`, `pgdn`, `end`,
    ///         `←`, `→`) or leaves it (`tab`), so that the key table decides; else what the key asks: the
    ///         message to send, or nothing.
    [[nodiscard]] std::optional<std::optional<UiAction>> typeIntoMessage(core::tui::KeyEvent const& key,
                                                                         Interaction& interaction,
                                                                         KeyContext const& context)
    {
        if (interaction.place != Place::Focus || !hasMessageBox(context.engine, context.selected))
            return std::nullopt;
        switch (key.key)
        {
            // The box has no cursor to move yet: ← and → turn the transcript's pages, as in the Fleet.
            case core::tui::KeyCode::PageUp:
            case core::tui::KeyCode::PageDown:
            case core::tui::KeyCode::End:
            case core::tui::KeyCode::Left:
            case core::tui::KeyCode::Right:
            case core::tui::KeyCode::Tab: return std::nullopt;
            default: break;
        }
        auto const& run = context.selected->run;
        auto& draft = interaction.drafts[run];
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
        else if (key.key == core::tui::KeyCode::Backspace)
            dropLastCodepoint(draft);
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
    if (auto typed = typeIntoMessage(key, interaction, context))
        return std::move(*typed);
    if (typeIntoFilter(key, interaction) == FilterInput::Taken)
        return std::nullopt;
    auto const command = commandFor(key);
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
    return command ? dispatch(*command, interaction, context) : std::nullopt;
}

} // namespace orchestrator::view
