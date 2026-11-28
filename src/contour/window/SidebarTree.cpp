// SPDX-License-Identifier: Apache-2.0
#include <contour/window/AttentionJump.hpp>
#include <contour/window/SidebarTree.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
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
        std::string fullFolder;
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
                      SessionAttention const& windowMarks,
                      std::chrono::system_clock::time_point now)
    {
        auto leaf = LeafStatus {};
        leaf.urgency = paneUrgency(pane, windowMarks);
        leaf.folder = rowTextOf(pane.cwd);
        leaf.fullFolder = rowTextOf(pane.cwd, MaxFullReasonLength);
        auto const winner = winnerOf(pane.status, marksOf(pane, windowMarks).seenStatusGeneration);
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
        {
            leaf.reason = leaf.fullReason = finishedText(*pane.finished);
            // Where the daemon stated when it ended, the time since.
            leaf.time = formatTimeText(TimeKind::Relative, pane.finishedAt, std::nullopt, now);
        }
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
        row.fullFolder = leaf.fullFolder;
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

    /// Whether a section opens with its Daemon row.
    enum class SectionHeading : uint8_t
    {
        Omitted = 0, ///< A window with no daemon: its one section has no heading.
        Shown,       ///< A Daemon row heads the section.
    };

    /// What one pass over every section shares.
    struct Building
    {
        SidebarState const& state;
        std::chrono::system_clock::time_point now;
        std::string lowerFilter;
        std::optional<SidebarTarget> displayed;
        PendingJumps pending;

        /// @return Whether a filter or the Needs-me scope narrows the rows, expanding every ancestor it
        /// keeps.
        [[nodiscard]] bool narrowing() const noexcept
        {
            return !lowerFilter.empty() || state.scope != config::SidebarScope::All;
        }

        /// @return Whether @p row holds the filter, or there is none.
        [[nodiscard]] bool textMatches(SidebarRow const& row) const
        {
            return lowerFilter.empty() || matches(row, lowerFilter);
        }
    };

    /// One section as it is flattened.
    struct Section
    {
        DaemonSection const& meta;           ///< Which daemon, its link, its name; its directory is unused.
        WorkspaceDirectory const& directory; ///< Its workspaces.
        TabAttention const& marks;           ///< The seen-state its panes are read with.
        SectionHeading heading {};           ///< Whether a Daemon row heads it.
        std::size_t position = 0;            ///< Its place among the sections.
        DaemonRowLink link = DaemonRowLink::Observer; ///< Who holds its daemon's link.
    };

    /// What was last known of a pane says nothing of what runs there now: a last-known row draws no running
    /// mark, no progress and no live time.
    void dropLiveClaims(SidebarRow& row)
    {
        if (row.glyph == TabBadgeKind::Working)
            row.glyph = TabBadgeKind::None;
        if (row.time.kind == TimeKind::Live || row.time.kind == TimeKind::Progress)
            row.time = TimeText {};
    }

    /// @return The key of a row of @p section.
    [[nodiscard]] SidebarRowKey keyOf(
        Section const& section, SidebarRowKind kind, uint64_t workspace, uint64_t tab, PaneKey pane)
    {
        // A Daemon row is keyed by its daemon alone: a connect gives its section a run, and it stays the same
        // row, keeping the keyboard and an open menu.
        return SidebarRowKey { .kind = kind,
                               .daemon = section.meta.daemon,
                               .instance =
                                   kind == SidebarRowKind::Daemon ? DaemonInstance {} : section.meta.instance,
                               .workspace = workspace,
                               .tab = tab,
                               .pane = pane };
    }

    /// @return Whether @p target is on @p section's daemon, in the run its ids belong to.
    [[nodiscard]] bool inSection(std::optional<SidebarTarget> const& target, Section const& section)
    {
        return target && target->daemon == section.meta.daemon && target->instance == section.meta.instance;
    }

    /// @return Whether the row @p key names lies on the way to @p target: its workspace, its tab, its pane.
    [[nodiscard]] bool onPathTo(std::optional<SidebarTarget> const& target, SidebarRowKey const& key)
    {
        if (!target || target->daemon != key.daemon || target->instance != key.instance
            || target->workspace != key.workspace)
            return false;
        switch (key.kind)
        {
            case SidebarRowKind::Workspace: return true;
            case SidebarRowKind::Tab: return target->tab == key.tab;
            case SidebarRowKind::Pane: return target->tab == key.tab && target->pane == key.pane;
            case SidebarRowKind::Daemon:
            case SidebarRowKind::Hidden: break;
        }
        return false;
    }

    /// @return Whether the Needs-me scope lets a row with @p key and @p urgency stay of its own accord: it
    ///         needs the user, or it is on the way to the displayed tab or to a pending jump. Always under
    ///         All.
    [[nodiscard]] bool scopeKeeps(Building const& context, SidebarRowKey const& key, Urgency urgency)
    {
        return context.state.scope == config::SidebarScope::All || needsMe(urgency)
               || onPathTo(context.displayed, key) || std::ranges::any_of(JumpModes, [&](JumpMode mode) {
                      return onPathTo(context.pending.of(mode), key);
                  });
    }

    TabBlock tabBlockOf(Building const& context,
                        Section const& section,
                        DirectoryWorkspace const& workspace,
                        DirectoryTab const& tab,
                        std::size_t position,
                        std::vector<LeafStatus>& leavesOut)
    {
        auto leaves = std::vector<LeafStatus> {};
        leaves.reserve(tab.panes.size());
        for (auto const& pane: tab.panes)
            leaves.push_back(leafOf(pane, section.marks.of(pane.key), context.now));

        auto block = TabBlock {};
        auto& row = block.row;
        row.key = keyOf(section, SidebarRowKind::Tab, workspace.id, tab.id, {});
        row.depth = 1;
        row.position = position;
        auto const& fallbackTitle = tab.panes.empty() ? std::string {} : tab.panes.front().title;
        row.title = rowTextOf(!tab.name.empty() ? tab.name : fallbackTitle);
        if (row.title.empty())
            row.title = "Terminal";
        row.selection = inSection(context.displayed, section) && context.displayed->workspace == workspace.id
                                && context.displayed->tab == tab.id
                            ? RowSelection::Selected
                            : RowSelection::None;
        row.tabColor = tab.color;
        row.workspaceColor = workspace.color;
        adopt(row, mostUrgent(leaves));
        row.leaves = countsOf(leaves);

        auto const narrowing = context.narrowing();
        auto const split = tab.panes.size() > 1;
        auto const folded =
            context.state.collapsedTabs.contains(TabKey { .workspace = { .daemon = section.meta.daemon,
                                                                         .instance = section.meta.instance,
                                                                         .workspace = workspace.id },
                                                          .tab = tab.id });
        if (split && (narrowing || !folded))
        {
            for (auto const& [panePosition, pane, leaf]:
                 std::views::zip(std::views::iota(std::size_t { 0 }), tab.panes, leaves))
            {
                auto paneRow = SidebarRow {};
                paneRow.key = keyOf(section, SidebarRowKind::Pane, workspace.id, tab.id, pane.key);
                paneRow.depth = 2;
                paneRow.position = panePosition;
                paneRow.tabColor = tab.color;
                paneRow.workspaceColor = workspace.color;
                auto const& named = !pane.title.empty() ? pane.title : pane.cwd;
                paneRow.title = rowTextOf(named);
                if (paneRow.title.empty())
                    paneRow.title = std::format("Pane {}", panePosition + 1);
                adopt(paneRow, leaf);
                paneRow.leaves = countsOf(std::span { &leaf, 1 });
                if (context.textMatches(paneRow) && scopeKeeps(context, paneRow.key, paneRow.urgency))
                    block.panes.push_back(std::move(paneRow));
            }
        }
        if (!split)
            row.expansion = RowExpansion::None;
        else if (narrowing)
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
    /// @param displayedTab The tab the window displays, already known to be on the workspace's daemon.
    /// @return ContainsSelected when the workspace holds that tab and none of @p tabs is it; else None.
    [[nodiscard]] RowSelection workspaceSelectionOf(DirectoryWorkspace const& workspace,
                                                    std::span<TabBlock const> tabs,
                                                    std::optional<SidebarTarget> const& displayedTab)
    {
        auto const holdsDisplayed =
            displayedTab && displayedTab->tab && displayedTab->workspace == workspace.id
            && std::ranges::contains(workspace.tabs, *displayedTab->tab, &DirectoryTab::id);
        auto const showsDisplayed = std::ranges::any_of(
            tabs, [](TabBlock const& block) { return block.row.selection == RowSelection::Selected; });
        return holdsDisplayed && !showsDisplayed ? RowSelection::ContainsSelected : RowSelection::None;
    }

    /// @param parts The texts, in order.
    /// @return The parts that are not empty, joined by LabelSeparator.
    [[nodiscard]] std::string joinedParts(std::initializer_list<std::string const*> parts)
    {
        auto joined = std::string {};
        for (auto const* part: parts)
        {
            if (part->empty())
                continue;
            if (!joined.empty())
                joined += LabelSeparator;
            joined += *part;
        }
        return joined;
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

namespace
{
    /// Finishes @p heading, @p section's Daemon row, once its workspaces are known: its urgency and counts
    /// roll up theirs, and it holds the selection and the pending jump its rows do not show.
    /// @param sectionWorkspaces Every Workspace row of the section, listed or folded away.
    /// @param below The rows listed beneath the heading.
    void finishHeading(Building const& context,
                       Section const& section,
                       SidebarRow& heading,
                       std::span<SidebarRow const> sectionWorkspaces,
                       std::span<SidebarRow const> below)
    {
        heading.position = section.position;
        heading.linkState = section.meta.state;
        heading.linkReason = rowTextOf(section.meta.reason);
        heading.locality = section.meta.locality;
        heading.link = section.link;
        if (!sectionWorkspaces.empty())
        {
            auto const& best = *std::ranges::max_element(sectionWorkspaces, {}, &SidebarRow::urgency);
            heading.urgency = best.urgency;
            heading.glyph = best.glyph;
            for (auto const& workspaceRow: sectionWorkspaces)
            {
                heading.leaves.needYou += workspaceRow.leaves.needYou;
                heading.leaves.working += workspaceRow.leaves.working;
            }
        }
        if (section.directory.workspaces.empty())
            heading.expansion = RowExpansion::None;
        else if (context.narrowing())
            heading.expansion = below.empty() ? RowExpansion::Collapsed : RowExpansion::Expanded;
        else
            heading.expansion = context.state.foldedDaemons.contains(section.meta.daemon)
                                    ? RowExpansion::Collapsed
                                    : RowExpansion::Expanded;
        // A fold or a filter that hides the displayed tab hands its selection to the daemon row.
        auto const& displayed = context.displayed;
        auto const holdsDisplayed =
            inSection(displayed, section) && displayed->tab
            && std::ranges::any_of(section.directory.workspaces, [&](DirectoryWorkspace const& workspace) {
                   return workspace.id == displayed->workspace
                          && std::ranges::contains(workspace.tabs, *displayed->tab, &DirectoryTab::id);
               });
        auto const showsDisplayed =
            std::ranges::any_of(below, [](SidebarRow const& r) { return r.selection != RowSelection::None; });
        heading.selection =
            holdsDisplayed && !showsDisplayed ? RowSelection::ContainsSelected : RowSelection::None;
        if (std::ranges::any_of(JumpModes,
                                [&](JumpMode mode) { return inSection(context.pending.of(mode), section); }))
            heading.pending = RowPending::Yes;
    }

    /// Appends @p section's rows to @p listing: its Daemon row when it has a heading, then its workspaces
    /// with their tabs and panes, as the state folds them and the filter and the scope narrow them.
    void appendSection(Building const& context, Section const& section, SidebarListing& listing)
    {
        auto const& state = context.state;
        auto const narrowing = context.narrowing();
        auto const& meta = section.meta;
        auto& rows = listing.rows;
        auto const firstRow = rows.size();
        auto const firstWorkspace = listing.workspaces.size();
        auto const headed = section.heading == SectionHeading::Shown;

        auto const daemonName = rowTextOf(meta.name);

        auto daemonRow = SidebarRow {};
        daemonRow.key = keyOf(section, SidebarRowKind::Daemon, 0, 0, {});
        daemonRow.daemonInstance = meta.instance;
        daemonRow.title = daemonName;
        auto const daemonMatches = headed && !context.lowerFilter.empty()
                                   && matches(daemonRow, context.lowerFilter)
                                   && state.scope == config::SidebarScope::All;
        auto const daemonFolded = headed && state.foldedDaemons.contains(meta.daemon);
        if (headed)
            rows.push_back(daemonRow); // finished below, once its workspaces are known

        for (auto const& [position, workspace]: core::views::enumerate(section.directory.workspaces))
        {
            auto const workspaceKey =
                WorkspaceKey { .daemon = meta.daemon, .instance = meta.instance, .workspace = workspace.id };
            auto const folded = state.foldsByDefault(meta.daemon) != state.toggled.contains(workspaceKey);
            auto leaves = std::vector<LeafStatus> {};
            auto tabs = std::vector<TabBlock> {};
            for (auto const& [tabPosition, tab]: core::views::enumerate(workspace.tabs))
            {
                auto block = tabBlockOf(
                    context, section, workspace, tab, static_cast<std::size_t>(tabPosition), leaves);
                if (!block.panes.empty()
                    || (context.textMatches(block.row)
                        && scopeKeeps(context, block.row.key, block.row.urgency)))
                    tabs.push_back(std::move(block));
            }
            if (!narrowing && folded)
                tabs.clear();

            auto row = SidebarRow {};
            row.key = keyOf(section, SidebarRowKind::Workspace, workspace.id, 0, {});
            row.position = static_cast<std::size_t>(position);
            row.title = rowTextOf(workspaceTitleOf(workspace, static_cast<std::size_t>(position)));
            if (workspace.git)
                row.gitText = gitTextOf(*workspace.git);
            row.workspaceColor = workspace.color;
            auto const best = mostUrgent(leaves);
            row.urgency = best.urgency;
            row.glyph = best.glyph;
            row.leaves = countsOf(leaves);
            row.shownElsewhere =
                meta.shownElsewhere.contains(workspace.id) ? ShownElsewhere::Yes : ShownElsewhere::No;
            if (workspace.tabs.empty())
                row.expansion = RowExpansion::None;
            else if (narrowing)
                row.expansion = tabs.empty() ? RowExpansion::Collapsed : RowExpansion::Expanded;
            else
                row.expansion = folded ? RowExpansion::Collapsed : RowExpansion::Expanded;

            // A fold that hides the displayed tab hands its selection to the workspace, the nearest row
            // shown.
            row.selection = inSection(context.displayed, section)
                                ? workspaceSelectionOf(workspace, tabs, context.displayed)
                                : RowSelection::None;
            listing.workspaces.push_back(row);
            if (!narrowing && daemonFolded)
                continue;
            // A daemon whose name the filter holds keeps its workspaces, folded unless a tab matches too.
            auto const kept = !tabs.empty() || daemonMatches
                              || (context.textMatches(row) && scopeKeeps(context, row.key, row.urgency));
            if (narrowing && !kept)
                continue;
            rows.push_back(std::move(row));
            appendTabRows(rows, tabs);
        }

        auto const sectionWorkspaces = std::span { listing.workspaces }.subspan(firstWorkspace);
        auto const shownBelow = rows.size() > firstRow + (headed ? 1 : 0);
        if (headed)
            finishHeading(context,
                          section,
                          rows[firstRow],
                          sectionWorkspaces,
                          std::span<SidebarRow const> { rows }.subspan(firstRow + 1));

        // A pending jump's row is the deepest row shown on the way to its target.
        auto const sectionRows = std::span { rows }.subspan(firstRow);
        auto const newestFirst = sectionRows | std::views::reverse;
        for (auto const mode: JumpModes)
            if (auto const target = std::ranges::find_if(
                    newestFirst,
                    [&](SidebarRow const& r) { return onPathTo(context.pending.of(mode), r.key); });
                target != newestFirst.end())
                target->pending = RowPending::Yes;

        // A section whose link is down lists what was last known of it.
        auto const staleness = section.directory.workspaces.empty()
                                   ? RowStaleness::Live
                                   : LinkSlotRows[static_cast<std::size_t>(meta.state)].tree;
        auto const finish = [&](SidebarRow& row) {
            row.staleness = staleness;
            if (staleness == RowStaleness::LastKnown)
                dropLiveClaims(row);
            row.daemonName = daemonName;
            // Only a Daemon row speaks for the link; the rows beneath it say what they ask for.
            auto const link =
                row.key.kind == SidebarRowKind::Daemon ? meta.state : DaemonLinkState::Connected;
            row.slot = statusSlotOf(link, row.pending, row.urgency);
        };
        std::ranges::for_each(sectionRows, finish);
        std::ranges::for_each(sectionWorkspaces, finish);

        // Narrowed to nothing, a daemon whose name does not match is not listed at all.
        if (headed && narrowing && !shownBelow && !daemonMatches)
            rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(firstRow));
    }
} // namespace

SidebarListing sidebarListing(SidebarSources const& sources,
                              SidebarState const& state,
                              std::chrono::system_clock::time_point now,
                              std::optional<SidebarTarget> displayed,
                              PendingJumps pending)
{
    static auto const nothingSeen = TabAttention {};
    auto const context = Building { .state = state,
                                    .now = now,
                                    .lowerFilter = asciiLowered(state.filter),
                                    .displayed = std::move(displayed),
                                    .pending = std::move(pending) };
    auto listing = SidebarListing {};
    for (auto const& [index, section]: core::views::enumerate(sources.sections))
    {
        auto const position = static_cast<std::size_t>(index);
        auto const& marks =
            section.daemon == sources.own && sources.ownMarks != nullptr ? *sources.ownMarks : nothingSeen;
        auto const link = [&] {
            if (section.feed == SectionFeed::Observer)
                return DaemonRowLink::Observer;
            return section.daemon == sources.own ? DaemonRowLink::OwnWindow : DaemonRowLink::OtherWindow;
        }();
        appendSection(context,
                      Section { .meta = section,
                                .directory = section.directory,
                                .marks = marks,
                                .heading = SectionHeading::Shown,
                                .position = position,
                                .link = link },
                      listing);
    }
    if (!sources.hidden.empty() && state.scope == config::SidebarScope::All)
    {
        auto row = SidebarRow {};
        row.key.kind = SidebarRowKind::Hidden;
        row.position = sources.sections.size();
        row.hidden = std::ranges::to<std::vector>(sources.hidden);
        listing.rows.push_back(std::move(row));
    }
    return listing;
}

SidebarListing sidebarListing(WorkspaceDirectory const& directory,
                              TabAttention const& attention,
                              SidebarState const& state,
                              std::chrono::system_clock::time_point now,
                              std::optional<TabRef> displayedTab)
{
    // No daemon: one section, named by nothing, whose link is as good as connected.
    static auto const noDaemon = DaemonSection { .daemon = {},
                                                 .instance = {},
                                                 .name = {},
                                                 .locality = DaemonLocality::Local,
                                                 .state = DaemonLinkState::Connected,
                                                 .reason = {},
                                                 .directory = {},
                                                 .shownElsewhere = {},
                                                 .feed = SectionFeed::WindowLink };
    auto const displayed = displayedTab
                               ? std::optional { SidebarTarget { .daemon = {},
                                                                 .instance = {},
                                                                 .workspace = displayedTab->workspace,
                                                                 .tab = displayedTab->tab,
                                                                 .pane = std::nullopt } }
                               : std::nullopt;
    auto const context = Building { .state = state,
                                    .now = now,
                                    .lowerFilter = asciiLowered(state.filter),
                                    .displayed = displayed,
                                    .pending = {} };
    auto listing = SidebarListing {};
    appendSection(context,
                  Section { .meta = noDaemon,
                            .directory = directory,
                            .marks = attention,
                            .heading = SectionHeading::Omitted,
                            .position = 0 },
                  listing);
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
    // What was last known of a daemon is not counted: nothing says it still waits or works.
    for (auto const& row: rows | std::views::filter([](SidebarRow const& r) {
                              return r.key.kind == SidebarRowKind::Workspace
                                     && r.staleness == RowStaleness::Live;
                          }))
    {
        footer.needYou += row.leaves.needYou;
        footer.working += row.leaves.working;
    }
    return footer;
}

namespace
{
    /// One of the footer's counts and the status whose glyph says it.
    struct FooterGlyph
    {
        uint32_t SidebarFooter::* count; ///< The count.
        TabBadgeKind status;             ///< The status a row draws for what it counts.
    };

    /// The footer's counts in the order its text names them.
    constexpr auto FooterGlyphs = std::array {
        FooterGlyph { .count = &SidebarFooter::needYou, .status = TabBadgeKind::Blocked },
        FooterGlyph { .count = &SidebarFooter::working, .status = TabBadgeKind::Working },
    };
} // namespace

std::string sidebarFooterGlyphText(SidebarFooter const& footer, config::SidebarLook look)
{
    auto text = std::string {};
    for (auto const& glyph:
         FooterGlyphs | std::views::filter([&](FooterGlyph const& g) { return footer.*g.count > 0; }))
    {
        if (!text.empty())
            text += " · ";
        text += std::format("{} {}", tabBadgeGlyph(glyph.status, look), footer.*glyph.count);
    }
    return text;
}

SidebarSummary sidebarSummary(std::span<SidebarRow const> rows, std::optional<WorkspaceKey> const& current)
{
    auto summary = SidebarSummary {};
    for (auto const& row: rows | std::views::filter([](SidebarRow const& r) {
                              return r.key.kind == SidebarRowKind::Workspace;
                          }))
    {
        if (current
            == WorkspaceKey {
                .daemon = row.key.daemon, .instance = row.key.instance, .workspace = row.key.workspace })
            summary.currentWorkspace = row.title;
        else if (row.staleness == RowStaleness::Live && row.urgency > summary.otherWorkspaces)
        {
            summary.otherWorkspaces = row.urgency;
            summary.otherWorkspacesGlyph = row.glyph;
        }
    }
    return summary;
}

bool isUnnarrowed(SidebarState const& state) noexcept
{
    return state.filter.empty() && state.scope == config::SidebarScope::All;
}

std::optional<std::size_t> firstMatchOf(std::span<SidebarRow const> rows, SidebarState const& state)
{
    auto const lowerFilter = asciiLowered(state.filter);
    auto const isLeafward = [](SidebarRow const& row) {
        return row.key.kind == SidebarRowKind::Tab || row.key.kind == SidebarRowKind::Pane;
    };
    auto const isWorkspace = [](SidebarRow const& row) {
        return row.key.kind == SidebarRowKind::Workspace;
    };
    auto const itselfMatches = [&](SidebarRow const& row) {
        if (!lowerFilter.empty())
            return matches(row, lowerFilter);
        // A tab's urgency rolls up its panes': while they are listed, the one that asks is the match.
        return needsMe(row.urgency)
               && (row.key.kind != SidebarRowKind::Tab || row.expansion != RowExpansion::Expanded);
    };
    using Rule = std::function<bool(SidebarRow const&)>;
    auto const preferences = std::array<Rule, 4> {
        [&](SidebarRow const& row) { return isLeafward(row) && itselfMatches(row); },
        [&](SidebarRow const& row) { return isWorkspace(row) && itselfMatches(row); },
        isLeafward,
        isWorkspace,
    };
    for (auto const& preferred: preferences)
        if (auto const found = std::ranges::find_if(rows, preferred); found != rows.end())
            return static_cast<std::size_t>(std::distance(rows.begin(), found));
    return std::nullopt;
}

bool needsMe(Urgency urgency) noexcept
{
    return std::ranges::contains(JumpClasses, urgency);
}

std::size_t rowsNeedingMe(std::span<SidebarRow const> rows) noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(rows, [](SidebarRow const& row) { return needsMe(row.urgency); }));
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
    return joinedParts({ &row.program, &row.fullFolder, &row.time.text });
}

std::string idleLineOf(SidebarRow const& row)
{
    if (secondLineOf(row) == SecondLine::None)
        return {};
    return joinedParts({ &row.folder, &row.program });
}

int parentRowOf(std::span<SidebarRow const> rows, int row) noexcept
{
    if (row <= 0 || std::cmp_greater_equal(row, rows.size()))
        return -1;
    auto const& shown = rows[static_cast<std::size_t>(row)];
    // A workspace row shares its depth with its daemon's row, which it hangs under all the same.
    auto const isParent = [&](SidebarRow const& above) {
        switch (shown.key.kind)
        {
            case SidebarRowKind::Daemon:
            case SidebarRowKind::Hidden: return false;
            case SidebarRowKind::Workspace: return above.key.kind == SidebarRowKind::Daemon;
            case SidebarRowKind::Tab:
            case SidebarRowKind::Pane: break;
        }
        return above.depth < shown.depth;
    };
    for (auto const above: std::views::iota(0, row) | std::views::reverse)
        if (isParent(rows[static_cast<std::size_t>(above)]))
            return above;
    return -1;
}

namespace
{
    /// @return The cell @p glyph is drawn as.
    [[nodiscard]] std::string_view treeGlyphText(TreeGlyph glyph) noexcept
    {
        switch (glyph)
        {
            case TreeGlyph::None: break;
            case TreeGlyph::Tee: return "├";
            case TreeGlyph::Last: return "└";
            case TreeGlyph::Bar: return "│";
        }
        return " ";
    }

    /// @return @p glyph as the workspace rail draws it: a connector whose stroke reaches the cell's bottom.
    [[nodiscard]] TreeGlyph railGlyphOf(TreeGlyph glyph) noexcept
    {
        switch (glyph)
        {
            case TreeGlyph::None:
            case TreeGlyph::Bar: break;
            case TreeGlyph::Tee:
            case TreeGlyph::Last: return TreeGlyph::Tee;
        }
        return TreeGlyph::Bar;
    }
} // namespace

std::string treeLinesText(TreeLines const& lines, uint8_t depth)
{
    auto text = std::string {};
    for (auto const glyph: lines.columns | std::views::take(depth))
    {
        text += treeGlyphText(glyph);
        text += ' ';
    }
    return text;
}

std::string treeContinuationText(SidebarRow const& row)
{
    return row.depth == 0 ? std::string {}
                          : treeLinesText(row.continued, static_cast<uint8_t>(row.continued.columns.size()));
}

RailCells workspaceRailCellsOf(SidebarRow const& row)
{
    if (row.key.kind == SidebarRowKind::Daemon || row.key.kind == SidebarRowKind::Hidden)
        return {};
    if (row.depth == 0)
        return RailCells { .first = row.expansion == RowExpansion::None
                                        ? std::string { treeGlyphText(TreeGlyph::Bar) }
                                        : std::string {},
                           .second = {} };
    return RailCells { .first = std::string { treeGlyphText(railGlyphOf(row.lines.columns.front())) },
                       .second = std::string { treeGlyphText(railGlyphOf(row.continued.columns.front())) } };
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

std::string rowTextOf(std::string_view text, std::size_t maxLength)
{
    return sanitizedTo(text, maxLength);
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
