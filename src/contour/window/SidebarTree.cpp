// SPDX-License-Identifier: Apache-2.0
#include <contour/window/AttentionJump.hpp>
#include <contour/window/SidebarTree.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
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
        std::string reason;
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

    constexpr std::string_view LabelSeparator = " · ";

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

    /// Text from outside, made fit for a row: no controls, no invisible or bidi formatting, bounded.
    std::string sanitized(std::string_view text)
    {
        return elided(disarmInvisibleFormatting(text), MaxRowTextLength);
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

    std::string reasonOf(vtbackend::ProgramStatusRecord const& record, BadgeKind kind)
    {
        auto text = sanitized(record.message.empty() ? record.title : record.message);
        if (kind != BadgeKind::Blocked)
            return text;
        auto const label = std::ranges::find(BlockedLabels, record.kind, &BlockedLabel::kind)->label;
        if (label.empty())
            return text;
        return elided(text.empty() ? std::string { label }
                                   : std::format("{}{}{}", label, LabelSeparator, text),
                      MaxRowTextLength);
    }

    TimeText timeOf(vtbackend::ProgramStatusRecord const& record,
                    BadgeKind kind,
                    std::chrono::system_clock::time_point now)
    {
        if (record.progress && (kind == BadgeKind::Working || kind == BadgeKind::Blocked))
            return { .kind = TimeKind::Progress,
                     .text = std::format("{}%", *record.progress),
                     .percent = record.progress };
        if (record.since == std::chrono::system_clock::time_point {})
            return {};
        auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - record.since);
        auto const live = kind == BadgeKind::Working || kind == BadgeKind::Blocked;
        return { .kind = live ? TimeKind::Live : TimeKind::Relative,
                 .text = live ? formatLive(elapsed) : formatRelative(elapsed),
                 .percent = std::nullopt };
    }

    /// `exit 1 · 18s`.
    std::string finishedText(vthost::proto::FinishedCommand const& finished)
    {
        auto text =
            finished.exitCode ? std::format("exit {}", *finished.exitCode) : std::string { "finished" };
        if (finished.durationMs)
            text += std::format("{}{}",
                                LabelSeparator,
                                vtbackend::formatCommandDuration(std::chrono::milliseconds {
                                    static_cast<int64_t>(*finished.durationMs) }));
        return text;
    }

    LeafStatus leafOf(DirectoryPane const& pane,
                      SessionAttention const& attention,
                      std::chrono::system_clock::time_point now)
    {
        auto leaf = LeafStatus {};
        leaf.urgency = paneUrgency(pane, attention);
        auto const winner = winnerOf(pane.status, attention.seenStatusGeneration);
        if (winner.record && urgencyOf(winner.kind, StatusSeen::Unseen) == leaf.urgency)
        {
            leaf.glyph = glyphOf(winner.kind);
            leaf.reason = reasonOf(*winner.record, winner.kind);
            leaf.time = timeOf(*winner.record, winner.kind, now);
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
            leaf.reason = finishedText(*pane.finished);
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
        row.reason = leaf.reason;
        row.time = leaf.time;
    }

    std::string gitTextOf(DirectoryGit const& git)
    {
        auto text = sanitized(git.branch);
        if (git.ahead && *git.ahead != 0)
            text += std::format(" ↑{}", *git.ahead);
        if (git.behind && *git.behind != 0)
            text += std::format(" ↓{}", *git.behind);
        return text;
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
        row.title = sanitized(!tab.label.empty() ? tab.label : fallbackTitle);
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
                paneRow.title = sanitized(named);
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
} // namespace

std::vector<SidebarRow> sidebarRows(WorkspaceDirectory const& directory,
                                    TabAttention const& attention,
                                    SidebarState const& state,
                                    std::chrono::system_clock::time_point now,
                                    std::optional<TabRef> displayedTab)
{
    auto const context = Building {
        .attention = attention, .state = state, .now = now, .lowerFilter = asciiLowered(state.filter)
    };
    auto const filtering = !context.lowerFilter.empty();
    auto rows = std::vector<SidebarRow> {};

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
        row.title =
            workspace.label.empty() ? std::format("Workspace {}", position + 1) : sanitized(workspace.label);
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

        if (filtering && !matches(row, context.lowerFilter) && tabs.empty())
            continue;
        rows.push_back(std::move(row));
        for (auto const& [index, block]: std::views::enumerate(tabs))
        {
            auto const lastTab = static_cast<std::size_t>(index) + 1 == tabs.size();
            auto tabRow = block.row;
            tabRow.lines.columns[0] = lastTab ? TreeGlyph::Last : TreeGlyph::Tee;
            rows.push_back(std::move(tabRow));
            for (auto const& [paneIndex, pane]: std::views::enumerate(block.panes))
            {
                auto paneRow = pane;
                paneRow.lines.columns[0] = lastTab ? TreeGlyph::None : TreeGlyph::Bar;
                paneRow.lines.columns[1] = static_cast<std::size_t>(paneIndex) + 1 == block.panes.size()
                                               ? TreeGlyph::Last
                                               : TreeGlyph::Tee;
                rows.push_back(std::move(paneRow));
            }
        }
    }
    return rows;
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

} // namespace contour::window
