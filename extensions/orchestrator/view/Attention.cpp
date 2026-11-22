// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Attention.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator::view
{

namespace
{
    /// @return Whether @p seen and @p now are both known and differ.
    [[nodiscard]] constexpr bool changed(uint64_t seen, uint64_t now) noexcept
    {
        return seen != 0 && now != 0 && seen != now;
    }

    /// Whether a tab is the one shown.
    enum class TabShown : uint8_t
    {
        No = 0, ///< Its content is seen only the first time it is known.
        Yes,    ///< Its content is seen as it is now.
    };

    /// Takes @p now as seen when it is known and the tab is shown, or nothing was seen of it yet.
    constexpr void adopt(uint64_t& seen, uint64_t now, TabShown shown) noexcept
    {
        if (now != 0 && (shown == TabShown::Yes || seen == 0))
            seen = now;
    }
} // namespace

RunMarks runMarksOf(Run const& run)
{
    return RunMarks { .state = rowStateOf(run),
                      .changedWallMs = waitingSinceOf(run),
                      .log = run.stats ? run.stats->last : std::string {} };
}

TabMarks tabMarksOf(Engine const& engine, std::string_view tab, std::string_view run)
{
    auto marks = TabMarks {};
    if (auto const* const board = engine.board(run); board != nullptr && board->state == BoardState::Read)
        marks.board = board->modifiedWallMs;
    // Only the last page tells how far the transcript has come.
    if (auto const* const view = engine.transcript(tab, run);
        view != nullptr && view->content && view->content->page == view->content->pages)
        marks.transcript = (uint64_t { view->content->pages } << 32U) | view->content->entries.size();
    return marks;
}

void SeenLedger::look(std::string_view run, RunMarks const& marks, TabMarks const& tabs, FocusTab shown)
{
    auto seen = _seen.find(run);
    if (seen == _seen.end())
        seen = _seen.emplace(std::string { run }, Seen {}).first;
    seen->second.run = marks;
    adopt(seen->second.tabs.board, tabs.board, shown == FocusTab::Board ? TabShown::Yes : TabShown::No);
    adopt(seen->second.tabs.transcript,
          tabs.transcript,
          shown == FocusTab::Transcript ? TabShown::Yes : TabShown::No);
}

News SeenLedger::newsOf(std::string_view run, RunMarks const& marks)
{
    auto const seen = _seen.find(run);
    if (seen == _seen.end())
    {
        _seen.emplace(std::string { run }, Seen { .run = marks, .tabs = {} });
        return News::None;
    }
    return seen->second.run == marks ? News::None : News::Unseen;
}

News SeenLedger::tabNewsOf(std::string_view run, TabMarks const& tabs, FocusTab tab) const
{
    auto const seen = _seen.find(run);
    if (seen == _seen.end())
        return News::None;
    switch (tab)
    {
        case FocusTab::Board: return changed(seen->second.tabs.board, tabs.board) ? News::Unseen : News::None;
        case FocusTab::Transcript:
            return changed(seen->second.tabs.transcript, tabs.transcript) ? News::Unseen : News::None;
        case FocusTab::Details: break;
    }
    return News::None;
}

std::vector<std::string> needingRuns(std::span<Row const> rows)
{
    auto runs = std::vector<std::string> {};
    for (auto const& row: rows)
        if (!row.run.empty() && row.attention == FleetGroup::NeedsYou)
            runs.push_back(row.run);
    std::ranges::sort(runs);
    return runs;
}

std::vector<std::string> needsYouEntries(std::span<std::string const> before,
                                         std::span<std::string const> now)
{
    auto entries = std::vector<std::string> {};
    std::ranges::set_difference(now, before, std::back_inserter(entries));
    return entries;
}

} // namespace orchestrator::view
