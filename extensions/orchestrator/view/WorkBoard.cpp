// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/WorkBoard.hpp>

#include <core/tui/Canvas.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/NavInput.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/StartDialog.hpp>
#include <orchestrator/view/Theme.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    using core::tui::Canvas;
    using core::tui::Rect;

    // {{{ the tables

    /// How a group's heading is drawn, indexed by WorkGroup.
    constexpr auto GroupRoles = std::array {
        Role::Warning, // NeedsYou
        Role::Passing, // ReadyToMerge
        Role::Working, // InReview
        Role::Text,    // NoMrYet
        Role::Dim,     // EverythingElse
    };
    static_assert(GroupRoles.size() == WorkGroupRows.size());

    /// A merge request's pipeline, indexed by CiState: `✗ pipeline`, `◌ running`, `✓ pipeline`, `·`.
    constexpr auto PipelineLooks = std::array {
        Look { .mark = "·"sv, .role = Role::Dim, .word = {} },
        Look { .mark = "◌"sv, .role = Role::Working, .word = "running"sv },
        Look { .mark = "✓"sv, .role = Role::Passing, .word = "pipeline"sv },
        Look { .mark = "✗"sv, .role = Role::Failing, .word = "pipeline"sv },
    };
    static_assert(PipelineLooks.size() == std::to_underlying(CiState::Failing) + 1U);

    /// A merge request's review, indexed by ReviewState: `· no review`, `… review`, `✓ approved`,
    /// `⟲ changes`. An inferred None is drawn as Required (reviewLook()).
    constexpr auto ReviewCellLooks = std::array {
        Look { .mark = "·"sv, .role = Role::Dim, .word = "no review"sv },
        Look { .mark = "…"sv, .role = Role::Dim, .word = "review"sv },
        Look { .mark = "✓"sv, .role = Role::Passing, .word = "approved"sv },
        Look { .mark = "⟲"sv, .role = Role::Warning, .word = "changes"sv },
    };
    static_assert(ReviewCellLooks.size() == std::to_underlying(ReviewState::ChangesRequested) + 1U);

    /// A conflict wins the review cell.
    constexpr auto ConflictLook = Look { .mark = "⚠"sv, .role = Role::Warning, .word = "conflicts"sv };

    /// A cell with nothing to say.
    constexpr auto NothingLook = Look { .mark = "·"sv, .role = Role::Dim, .word = {} };

    /// How a ticket's status is drawn, indexed by StatusCategory.
    constexpr auto StatusRoles = std::array {
        Role::Dim,     // Unknown
        Role::Dim,     // ToDo
        Role::Working, // InProgress
        Role::Passing, // Done
    };
    static_assert(StatusRoles.size() == std::to_underlying(StatusCategory::Done) + 1U);

    /// How much of a row a narrow list keeps: each level drops a little more, in this order.
    enum class Detail : uint8_t
    {
        Full = 0,      ///< Every cell, with its words.
        NoAge,         ///< The Agent column's age dropped; a `⬢ worker` stays.
        ReviewGlyph,   ///< The Review column as its glyph alone.
        PipelineGlyph, ///< The Pipeline column as its glyph alone.
    };

    constexpr auto Details =
        std::array { Detail::Full, Detail::NoAge, Detail::ReviewGlyph, Detail::PipelineGlyph };

    /// The narrowest tab that keeps the Ticket column.
    constexpr auto TicketFromColumns = 60;
    /// The fewest cells the title keeps before a narrower level is chosen.
    constexpr auto MinTitleColumns = 16;
    /// The widest the MR, Ticket, Jira and Agent columns grow.
    constexpr auto MaxMrColumns = 7;
    constexpr auto MaxTicketColumns = 12;
    constexpr auto MaxJiraColumns = 18;
    /// The narrowest the Jira column gets (`◆ In Pro…`), and the share of the list it takes at most.
    constexpr auto MinJiraColumns = 9;
    constexpr auto JiraShare = 5;
    constexpr auto MaxAgentColumns = 12;
    /// The Pipeline and Review columns with their words: `✗ pipeline`, `⚠ conflicts`.
    constexpr auto PipelineColumns = 10;
    constexpr auto ReviewColumns = 11;
    /// The Agent column with an age: `⬢ <worker>` or `12m`.
    constexpr auto AgeColumns = 4;

    /// One page of the list less a line that stays in view.
    [[nodiscard]] std::size_t pageOf(std::size_t pageRows) noexcept
    {
        return pageRows > 1 ? pageRows - 1 : 1;
    }

    /// What the filter and the repository chip let through.
    class ItemFilter
    {
      public:
        ItemFilter(Engine const& engine, WorkState const& state):
            _filter { state.filter }, _repo { chipRepository(state, engine.repositories()) }
        {
        }

        [[nodiscard]] bool shows(WorkItem const& item) const
        {
            return (_repo.empty() || item.repo == _repo) && matchesFilter(item, _filter);
        }

      private:
        std::string_view _filter;
        std::string_view _repo;
    };

    // }}}
    // {{{ the cells

    /// @return The forge of the repository named @p repo; GitHub when it is not configured.
    [[nodiscard]] Forge forgeOf(Engine const& engine, std::string_view repo) noexcept
    {
        auto const* const config = engine.config();
        if (config == nullptr)
            return Forge::GitHub;
        auto const found = std::ranges::find(config->repos(), repo, &Repo::name);
        return found != config->repos().end() ? found->forge : Forge::GitHub;
    }

    /// @return The Pipeline cell's look.
    [[nodiscard]] Look pipelineLook(WorkItem const& item) noexcept
    {
        return item.changeRequest ? entryOf(PipelineLooks, item.changeRequest->ci) : NothingLook;
    }

    /// @return The Review cell's look: a conflict wins.
    [[nodiscard]] Look reviewLook(WorkItem const& item) noexcept
    {
        if (!item.changeRequest)
            return NothingLook;
        if (item.changeRequest->conflicts == Conflicts::Conflicting)
            return ConflictLook;
        auto const& mr = *item.changeRequest;
        // An inferred None is GitLab's merge status naming no review, not a repository that asks for none.
        if (mr.review == ReviewState::None && mr.reviewSource == StateSource::Inferred)
            return entryOf(ReviewCellLooks, ReviewState::Required);
        return entryOf(ReviewCellLooks, mr.review);
    }

    /// A cell's text and how it is drawn.
    struct Cell
    {
        std::string text;      ///< What it says; empty: nothing.
        Role role = Role::Dim; ///< How it is drawn.
    };

    /// @return The Jira cell: `◆ <status>` for a Jira ticket, `● open` for a forge issue, `◆ ?` for a key
    ///         without a status, `·` for none.
    [[nodiscard]] Cell jiraCell(WorkItem const& item)
    {
        if (item.ticket && std::holds_alternative<IssueRef>(item.ticket->item))
            return Cell { .text = "● open", .role = Role::Working };
        if (item.ticket && !item.ticket->status.empty())
            return Cell { .text = std::format("◆ {}", item.ticket->status),
                          .role = entryOf(StatusRoles, item.ticket->category) };
        if (!item.ticketKey.empty())
            return Cell { .text = "◆ ?", .role = Role::Dim };
        return Cell { .text = "·", .role = Role::Dim };
    }

    /// @return The age of an item's last update the Agent cell shows: none while a run works on it, or
    ///         when its update time is unknown.
    [[nodiscard]] std::optional<uint64_t> shownAgeOf(WorkItem const& item, uint64_t nowWallMs) noexcept
    {
        if (!item.agentRun.empty() || item.updatedWallMs == 0)
            return std::nullopt;
        return nowWallMs > item.updatedWallMs ? nowWallMs - item.updatedWallMs : 0;
    }

    /// @return The Agent cell: `⬢ <worker>` while a run works on the item; `⚙ queued` while automation waits
    ///         for a free slot to start on it; else, at Detail::Full, the age of its last update, or `·` when
    ///         that is unknown; else nothing.
    [[nodiscard]] Cell agentCell(Engine const& engine,
                                 WorkItem const& item,
                                 uint64_t nowWallMs,
                                 Detail detail)
    {
        if (!item.agentRun.empty())
            return Cell { .text = std::format("⬢ {}", item.agentWorker), .role = Role::Working };
        if (engine.isQueuedForAutomation(workItemKey(item)))
            return Cell { .text = "⚙ queued", .role = Role::Dim };
        if (detail != Detail::Full)
            return {};
        if (auto const age = shownAgeOf(item, nowWallMs))
            return Cell { .text = updateAgeText(*age), .role = Role::Dim };
        return Cell { .text = "·", .role = Role::Dim };
    }

    /// The widths a list's columns take, a level of detail chosen.
    struct Columns
    {
        Detail detail = Detail::Full; ///< What narrowing chose.
        int gap = 1;                  ///< The cells between two columns.
        int mr = 0;                   ///< 0: no MR row is listed.
        int ticket = 0;               ///< 0: dropped.
        int title = 0;                ///< What is left.
        int pipeline = 0;             ///< PipelineColumns, or its glyph's 1.
        int review = 0;               ///< ReviewColumns, or its glyph's 1.
        int jira = 0;                 ///< At most MaxJiraColumns, narrowed with the list.
        int agent = 0;                ///< 0: dropped.
    };

    /// @return The widths of the columns for a list @p width wide, in a tab @p tabColumns wide: the first
    ///         level of detail whose title keeps MinTitleColumns, else the last.
    [[nodiscard]] Columns columnsFor(Engine const& engine,
                                     std::span<WorkItem const> items,
                                     int width,
                                     int tabColumns)
    {
        auto widest = Columns {};
        auto workers = 0;
        for (auto const& item: items)
        {
            widest.mr = std::max(widest.mr, columnsOf(workMrLabel(engine, item)));
            widest.ticket = std::max(widest.ticket, columnsOf(item.ticketKey));
            widest.jira = std::max(widest.jira, columnsOf(jiraCell(item).text));
            workers = std::max(workers, columnsOf(agentCell(engine, item, 0, Detail::NoAge).text));
        }
        auto columns = Columns {};
        // A wide list spaces its columns out by two cells.
        columns.gap = width >= WideFromColumns ? 2 : 1;
        columns.mr = std::min(widest.mr, MaxMrColumns);
        columns.ticket =
            tabColumns >= TicketFromColumns ? std::min(std::max(widest.ticket, 1), MaxTicketColumns) : 0;
        // The Jira column narrows with the list, so that a narrow one keeps room for the title.
        columns.jira = std::min({ widest.jira, MaxJiraColumns, std::max(MinJiraColumns, width / JiraShare) });
        for (auto const detail: Details)
        {
            columns.detail = detail;
            columns.pipeline = detail >= Detail::PipelineGlyph ? 1 : PipelineColumns;
            columns.review = detail >= Detail::ReviewGlyph ? 1 : ReviewColumns;
            columns.agent =
                std::min(detail == Detail::Full ? std::max(workers, AgeColumns) : workers, MaxAgentColumns);
            auto used = 0;
            auto count = 0;
            for (auto const cells: { columns.mr,
                                     columns.ticket,
                                     columns.pipeline,
                                     columns.review,
                                     columns.jira,
                                     columns.agent })
                if (cells > 0)
                {
                    used += cells;
                    ++count;
                }
            columns.title = width - used - (count * columns.gap);
            if (columns.title >= MinTitleColumns)
                break;
        }
        columns.title = std::max(columns.title, 1);
        return columns;
    }

    // }}}
    // {{{ the selection

    /// Scrolls @p state so that its selection is in view, with no blank lines below the last one.
    void scrollIntoView(WorkState& state, std::size_t lineCount, std::size_t pageRows) noexcept
    {
        if (pageRows == 0)
            return;
        state.scroll = std::min(state.scroll, state.selected);
        if (state.selected >= state.scroll + pageRows)
            state.scroll = state.selected + 1 - pageRows;
        state.scroll = std::min(state.scroll, lineCount > pageRows ? lineCount - pageRows : 0);
    }

    /// The lines and items a command acts on.
    struct WorkContext
    {
        std::span<WorkLine const> lines; ///< The lines as drawn.
        std::span<WorkItem const> items; ///< The items they index.
        Engine const& engine;            ///< Its repositories and their forges.
        std::size_t pageRows = 0;        ///< How many lines the list shows.
    };

    /// Selects line @p index, kept within the lines.
    void selectLine(WorkState& state, WorkContext const& context, std::size_t index)
    {
        if (context.lines.empty())
            return;
        state.selected = std::min(index, context.lines.size() - 1);
        state.selectedKey = workLineKey(context.lines[state.selected], context.items);
        scrollIntoView(state, context.lines.size(), context.pageRows);
    }

    /// @return The selected line; nullptr when there is none.
    [[nodiscard]] WorkLine const* selectedLine(WorkState const& state,
                                               std::span<WorkLine const> lines) noexcept
    {
        return state.selected < lines.size() ? &lines[state.selected] : nullptr;
    }

    /// Steps the repository chip by @p step over every repository and the configured ones.
    void stepRepoChip(WorkState& state, Engine const& engine, std::ptrdiff_t step) noexcept
    {
        auto const choices = static_cast<std::ptrdiff_t>(engine.repositories().size()) + 1;
        auto const next = (static_cast<std::ptrdiff_t>(state.repoChip) + step + choices) % choices;
        state.repoChip = static_cast<std::size_t>(next);
    }

    /// Steps the chip with the keys by @p step: the sort chip toggles the sort, any other the repository's.
    /// @return What the engine is asked: the sort toggled, or nothing.
    [[nodiscard]] std::vector<UiAction> stepChip(WorkState& state, Engine const& engine, std::ptrdiff_t step)
    {
        if (state.focus == WorkFocus::SortChip)
            return { ToggleFoldAction { .group = std::string { SortByUpdateKey } } };
        if (state.focus == WorkFocus::AutomationChip)
            return { toggledAutomation(engine) };
        stepRepoChip(state, engine, step);
        return {};
    }

    /// What `o` opens: the selected MR, else the selected ticket.
    [[nodiscard]] std::vector<UiAction> openSelected(Interaction& interaction, WorkContext const& context)
    {
        auto const* const line = selectedLine(interaction.work, context.lines);
        if (line == nullptr || line->kind != WorkLineKind::Item)
        {
            interaction.notice = "o open · select a merge request or a ticket";
            return {};
        }
        auto const& item = context.items[line->item];
        if (item.changeRequest)
            return { OpenUrlAction { .url = item.changeRequest->url,
                                     .label = workMrLabel(context.engine, item) } };
        if (item.ticket)
            return { OpenUrlAction { .url = item.ticket->url, .label = item.ticketKey } };
        return {};
    }

    /// What `m` opens: the row menu on the selected item, below its row once the dashboard places it.
    [[nodiscard]] std::vector<UiAction> openMenu(Interaction& interaction, WorkContext const& context)
    {
        auto const* const line = selectedLine(interaction.work, context.lines);
        if (line == nullptr || line->kind != WorkLineKind::Item)
        {
            interaction.notice = "m menu · select a merge request or a ticket";
            return {};
        }
        interaction.work.menu = RowMenuState { .itemKey = workItemKey(context.items[line->item]) };
        return {};
    }

    /// Keeps the selection among the lines shown once the list scrolled: on the first one shown when it was
    /// above them, on the last one when it was below.
    void keepSelectionShown(WorkState& state, WorkBoardData const& data)
    {
        if (data.lines.empty() || data.listRows == 0)
            return;
        auto const last = std::min(data.lines.size(), state.scroll + data.listRows) - 1;
        auto const kept = std::clamp(state.selected, state.scroll, last);
        if (kept == state.selected)
            return;
        state.selected = kept;
        state.selectedKey = workLineKey(data.lines[kept], data.items);
    }

    /// @return The furthest the list scrolls: its last line at the bottom.
    [[nodiscard]] std::size_t maxScrollOf(WorkBoardData const& data) noexcept
    {
        return data.lines.size() > data.listRows ? data.lines.size() - data.listRows : 0;
    }

    using WorkHandler = std::vector<UiAction> (*)(Interaction&, WorkContext const&);

    /// One Work-view command and what it does.
    struct WorkHandlerRow
    {
        KeyCommand command = KeyCommand::Up; ///< The command.
        WorkHandler handler = nullptr;       ///< What it does.
    };

    constexpr auto WorkHandlers = std::array {
        WorkHandlerRow {
            .command = KeyCommand::Up,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const at = interaction.work.selected;
                selectLine(interaction.work, context, at == 0 ? 0 : at - 1);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::Down,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                selectLine(interaction.work, context, interaction.work.selected + 1);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::Home,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                selectLine(interaction.work, context, 0);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::End,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                selectLine(interaction.work, context, context.lines.empty() ? 0 : context.lines.size() - 1);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::ScrollPageUp,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const at = interaction.work.selected;
                auto const page = pageOf(context.pageRows);
                selectLine(interaction.work, context, at > page ? at - page : 0);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::ScrollPageDown,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                selectLine(interaction.work, context, interaction.work.selected + pageOf(context.pageRows));
                return {};
            } },
        // Half a page: by half the lines the list shows, at least one.
        WorkHandlerRow {
            .command = KeyCommand::HalfPageUp,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const at = interaction.work.selected;
                auto const half = std::max<std::size_t>(1, context.pageRows / 2);
                selectLine(interaction.work, context, at > half ? at - half : 0);
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::HalfPageDown,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                selectLine(interaction.work,
                           context,
                           interaction.work.selected + std::max<std::size_t>(1, context.pageRows / 2));
                return {};
            } },
        // ⏎ folds on a heading, steps a chip that has the keys; on an item, opens the start dialog.
        WorkHandlerRow {
            .command = KeyCommand::Enter,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto& work = interaction.work;
                switch (work.focus)
                {
                    case WorkFocus::RepoChip: stepRepoChip(work, context.engine, 1); return {};
                    case WorkFocus::SortChip:
                        return { ToggleFoldAction { .group = std::string { SortByUpdateKey } } };
                    case WorkFocus::AutomationChip: return { toggledAutomation(context.engine) };
                    case WorkFocus::List:
                    case WorkFocus::Filter: break;
                }
                auto const* const line = selectedLine(work, context.lines);
                if (line == nullptr)
                    return {};
                if (line->kind == WorkLineKind::Heading)
                    return { foldWorkGroup(work, context.engine, line->group) };
                // Its session id is left empty for the dashboard, which holds the source of them, to mint.
                auto const& item = context.items[line->item];
                auto dialog = openStartDialog(context.engine, item, {}, {});
                if (dialog)
                    interaction.startDialog = std::move(*dialog);
                else
                    interaction.notice = startRefusalNotice(dialog.error(), itemLabel(item.item));
                return {};
            } },
        WorkHandlerRow {
            .command = KeyCommand::Fold,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const* const line = selectedLine(interaction.work, context.lines);
                if (line == nullptr)
                    return {};
                return { foldWorkGroup(interaction.work, context.engine, line->group) };
            } },
        WorkHandlerRow {
            .command = KeyCommand::Filter,
            .handler = [](Interaction& interaction, WorkContext const&) -> std::vector<UiAction> {
                interaction.work.filtering = Filtering::Typing;
                interaction.work.focus = WorkFocus::Filter;
                return {};
            } },
        // esc in the list clears the filter (the filter's own esc keeps it), and gives the list the keys.
        WorkHandlerRow {
            .command = KeyCommand::Back,
            .handler = [](Interaction& interaction, WorkContext const&) -> std::vector<UiAction> {
                interaction.work.filter.clear();
                interaction.work.filtering = Filtering::Off;
                interaction.work.focus = WorkFocus::List;
                return {};
            } },
        WorkHandlerRow { .command = KeyCommand::Sort,
                         .handler = [](Interaction&, WorkContext const&) -> std::vector<UiAction> {
                             return { ToggleFoldAction { .group = std::string { SortByUpdateKey } } };
                         } },
        // ← and → step the chip with the keys: the sort chip has two values, so either way toggles it;
        // anywhere else they step the repository chip (Decision 16).
        WorkHandlerRow {
            .command = KeyCommand::RepoPrevious,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                return stepChip(interaction.work, context.engine, -1);
            } },
        WorkHandlerRow {
            .command = KeyCommand::RepoNext,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                return stepChip(interaction.work, context.engine, 1);
            } },
        // The group of the selected line, as shown: its items no agent works on.
        WorkHandlerRow {
            .command = KeyCommand::RunOnGroup,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const* const line = selectedLine(interaction.work, context.lines);
                if (line == nullptr)
                    return {};
                auto const group = line->group;
                auto const shown =
                    context.lines | std::views::filter([&](WorkLine const& each) {
                        return each.kind == WorkLineKind::Item && each.group == group;
                    })
                    | std::views::transform([&](WorkLine const& each) { return context.items[each.item]; })
                    | std::ranges::to<std::vector>();
                if (auto dialog = openBulkDialog(context.engine, group, shown))
                {
                    interaction.bulk = std::move(*dialog);
                    interaction.overlay = Overlay::Bulk;
                }
                else
                    interaction.notice = "every item of this group has an agent, or it is folded";
                return {};
            } },
        WorkHandlerRow { .command = KeyCommand::ToggleAutomation,
                         .handler = [](Interaction&, WorkContext const& context) -> std::vector<UiAction> {
                             if (!context.engine.hasAutomationRules())
                                 return {};
                             return { toggledAutomation(context.engine) };
                         } },
        WorkHandlerRow { .command = KeyCommand::OpenLink, .handler = openSelected },
        WorkHandlerRow { .command = KeyCommand::RowMenu, .handler = openMenu },
        // Every group shown open but the selected line's folds.
        WorkHandlerRow {
            .command = KeyCommand::FoldOthers,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto const* const line = selectedLine(interaction.work, context.lines);
                if (line == nullptr)
                    return {};
                return foldOtherGroups(interaction.work, context.lines, context.engine, line->group);
            } },
        // tab moves the keys along the list and the filter bar's chips; the filter types while it has them.
        WorkHandlerRow {
            .command = KeyCommand::Tab,
            .handler = [](Interaction& interaction, WorkContext const& context) -> std::vector<UiAction> {
                auto& work = interaction.work;
                // The automation chip is shown, and so takes the keys, only while rules are configured.
                auto const last =
                    context.engine.hasAutomationRules() ? WorkFocus::AutomationChip : WorkFocus::SortChip;
                auto const Count = std::to_underlying(last) + 1;
                work.focus = static_cast<WorkFocus>((std::to_underlying(work.focus) + 1) % Count);
                work.filtering = work.focus == WorkFocus::Filter ? Filtering::Typing : Filtering::Off;
                return {};
            } },
    };

    // }}}
} // namespace

bool isGroupFolded(Engine const& engine, WorkGroup group) noexcept
{
    auto const& row = WorkGroupRows[std::to_underlying(group)];
    if (auto const choice = engine.foldChoiceOf(row.foldKey))
        return *choice == FoldChoice::Folded;
    auto const builtIn = row.fold == orchestrator::FoldDefault::Folded;
    // A fold key stored before choices toggled the row's built-in default, whatever `work.folded` says now.
    if (engine.isFolded(row.foldKey))
        return !builtIn;
    auto const* const config = engine.config();
    return config != nullptr ? std::ranges::contains(config->work().folded, group) : builtIn;
}

WorkSort sortOf(Engine const& engine) noexcept
{
    return engine.isFolded(SortByUpdateKey) ? WorkSort::LastUpdate : WorkSort::Importance;
}

std::string workMrLabel(Engine const& engine, WorkItem const& item)
{
    if (!item.changeRequest)
        return {};
    return std::format(
        "{}{}", forgeOf(engine, item.repo) == Forge::GitLab ? "!" : "#", item.changeRequest->number);
}

UiAction foldWorkGroup(WorkState& state, Engine const& engine, WorkGroup group)
{
    state.selectedKey = workLineKey(WorkLine { .kind = WorkLineKind::Heading, .group = group }, {});
    return SetFoldAction { .group = std::string { WorkGroupRows[std::to_underlying(group)].foldKey },
                           .choice = isGroupFolded(engine, group) ? FoldChoice::Open : FoldChoice::Folded };
}

void selectWorkLine(WorkState& state, WorkBoardData const& data, std::size_t index)
{
    if (data.lines.empty())
        return;
    state.selected = std::min(index, data.lines.size() - 1);
    state.selectedKey = workLineKey(data.lines[state.selected], data.items);
    scrollIntoView(state, data.lines.size(), data.listRows);
}

void scrollWorkList(WorkState& state, WorkBoardData const& data, std::ptrdiff_t lines)
{
    auto const scrolled = std::clamp(static_cast<std::ptrdiff_t>(state.scroll) + lines,
                                     std::ptrdiff_t { 0 },
                                     static_cast<std::ptrdiff_t>(maxScrollOf(data)));
    state.scroll = static_cast<std::size_t>(scrolled);
    keepSelectionShown(state, data);
}

void jumpWorkList(WorkState& state, WorkBoardData const& data, std::size_t row)
{
    if (data.listRows == 0)
        return;
    auto const bottom = data.listRows - 1;
    auto const maxScroll = maxScrollOf(data);
    // The scrollbar's top cell is the list's start, its bottom cell its end; the cells between, in
    // proportion.
    state.scroll = bottom == 0 ? 0 : ((std::min(row, bottom) * maxScroll) + (bottom / 2)) / bottom;
    keepSelectionShown(state, data);
}

std::vector<WorkLine> workLines(std::span<WorkItem const> items,
                                std::span<std::size_t const> order,
                                Engine const& engine,
                                WorkState const& state)
{
    auto const filter = ItemFilter { engine, state };
    auto const shown = [&](std::size_t index) {
        return filter.shows(items[index]);
    };
    auto lines = std::vector<WorkLine> {};
    if (sortOf(engine) == WorkSort::LastUpdate)
    {
        for (auto const index: order)
            if (shown(index))
                lines.push_back(
                    WorkLine { .kind = WorkLineKind::Item, .group = items[index].group, .item = index });
        return lines;
    }
    auto const* const config = engine.config();
    auto const empty = config != nullptr ? config->work().empty : EmptyGroups::Hidden;
    for (auto const& row: WorkGroupRows)
    {
        auto const heading = lines.size();
        lines.push_back(WorkLine { .kind = WorkLineKind::Heading, .group = row.group, .item = 0 });
        for (auto const index: order)
            if (items[index].group == row.group && shown(index))
                lines.push_back(WorkLine { .kind = WorkLineKind::Item, .group = row.group, .item = index });
        if (lines.size() == heading + 1 && empty == EmptyGroups::Hidden)
            lines.pop_back();
        else if (isGroupFolded(engine, row.group))
            lines.resize(heading + 1);
    }
    return lines;
}

std::string workLineKey(WorkLine const& line, std::span<WorkItem const> items)
{
    if (line.kind == WorkLineKind::Heading)
        return std::format("group:{}", WorkGroupRows[std::to_underlying(line.group)].foldKey);
    return line.item < items.size() ? workItemKey(items[line.item]) : std::string {};
}

void keepWorkSelection(WorkState& state,
                       std::span<WorkLine const> lines,
                       std::span<WorkItem const> items,
                       std::size_t pageRows)
{
    if (lines.empty())
    {
        state.selected = 0;
        state.scroll = 0;
        return;
    }
    auto found = std::optional<std::size_t> {};
    for (auto const [at, line]: std::views::enumerate(lines))
    {
        auto const matches = state.selectedKey.empty() ? line.kind == WorkLineKind::Item
                                                       : workLineKey(line, items) == state.selectedKey;
        if (matches)
        {
            found = static_cast<std::size_t>(at);
            break;
        }
    }
    // A line that is gone leaves the selection where it was, on the line that took its place.
    state.selected = std::min(found.value_or(state.selected), lines.size() - 1);
    state.selectedKey = workLineKey(lines[state.selected], items);
    scrollIntoView(state, lines.size(), pageRows);
}

namespace
{
    /// One unit an age is told in: below @ref below, in @ref unit, spelt with @ref suffix.
    struct AgeUnit
    {
        uint64_t below = 0;           ///< The first age the next unit tells.
        uint64_t unit = 0;            ///< Its length in milliseconds.
        std::string_view suffix = {}; ///< Such as `m`.
    };

    constexpr auto SecondMs = uint64_t { 1'000 };
    constexpr auto MinuteMs = 60 * SecondMs;
    constexpr auto HourMs = 60 * MinuteMs;
    constexpr auto DayMs = 24 * HourMs;

    constexpr auto AgeUnits = std::array {
        AgeUnit { .below = MinuteMs, .unit = SecondMs, .suffix = "s"sv },
        AgeUnit { .below = HourMs, .unit = MinuteMs, .suffix = "m"sv },
        AgeUnit { .below = DayMs, .unit = HourMs, .suffix = "h"sv },
        AgeUnit { .below = std::numeric_limits<uint64_t>::max(), .unit = DayMs, .suffix = "d"sv },
    };

    /// @return The unit @p ms is told in.
    [[nodiscard]] AgeUnit const& ageUnitOf(uint64_t ms) noexcept
    {
        return *std::ranges::find_if(AgeUnits, [ms](AgeUnit const& unit) { return ms < unit.below; });
    }
} // namespace

std::string updateAgeText(uint64_t ms)
{
    auto const& unit = ageUnitOf(ms);
    return std::format("{}{}", ms / unit.unit, unit.suffix);
}

uint64_t untilAgeChanges(uint64_t ms) noexcept
{
    auto const unit = ageUnitOf(ms).unit;
    return unit - (ms % unit);
}

std::vector<UiAction> applyWorkCommand(KeyCommand command,
                                       Interaction& interaction,
                                       std::span<WorkLine const> lines,
                                       std::span<WorkItem const> items,
                                       Engine const& engine,
                                       std::size_t pageRows)
{
    // The commands both views share act as they do in the Agents view.
    if (command == KeyCommand::SwitchView || command == KeyCommand::NewTask || command == KeyCommand::Palette)
    {
        auto action = dispatch(command, interaction, KeyContext { .engine = &engine });
        return action ? std::vector { std::move(*action) } : std::vector<UiAction> {};
    }
    auto const context =
        WorkContext { .lines = lines, .items = items, .engine = engine, .pageRows = pageRows };
    auto const row = std::ranges::find(WorkHandlers, command, &WorkHandlerRow::command);
    return row != WorkHandlers.end() ? row->handler(interaction, context) : std::vector<UiAction> {};
}

bool isWorkCommand(KeyCommand command) noexcept
{
    return std::ranges::contains(WorkHandlers, command, &WorkHandlerRow::command);
}

std::vector<UiAction> foldOtherGroups(WorkState& state,
                                      std::span<WorkLine const> lines,
                                      Engine const& engine,
                                      WorkGroup group)
{
    // Each fold would select its heading: the selection stays where it was.
    auto const selected = state.selectedKey;
    auto actions = std::vector<UiAction> {};
    for (auto const& line: lines)
        if (line.kind == WorkLineKind::Heading && line.group != group && !isGroupFolded(engine, line.group))
            actions.push_back(foldWorkGroup(state, engine, line.group));
    state.selectedKey = selected;
    return actions;
}

std::vector<UiAction> applyWorkKey(core::tui::KeyEvent const& key,
                                   std::optional<NavAction> nav,
                                   Interaction& interaction,
                                   std::span<WorkLine const> lines,
                                   std::span<WorkItem const> items,
                                   Engine const& engine,
                                   std::size_t pageRows)
{
    auto& work = interaction.work;
    if (work.filtering == Filtering::Typing)
    {
        auto const& keys = keysInForce(engine);
        if (typeIntoFilter(key, typedNavOf(key, nav, keys), work.filtering, work.filter)
            == FilterInput::Taken)
        {
            // Left, the filter is kept; the list has the keys, and its esc clears it.
            if (work.filtering == Filtering::Off)
                work.focus = WorkFocus::List;
            return {};
        }
        // A key the filter passed on acts as it would with nothing typed: ↓ still moves the selection.
        nav = passedKeyAction(key, keys);
    }
    auto const listCommand = nav ? listCommandOf(*nav, View::Work) : std::nullopt;
    auto const command = listCommand ? listCommand : workCommandFor(key);
    return command ? applyWorkCommand(*command, interaction, lines, items, engine, pageRows)
                   : std::vector<UiAction> {};
}

// {{{ the view

namespace
{
    /// Draws a cell of @p columns at @p x, cut to fit; returns the column after it and the gap.
    int putCell(Canvas& canvas,
                int line,
                int x,
                int columns,
                std::string_view text,
                core::tui::Style const& style,
                int gap)
    {
        if (columns <= 0)
            return x;
        canvas.putString(line, x, cut(text, columns), style);
        return x + columns + gap;
    }

    /// @return A Look's text: its mark and word, or its mark alone.
    [[nodiscard]] std::string lookText(Look const& look, int columns)
    {
        if (columns <= 1 || look.word.empty())
            return std::string { look.mark };
        return std::format("{} {}", look.mark, look.word);
    }

    /// @return The details line of the selected item: the failing checks, the conflict's base, the linked
    ///         ticket and its status, the agent on it.
    [[nodiscard]] std::string detailsOf(Engine const& engine, WorkItem const& item)
    {
        auto parts = std::vector<std::string> {};
        auto const label = workMrLabel(engine, item);
        parts.push_back(label.empty() ? item.ticketKey : label);
        parts.push_back(item.repo);
        if (auto const& mr = item.changeRequest)
        {
            if (mr->ci == CiState::Failing)
            {
                auto checks = std::string {};
                for (auto const& check: mr->failingChecks)
                    checks += checks.empty() ? check : std::format(", {}", check);
                parts.push_back(checks.empty() ? std::string { "pipeline fails" }
                                               : std::format("pipeline fails: {}", checks));
            }
            if (mr->conflicts == Conflicts::Conflicting)
                parts.push_back(std::format("conflicts with {}", mr->base));
            if (mr->review == ReviewState::ChangesRequested)
                parts.emplace_back("changes requested");
            // GitLab's single merge status stood in for a pipeline the board could not match.
            if (mr->ciSource == StateSource::Inferred)
                parts.emplace_back("pipeline and review inferred from the merge status");
            if (!item.ticketKey.empty())
                parts.push_back(item.ticket && !item.ticket->status.empty()
                                    ? std::format("{} {}", item.ticketKey, item.ticket->status)
                                    : item.ticketKey);
        }
        else if (item.ticket && !item.ticket->status.empty())
            parts.push_back(item.ticket->status);
        if (!item.agentRun.empty())
            parts.push_back(std::format("agent {} on {}", item.agentRun, item.agentWorker));
        auto text = std::string {};
        for (auto const& part: parts)
            if (!part.empty())
                text += text.empty() ? part : std::format("{}{}", HintSeparator, part);
        return text;
    }
} // namespace

WorkBoardView::WorkBoardView(Engine const& engine,
                             Interaction& interaction,
                             WorkBoardData& data,
                             HitMap& hits,
                             core::IClock const& clock):
    _engine { engine }, _interaction { interaction }, _data { data }, _hits { hits }, _clock { clock }
{
}

namespace
{
    /// What one frame of the board draws with.
    struct Frame
    {
        Canvas& inside;          ///< The board inside its frame.
        HitRecorder const& hits; ///< Where its chips' hits go.
        Engine const& engine;    ///< Its repositories, forges and wall clock.
        WorkState const& work;   ///< Where the user is.
    };

    /// Draws the filter bar on the first line: the filter on the left, the repository and sort chips on the
    /// right, the one that has the keys in the focus colour; each records its WorkChip hit.
    void drawFilterBar(Frame const& frame)
    {
        auto const& theme = frame.inside.theme();
        auto const& work = frame.work;
        auto const focused = [&](WorkFocus focus, Role role) {
            return styleOf(theme, work.focus == focus ? Role::Focus : role);
        };
        auto const chip = [&](int x, std::string_view text, WorkFocus focus) {
            frame.inside.putString(0, x, text, focused(focus, Role::Dim));
            frame.hits.add(Rect { .x = x, .y = 0, .width = columnsOf(text), .height = 1 },
                           Hit { .kind = HitKind::WorkChip, .value = std::to_underlying(focus) });
        };
        auto const chipRepo = chipRepository(work, frame.engine.repositories());
        auto const repo = chipRepo.empty() ? "all repositories"sv : chipRepo;
        auto const repoText = std::format("‹ {} ›", repo);
        auto const sortText = std::format(
            "sort: {}", sortOf(frame.engine) == WorkSort::LastUpdate ? "last update"sv : "importance"sv);
        auto x = frame.inside.width() - 1;
        if (frame.engine.hasAutomationRules())
        {
            auto const automationText =
                frame.engine.automation() == AutomationSwitch::On ? "auto ▶ on"sv : "auto ⏸ paused"sv;
            x -= columnsOf(automationText);
            chip(x, automationText, WorkFocus::AutomationChip);
            x -= 2;
        }
        x -= columnsOf(sortText);
        chip(x, sortText, WorkFocus::SortChip);
        x -= columnsOf(repoText) + 2;
        chip(x, repoText, WorkFocus::RepoChip);
        auto const typing = work.filtering == Filtering::Typing;
        auto const shown = typing || !work.filter.empty();
        auto const filterText =
            shown ? std::format("/ {}{}", work.filter, typing ? "▏" : "") : std::string { "/ filter" };
        auto const filterColumns = std::max(0, x - 3);
        frame.inside.putString(
            0, 1, cut(filterText, filterColumns), focused(WorkFocus::Filter, shown ? Role::Text : Role::Dim));
        frame.hits.add(Rect { .x = 0, .y = 0, .width = filterColumns + 1, .height = 1 },
                       Hit { .kind = HitKind::WorkChip, .value = std::to_underlying(WorkFocus::Filter) });
    }

    /// @return Why the list is empty.
    [[nodiscard]] std::string_view whyEmpty(Engine const& engine, WorkState const& work) noexcept
    {
        if (engine.config() == nullptr)
            return "Nothing to list until the orchestrator is configured."sv;
        if (!work.filter.empty() || work.repoChip != 0)
            return "Nothing matches the filter."sv;
        return "No merge requests or tickets."sv;
    }

    /// @return An item's title: its merge request's, else its ticket's.
    [[nodiscard]] std::string_view titleOf(WorkItem const& item) noexcept
    {
        if (item.changeRequest)
            return item.changeRequest->title;
        return item.ticket ? std::string_view { item.ticket->title } : std::string_view {};
    }

    /// Draws an item's row on @p y, its cells as @p columns lays them out, each in the role @p pick styles.
    template <typename Pick>
    void drawItem(Canvas& list,
                  int y,
                  Engine const& engine,
                  WorkItem const& item,
                  Columns const& columns,
                  uint64_t nowWallMs,
                  Pick const& pick)
    {
        auto x = 1;
        x = putCell(list, y, x, columns.mr, workMrLabel(engine, item), pick(Role::Text), columns.gap);
        auto const hasKey = !item.ticketKey.empty();
        x = putCell(list,
                    y,
                    x,
                    columns.ticket,
                    hasKey ? std::string_view { item.ticketKey } : "·"sv,
                    pick(hasKey ? Role::Text : Role::Dim),
                    columns.gap);
        x = putCell(list, y, x, columns.title, titleOf(item), pick(Role::Text), columns.gap);
        auto const pipeline = pipelineLook(item);
        x = putCell(list,
                    y,
                    x,
                    columns.pipeline,
                    lookText(pipeline, columns.pipeline),
                    pick(pipeline.role),
                    columns.gap);
        auto const review = reviewLook(item);
        x = putCell(
            list, y, x, columns.review, lookText(review, columns.review), pick(review.role), columns.gap);
        auto const jira = jiraCell(item);
        x = putCell(list, y, x, columns.jira, jira.text, pick(jira.role), columns.gap);
        auto const agent = agentCell(engine, item, nowWallMs, columns.detail);
        static_cast<void>(putCell(list, y, x, columns.agent, agent.text, pick(agent.role), columns.gap));
    }

    /// Records where an item's MR number and ticket key are drawn on @p y, as drawItem() lays them out: a
    /// double click there opens them.
    void recordLinks(
        HitRecorder const& hits, int y, std::size_t line, WorkItem const& item, Columns const& columns)
    {
        auto x = 1;
        if (columns.mr > 0)
        {
            if (item.changeRequest)
                hits.add(Rect { .x = x, .y = y, .width = columns.mr, .height = 1 },
                         Hit { .kind = HitKind::WorkMrLink, .value = line });
            x += columns.mr + columns.gap;
        }
        if (columns.ticket > 0 && !item.ticketKey.empty())
            hits.add(Rect { .x = x, .y = y, .width = columns.ticket, .height = 1 },
                     Hit { .kind = HitKind::WorkTicketLink, .value = line });
    }

    /// Draws the scrollbar on the list's right edge, @p rows high, while the list holds more than it shows.
    void drawScrollbar(Canvas& inside, WorkState const& work, std::size_t lines, std::size_t rows)
    {
        if (lines <= rows || rows == 0)
            return;
        auto const thumb = std::max<std::size_t>(1, rows * rows / lines);
        auto const top = work.scroll * (rows - thumb) / (lines - rows);
        for (auto const at: std::views::iota(std::size_t { 0 }, rows))
        {
            auto const inThumb = at >= top && at < top + thumb;
            inside.putString(static_cast<int>(at) + 1,
                             inside.width() - 1,
                             inThumb ? "┃" : "│",
                             styleOf(inside.theme(), inThumb ? Role::Text : Role::Border));
        }
    }

    /// @return The details line: what the selected line is about.
    [[nodiscard]] std::string detailsLine(Engine const& engine,
                                          WorkState const& work,
                                          std::span<WorkLine const> lines,
                                          std::span<WorkItem const> items)
    {
        auto const* const line = selectedLine(work, lines);
        if (line == nullptr)
            return {};
        if (line->kind == WorkLineKind::Item)
            return detailsOf(engine, items[line->item]);
        return std::format("{} · {} folds it",
                           WorkGroupRows[std::to_underlying(line->group)].name,
                           workKeyNameOf(KeyCommand::Fold, keysInForce(engine)));
    }
} // namespace

void WorkBoardView::render(Canvas& canvas)
{
    auto const& theme = canvas.theme();
    auto& work = _interaction.work;
    auto const& lines = _data.lines;
    auto const& items = _data.items;
    // The title and each heading count the items the filter and the chip let through, folded or not.
    auto counts = std::array<std::size_t, WorkGroupRows.size()> {};
    auto const filter = ItemFilter { _engine, work };
    for (auto const& item: items)
        if (filter.shows(item))
            ++counts[std::to_underlying(item.group)];
    drawFrame(canvas,
              std::format("Work {}", std::ranges::fold_left(counts, std::size_t { 0 }, std::plus {})),
              Role::Focus);
    auto inside = insideFrame(canvas);
    auto const hits = HitRecorder { _hits, { .x = screenBounds().x + 1, .y = screenBounds().y + 1 } };
    drawFilterBar(Frame { .inside = inside, .hits = hits, .engine = _engine, .work = work });

    // The list, between the filter bar and the details line; its right edge is the scrollbar's.
    auto const rows = std::max(0, inside.height() - 2);
    _data.listRows = static_cast<std::size_t>(rows);
    keepWorkSelection(work, lines, items, _data.listRows);
    auto list =
        inside.subcanvas(Rect { .x = 0, .y = 1, .width = std::max(0, inside.width() - 1), .height = rows });
    auto const listHits = hits.shifted(0, 1);
    // The list's right edge is the scrollbar's, whether it is drawn or not: a click there jumps.
    listHits.add(Rect { .x = list.width(), .y = 0, .width = 1, .height = rows },
                 Hit { .kind = HitKind::WorkScrollbar });
    if (lines.empty())
        list.putString(0, 1, cut(whyEmpty(_engine, work), list.width() - 2), styleOf(theme, Role::Dim));
    auto const columns = columnsFor(_engine, items, list.width() - 2, canvas.width());
    // The engine's wall clock moves only when it wakes: the ages move on from it with the clock, and the
    // frame says when the next one it draws changes.
    auto const nowMonoMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now().time_since_epoch()).count());
    if (!_data.wall || _data.wall->wallMs != _engine.wallMs())
        _data.wall = WallSample { .wallMs = _engine.wallMs(), .monoMs = nowMonoMs };
    auto const nowWallMs = _data.wall->wallMs + (nowMonoMs - std::min(nowMonoMs, _data.wall->monoMs));
    _data.ageChangeMs.reset();
    for (auto const at: std::views::iota(work.scroll, std::min(lines.size(), work.scroll + _data.listRows)))
    {
        auto const y = static_cast<int>(at - work.scroll);
        auto const& line = lines[at];
        auto const isSelected = at == work.selected;
        auto const pick = [&](Role role) {
            auto style = styleOf(theme, role);
            if (isSelected)
                style.bg = styleOf(theme, Role::Selection).bg;
            return style;
        };
        if (isSelected)
        {
            list.fill(Rect { .x = 0, .y = y, .width = list.width(), .height = 1 }, ' ', pick(Role::Text));
            list.putString(y, 0, "▍", pick(Role::Accent));
        }
        else if (work.hovered == at)
            list.putString(y, 0, "▏", pick(Role::Dim));
        auto const row = Rect { .x = 0, .y = y, .width = list.width(), .height = 1 };
        if (line.kind == WorkLineKind::Item)
        {
            auto const& item = items[line.item];
            listHits.add(row, Hit { .kind = HitKind::WorkRow, .value = at });
            recordLinks(listHits, y, at, item, columns);
            drawItem(list, y, _engine, item, columns, nowWallMs, pick);
            if (auto const age = shownAgeOf(item, nowWallMs); age && columns.detail == Detail::Full)
                _data.ageChangeMs = std::min(_data.ageChangeMs.value_or(std::numeric_limits<uint64_t>::max()),
                                             nowMonoMs + untilAgeChanges(*age));
            continue;
        }
        listHits.add(row, Hit { .kind = HitKind::WorkHeading, .value = std::to_underlying(line.group) });
        auto style = pick(GroupRoles[std::to_underlying(line.group)]);
        style.bold = true;
        // Sanitised already: the table's name and a count, two spaces apart.
        list.putString(y,
                       1,
                       cutSanitised(std::format("{} {}  {}",
                                                isGroupFolded(_engine, line.group) ? "▸" : "▾",
                                                WorkGroupRows[std::to_underlying(line.group)].name,
                                                counts[std::to_underlying(line.group)]),
                                    list.width() - 2),
                       style);
    }
    drawScrollbar(inside, work, lines.size(), _data.listRows);
    inside.putString(inside.height() - 1,
                     1,
                     cut(detailsLine(_engine, work, lines, items), inside.width() - 2),
                     styleOf(theme, Role::Dim));
}

// }}}

} // namespace orchestrator::view
