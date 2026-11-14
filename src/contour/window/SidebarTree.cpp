// SPDX-License-Identifier: Apache-2.0
#include <contour/window/AttentionJump.hpp>
#include <contour/window/SidebarTree.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <string_view>
#include <utility>

namespace contour::window
{

namespace
{
    /// What a pane's own status says of it, before it is rolled up.
    struct LeafStatus
    {
        Urgency urgency = Urgency::Idle;
        TabBadgeKind glyph = TabBadgeKind::None;
        ReasonParts reason;
        ReasonParts fullReason;
        std::string program;
        std::string folder;
        TimeText time;
    };

    /// The label a blocked record's reason starts with, by what it waits for.
    struct BlockedLabel
    {
        vtbackend::BlockedKind kind;
        std::string_view label;
    };

    constexpr auto BlockedLabels = std::array {
        BlockedLabel { .kind = vtbackend::BlockedKind::None, .label = {} },
        BlockedLabel { .kind = vtbackend::BlockedKind::Permission, .label = "Permission" },
        BlockedLabel { .kind = vtbackend::BlockedKind::Question, .label = "Question" },
        BlockedLabel { .kind = vtbackend::BlockedKind::Auth, .label = "Sign-in" },
    };

    /// The badge look a status badge kind is drawn as.
    constexpr TabBadgeKind glyphOf(BadgeKind kind) noexcept
    {
        switch (kind)
        {
            case BadgeKind::None: return TabBadgeKind::None;
            case BadgeKind::Working: return TabBadgeKind::Working;
            case BadgeKind::Blocked: return TabBadgeKind::Blocked;
            case BadgeKind::Done: return TabBadgeKind::Done;
            case BadgeKind::Error: return TabBadgeKind::Error;
        }
        return TabBadgeKind::None;
    }

    /// The glyph of a pane none of whose records won.
    constexpr TabBadgeKind glyphOf(Urgency urgency) noexcept
    {
        switch (urgency)
        {
            case Urgency::Idle: return TabBadgeKind::None;
            case Urgency::UnseenOutput: return TabBadgeKind::Output;
            case Urgency::Working: return TabBadgeKind::Working;
            case Urgency::DoneUnseen: return TabBadgeKind::Done;
            case Urgency::Failed: return TabBadgeKind::Error;
            case Urgency::NeedsYou: return TabBadgeKind::Bell;
        }
        return TabBadgeKind::None;
    }

    /// Text from outside, made fit for a row: no controls, no invisible or bidi formatting, at most
    /// @p maxLength characters.
    std::string sanitizedTo(std::string_view text, std::size_t maxLength)
    {
        return elided(disarmInvisibleFormatting(text), maxLength);
    }

    /// The record that decides a pane's badge, as badgeFor() picks it.
    struct Winner
    {
        vtbackend::ProgramStatusRecord const* record = nullptr;
        BadgeKind kind = BadgeKind::None;
    };

    Winner winnerOf(std::span<vtbackend::ProgramStatusRecord const> records, uint64_t seenGeneration)
    {
        auto const newestFirst = records | std::views::reverse;
        for (auto const& row: BadgeRows)
        {
            auto const found =
                std::ranges::find(newestFirst, row.state, &vtbackend::ProgramStatusRecord::state);
            if (found == newestFirst.end())
                continue;
            if (row.visibility == BadgeVisibility::WhileUnseen
                && statusSeenBy(*found, seenGeneration) == StatusSeen::Seen)
                continue;
            return { .record = &*found, .kind = row.kind };
        }
        return {};
    }

    /// The length of the tool name that leads a permission's message (`Bash: ` in `Bash: psql …`), with its
    /// colon and space; zero when the message does not start with one word and `: `.
    std::size_t toolPrefixLength(std::string_view message) noexcept
    {
        constexpr auto Separator = std::string_view { ": " };
        auto const end = message.find(Separator);
        if (end == 0 || end == std::string_view::npos)
            return 0;
        auto const tool = message.substr(0, end);
        if (std::ranges::any_of(tool, [](char ch) { return ch == ' ' || ch == '\t'; }))
            return 0;
        return end + Separator.size();
    }

    LeafStatus leafOf(DirectoryPane const& pane,
                      SessionAttention const& attention,
                      std::chrono::system_clock::time_point now)
    {
        auto leaf = LeafStatus {};
        leaf.urgency = paneUrgency(pane, attention);
        leaf.folder = rowTextOf(pane.cwd);
        auto const winner = winnerOf(pane.status, attention.seenStatusGeneration);
        if (winner.record && urgencyOf(winner.kind, StatusSeen::Unseen) == leaf.urgency)
        {
            leaf.glyph = glyphOf(winner.kind);
            leaf.reason = statusReasonOf(*winner.record, winner.kind, MaxRowTextLength);
            leaf.fullReason = statusReasonOf(*winner.record, winner.kind, MaxFullReasonLength);
            leaf.program = rowTextOf(winner.record->app);
            leaf.time = formatTimeText(statusTimeKind(winner.record->progress, winner.kind),
                                       winner.record->since == std::chrono::system_clock::time_point {}
                                           ? std::nullopt
                                           : std::optional { winner.record->since },
                                       winner.record->progress,
                                       now);
            return leaf;
        }
        leaf.glyph = glyphOf(leaf.urgency);
        auto const verdict = [&]() -> std::optional<FinishedBlockBadge> {
            switch (leaf.urgency)
            {
                case Urgency::Failed: return FinishedBlockBadge::Failed;
                case Urgency::DoneUnseen: return FinishedBlockBadge::Succeeded;
                case Urgency::Idle:
                case Urgency::UnseenOutput:
                case Urgency::Working:
                case Urgency::NeedsYou: return std::nullopt;
            }
            return std::nullopt;
        }();
        if (verdict && pane.finished && finishedBadgeOf(*pane.finished) == *verdict)
            leaf.reason = leaf.fullReason = finishedText(*pane.finished);
        return leaf;
    }

    /// The most urgent of @p leaves, the first among equals; idle for none.
    LeafStatus const& mostUrgent(std::span<LeafStatus const> leaves)
    {
        static auto const idle = LeafStatus {};
        if (leaves.empty())
            return idle;
        return *std::ranges::max_element(leaves, {}, &LeafStatus::urgency);
    }

    LeafCounts countsOf(std::span<LeafStatus const> leaves)
    {
        auto const count = [&](Urgency urgency) {
            return static_cast<uint32_t>(std::ranges::count(leaves, urgency, &LeafStatus::urgency));
        };
        return { .needYou = count(Urgency::NeedsYou), .working = count(Urgency::Working) };
    }

    void adopt(SidebarRow& row, LeafStatus const& leaf)
    {
        row.urgency = leaf.urgency;
        row.glyph = leaf.glyph;
        row.reason = leaf.reason.joined();
        row.reasonLabel = leaf.reason.label;
        row.reasonSeparator = leaf.reason.separator;
        row.reasonCommand = leaf.reason.command;
        row.fullReason = leaf.fullReason.joined();
        // The hover card never lets its command go, so the separator stays with the words there.
        row.fullReasonLabel = leaf.fullReason.label + leaf.fullReason.separator;
        row.fullReasonCommand = leaf.fullReason.command;
        row.program = leaf.program;
        row.folder = leaf.folder;
        row.time = leaf.time;
    }

    char asciiLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    std::string asciiLowered(std::string_view text)
    {
        return std::ranges::to<std::string>(text | std::views::transform(asciiLower));
    }

    /// Whether @p row holds @p lowerFilter (already lowered) in its title, reason or git text.
    bool matches(SidebarRow const& row, std::string_view lowerFilter)
    {
        return std::ranges::any_of(
            std::array { &row.title, &row.reason, &row.gitText },
            [&](std::string const* field) { return asciiLowered(*field).contains(lowerFilter); });
    }

    /// A tab with the rows it may show.
    struct TabBlock
    {
        SidebarRow row;
        std::vector<SidebarRow> panes; ///< Those that are shown.
    };

    struct Building
    {
        TabAttention const& attention;
        SidebarState const& state;
        std::chrono::system_clock::time_point now;
        std::string lowerFilter;
    };

    TabBlock tabBlockOf(Building const& context,
                        DirectoryWorkspace const& workspace,
                        DirectoryTab const& tab,
                        std::size_t position,
                        RowSelection selection,
                        std::vector<LeafStatus>& leavesOut)
    {
        auto leaves = std::vector<LeafStatus> {};
        leaves.reserve(tab.panes.size());
        for (auto const& pane: tab.panes)
            leaves.push_back(leafOf(pane, context.attention.of(pane.key), context.now));

        auto block = TabBlock {};
        auto& row = block.row;
        row.key = { .kind = SidebarRowKind::Tab, .workspace = workspace.id, .tab = tab.id, .pane = {} };
        row.depth = 1;
        row.position = position;
        auto const& fallbackTitle = tab.panes.empty() ? std::string {} : tab.panes.front().title;
        row.title = rowTextOf(!tab.label.empty() ? tab.label : fallbackTitle);
        if (row.title.empty())
            row.title = "Terminal";
        row.selection = selection;
        adopt(row, mostUrgent(leaves));
        row.leaves = countsOf(leaves);

        auto const filtering = !context.lowerFilter.empty();
        auto const split = tab.panes.size() > 1;
        auto const folded =
            context.state.collapsedTabs.contains(TabRef { .workspace = workspace.id, .tab = tab.id });
        if (split && (filtering || !folded))
        {
            for (auto const& [panePosition, pane, leaf]:
                 std::views::zip(std::views::iota(std::size_t { 0 }), tab.panes, leaves))
            {
                auto paneRow = SidebarRow {};
                paneRow.key = {
                    .kind = SidebarRowKind::Pane, .workspace = workspace.id, .tab = tab.id, .pane = pane.key
                };
                paneRow.depth = 2;
                paneRow.position = panePosition;
                auto const& named = !pane.title.empty() ? pane.title : pane.cwd;
                paneRow.title = rowTextOf(named);
                if (paneRow.title.empty())
                    paneRow.title = std::format("Pane {}", panePosition + 1);
                adopt(paneRow, leaf);
                paneRow.leaves = countsOf(std::span { &leaf, 1 });
                if (!filtering || matches(paneRow, context.lowerFilter))
                    block.panes.push_back(std::move(paneRow));
            }
        }
        if (!split)
            row.expansion = RowExpansion::None;
        else if (filtering)
            row.expansion = block.panes.empty() ? RowExpansion::Collapsed : RowExpansion::Expanded;
        else
            row.expansion = folded ? RowExpansion::Collapsed : RowExpansion::Expanded;
        leavesOut.insert(leavesOut.end(), leaves.begin(), leaves.end());
        return block;
    }
    /// Appends the rows of @p tabs, each tab followed by its panes, with the Terminal look's connectors on
    /// both of every row's lines.
    void appendTabRows(std::vector<SidebarRow>& rows, std::span<TabBlock const> tabs)
    {
        for (auto const& [index, block]: core::views::enumerate(tabs))
        {
            auto const lastTab = static_cast<std::size_t>(index) + 1 == tabs.size();
            // The line past a tab goes on to its next sibling, and from a tab with panes shown down to them.
            auto const pastTab = lastTab ? TreeGlyph::None : TreeGlyph::Bar;
            auto tabRow = block.row;
            tabRow.lines.columns[0] = lastTab ? TreeGlyph::Last : TreeGlyph::Tee;
            tabRow.continued.columns = { pastTab, block.panes.empty() ? TreeGlyph::None : TreeGlyph::Bar };
            rows.push_back(std::move(tabRow));
            for (auto const& [paneIndex, pane]: core::views::enumerate(block.panes))
            {
                auto const lastPane = static_cast<std::size_t>(paneIndex) + 1 == block.panes.size();
                auto paneRow = pane;
                paneRow.lines.columns = { pastTab, lastPane ? TreeGlyph::Last : TreeGlyph::Tee };
                paneRow.continued.columns = { pastTab, lastPane ? TreeGlyph::None : TreeGlyph::Bar };
                rows.push_back(std::move(paneRow));
            }
        }
    }

    /// Whether a workspace row carries the selection for the displayed tab its fold or filter hides.
    /// @param workspace The workspace.
    /// @param tabs Its tabs as shown.
    /// @param displayedTab The tab the window displays.
    /// @return ContainsSelected when the workspace holds that tab and none of @p tabs is it; else None.
    [[nodiscard]] RowSelection workspaceSelectionOf(DirectoryWorkspace const& workspace,
                                                    std::span<TabBlock const> tabs,
                                                    std::optional<TabRef> displayedTab)
    {
        auto const holdsDisplayed =
            displayedTab && displayedTab->workspace == workspace.id
            && std::ranges::contains(workspace.tabs, displayedTab->tab, &DirectoryTab::id);
        auto const showsDisplayed = std::ranges::any_of(
            tabs, [](TabBlock const& block) { return block.row.selection == RowSelection::Selected; });
        return holdsDisplayed && !showsDisplayed ? RowSelection::ContainsSelected : RowSelection::None;
    }

} // namespace

std::string gitTextOf(DirectoryGit const& git)
{
    auto text = rowTextOf(git.branch);
    if (git.ahead && *git.ahead != 0)
        text += std::format(" ↑{}", *git.ahead);
    if (git.behind && *git.behind != 0)
        text += std::format(" ↓{}", *git.behind);
    return text;
}

SidebarListing sidebarListing(WorkspaceDirectory const& directory,
                              TabAttention const& attention,
                              SidebarState const& state,
                              std::chrono::system_clock::time_point now,
                              std::optional<TabRef> displayedTab)
{
    auto const context = Building {
        .attention = attention, .state = state, .now = now, .lowerFilter = asciiLowered(state.filter)
    };
    auto const filtering = !context.lowerFilter.empty();
    auto listing = SidebarListing {};
    auto& rows = listing.rows;

    for (auto const& [position, workspace]: core::views::enumerate(directory.workspaces))
    {
        auto leaves = std::vector<LeafStatus> {};
        auto tabs = std::vector<TabBlock> {};
        auto const folded = state.collapsedWorkspaces.contains(workspace.id);
        for (auto const& [tabPosition, tab]: core::views::enumerate(workspace.tabs))
        {
            auto const selected = displayedTab == TabRef { .workspace = workspace.id, .tab = tab.id };
            auto block = tabBlockOf(context,
                                    workspace,
                                    tab,
                                    static_cast<std::size_t>(tabPosition),
                                    selected ? RowSelection::Selected : RowSelection::None,
                                    leaves);
            if (!filtering || matches(block.row, context.lowerFilter) || !block.panes.empty())
                tabs.push_back(std::move(block));
        }
        if (!filtering && folded)
            tabs.clear();

        auto row = SidebarRow {};
        row.key = { .kind = SidebarRowKind::Workspace, .workspace = workspace.id, .tab = 0, .pane = {} };
        row.position = static_cast<std::size_t>(position);
        row.title = rowTextOf(workspaceTitleOf(workspace, static_cast<std::size_t>(position)));
        if (workspace.git)
            row.gitText = gitTextOf(*workspace.git);
        auto const best = mostUrgent(leaves);
        row.urgency = best.urgency;
        row.glyph = best.glyph;
        row.leaves = countsOf(leaves);
        if (workspace.tabs.empty())
            row.expansion = RowExpansion::None;
        else if (filtering)
            row.expansion = tabs.empty() ? RowExpansion::Collapsed : RowExpansion::Expanded;
        else
            row.expansion = folded ? RowExpansion::Collapsed : RowExpansion::Expanded;

        // A fold that hides the displayed tab hands its selection to the workspace, the nearest row shown.
        row.selection = workspaceSelectionOf(workspace, tabs, displayedTab);
        listing.workspaces.push_back(row);
        if (filtering && !matches(row, context.lowerFilter) && tabs.empty())
            continue;
        rows.push_back(std::move(row));
        appendTabRows(rows, tabs);
    }
    return listing;
}

std::vector<SidebarRow> sidebarRows(WorkspaceDirectory const& directory,
                                    TabAttention const& attention,
                                    SidebarState const& state,
                                    std::chrono::system_clock::time_point now,
                                    std::optional<TabRef> displayedTab)
{
    return sidebarListing(directory, attention, state, now, displayedTab).rows;
}

SidebarFooter sidebarFooter(std::span<SidebarRow const> rows)
{
    auto footer = SidebarFooter {};
    for (auto const& row: rows | std::views::filter([](SidebarRow const& r) {
                              return r.key.kind == SidebarRowKind::Workspace;
                          }))
    {
        footer.needYou += row.leaves.needYou;
        footer.working += row.leaves.working;
    }
    return footer;
}

SidebarSummary sidebarSummary(std::span<SidebarRow const> rows, std::optional<uint64_t> current)
{
    auto summary = SidebarSummary {};
    for (auto const& row: rows | std::views::filter([](SidebarRow const& r) {
                              return r.key.kind == SidebarRowKind::Workspace;
                          }))
    {
        if (current == row.key.workspace)
            summary.currentWorkspace = row.title;
        else if (row.urgency > summary.otherWorkspaces)
        {
            summary.otherWorkspaces = row.urgency;
            summary.otherWorkspacesGlyph = row.glyph;
        }
    }
    return summary;
}

std::string_view urgencyText(Urgency urgency) noexcept
{
    switch (urgency)
    {
        case Urgency::Idle: return {};
        case Urgency::UnseenOutput: return "new output";
        case Urgency::Working: return "working";
        case Urgency::DoneUnseen: return "done";
        case Urgency::Failed: return "failed";
        case Urgency::NeedsYou: return "needs you";
    }
    return {};
}

std::string hoverDetailOf(SidebarRow const& row)
{
    auto detail = std::string {};
    for (auto const* part: { &row.program, &row.folder, &row.time.text })
    {
        if (part->empty())
            continue;
        if (!detail.empty())
            detail += LabelSeparator;
        detail += *part;
    }
    return detail;
}

int parentRowOf(std::span<SidebarRow const> rows, int row) noexcept
{
    if (row <= 0 || std::cmp_greater_equal(row, rows.size()))
        return -1;
    auto const depth = rows[static_cast<std::size_t>(row)].depth;
    for (auto const above: std::views::iota(0, row) | std::views::reverse)
        if (rows[static_cast<std::size_t>(above)].depth < depth)
            return above;
    return -1;
}

std::string treeLinesText(TreeLines const& lines, uint8_t depth)
{
    auto text = std::string {};
    for (auto const glyph: lines.columns | std::views::take(depth))
    {
        switch (glyph)
        {
            case TreeGlyph::None: text += ' '; break;
            case TreeGlyph::Tee: text += "├"; break;
            case TreeGlyph::Last: text += "└"; break;
            case TreeGlyph::Bar: text += "│"; break;
        }
        text += ' ';
    }
    return text;
}

std::string treeContinuationText(SidebarRow const& row)
{
    return row.depth == 0 ? std::string {}
                          : treeLinesText(row.continued, static_cast<uint8_t>(row.continued.columns.size()));
}

std::string meterText(int percent)
{
    // Eighths of a cell, empty to full: the partial blocks Unicode has, left-aligned.
    static constexpr auto Eighths =
        std::array<std::string_view, 9> { "░", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };
    auto const eighths = std::clamp(percent, 0, 100) * MeterCells * 8 / 100;
    auto text = std::string {};
    for (auto const cell: std::views::iota(0, MeterCells))
        text += Eighths[static_cast<std::size_t>(std::clamp(eighths - (cell * 8), 0, 8))];
    return text;
}

std::string formatLive(std::chrono::seconds elapsed)
{
    auto const total = std::max(elapsed, std::chrono::seconds::zero()).count();
    auto const hours = total / 3600;
    auto const minutes = total / 60 % 60;
    auto const seconds = total % 60;
    return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, seconds)
                     : std::format("{}:{:02}", minutes, seconds);
}

std::string formatRelative(std::chrono::seconds elapsed)
{
    namespace chrono = std::chrono;
    auto const clamped = std::max(elapsed, chrono::seconds::zero());
    if (clamped < chrono::seconds { 10 })
        return "now";
    if (clamped < chrono::minutes { 1 })
        return std::format("{}s ago", clamped.count());
    if (clamped < chrono::hours { 1 })
        return std::format("{}m ago", chrono::duration_cast<chrono::minutes>(clamped).count());
    if (clamped < chrono::hours { 24 })
        return std::format("{}h ago", chrono::duration_cast<chrono::hours>(clamped).count());
    return std::format("{}d ago", chrono::duration_cast<chrono::hours>(clamped).count() / 24);
}

std::string rowTextOf(std::string_view text)
{
    return sanitizedTo(text, MaxRowTextLength);
}

ReasonParts statusReasonOf(vtbackend::ProgramStatusRecord const& record,
                           BadgeKind kind,
                           std::size_t maxLength)
{
    auto text = sanitizedTo(record.message.empty() ? record.title : record.message, maxLength);
    if (kind != BadgeKind::Blocked)
        return { .label = std::move(text), .separator = {}, .command = {} };
    auto const label = std::ranges::find(BlockedLabels, record.kind, &BlockedLabel::kind)->label;
    if (label.empty())
        return { .label = std::move(text), .separator = {}, .command = {} };
    auto const lead = text.empty() ? std::string { label } : std::format("{}{}", label, LabelSeparator);
    if (record.kind == vtbackend::BlockedKind::Permission && !record.message.empty() && !text.empty())
    {
        auto const prefix = toolPrefixLength(text);
        auto const tool =
            prefix < text.size() ? prefix : std::size_t { 0 }; // A tool with no command stays one.
        auto const withTool = lead + text.substr(0, tool);
        auto const room = withTool.size() < maxLength ? maxLength - withTool.size() : std::size_t { 1 };
        return { .label = withTool,
                 .separator = {},
                 .command = elided(std::string_view { text }.substr(tool), room) };
    }
    return { .label = elided(text.empty() ? lead : lead + text, maxLength), .separator = {}, .command = {} };
}

TimeKind statusTimeKind(std::optional<uint8_t> progress, BadgeKind kind) noexcept
{
    auto const live = kind == BadgeKind::Working || kind == BadgeKind::Blocked;
    if (progress && live)
        return TimeKind::Progress;
    return live ? TimeKind::Live : TimeKind::Relative;
}

TimeText formatTimeText(TimeKind kind,
                        std::optional<std::chrono::system_clock::time_point> since,
                        std::optional<uint8_t> progress,
                        std::chrono::system_clock::time_point now)
{
    if (kind == TimeKind::Progress && progress)
        return { .kind = TimeKind::Progress, .text = std::format("{}%", *progress), .percent = progress };
    if (kind == TimeKind::None || kind == TimeKind::Progress || !since)
        return {};
    auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *since);
    return { .kind = kind,
             .text = kind == TimeKind::Live ? formatLive(elapsed) : formatRelative(elapsed),
             .percent = std::nullopt };
}

ReasonParts finishedText(vthost::proto::FinishedCommand const& finished)
{
    auto text = finished.exitCode ? std::format("exit {}", *finished.exitCode) : std::string { "finished" };
    if (finished.durationMs)
        text += std::format("{}{}",
                            LabelSeparator,
                            vtbackend::formatCommandDuration(
                                std::chrono::milliseconds { static_cast<int64_t>(*finished.durationMs) }));
    if (finished.commandLine.empty())
        return { .label = std::move(text), .separator = {}, .command = {} };
    return { .label = std::move(text),
             .separator = std::string { LabelSeparator },
             .command = rowTextOf(finished.commandLine) };
}

} // namespace contour::window
