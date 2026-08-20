// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/Folding.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace vtbackend
{

namespace
{
    /// Where a backward fold walk stands between two command blocks.
    ///
    /// The same three states scanCommandBlocksBackward() walks, and deliberately so: the two read the
    /// very same marks, and a fold that disagreed with the block the "copy last command" actions hand to
    /// the clipboard would be a fold around something other than what it claims.
    enum class ScanState : uint8_t
    {
        Searching, ///< Looking for the CommandEnd that closes the next block.
        InOutput,  ///< Walking up through the command's output towards its OutputStart.
        InPrompt,  ///< Walking up through the prompt towards the Marked line that begins it.
    };

    /// The hidden rows collected for the block currently being walked.
    ///
    /// Stable ids taken as the walk passes each line, rather than indices looked up again at the end:
    /// the source is asked about each index once, in increasing order, which is the contract that lets it
    /// climb the grid lazily and keep no more state than the line it is standing on.
    struct OutputUnderConstruction
    {
        std::optional<int64_t> bottomId; ///< Last physical row of the LOWEST hidden line -- the largest id.
        std::optional<int64_t> topId;    ///< Head of the HIGHEST hidden line -- the smallest id.

        [[nodiscard]] bool empty() const noexcept { return !bottomId.has_value(); }

        /// Adds one logical line, which the walk reaches in increasing index (decreasing id) order.
        /// @param headId Its head's stable id.
        /// @param lastPhysicalId The stable id of the last physical row it occupies.
        void add(int64_t headId, int64_t lastPhysicalId) noexcept
        {
            if (!bottomId)
                bottomId = lastPhysicalId;
            topId = headId;
        }
    };
} // namespace

std::vector<FoldRange> computeFoldRanges(FoldLineSource const& lines, size_t maxScanLines)
{
    auto ranges = std::vector<FoldRange> {};

    auto state = ScanState::Searching;
    auto output = OutputUnderConstruction {};

    /// Adds the logical line at @p index to the block's hidden rows.
    auto const hideLineAt = [&](size_t index) {
        output.add(lines.stableIdAt(index), lines.lastPhysicalStableIdAt(index));
    };

    /// Opens the block that the CommandEnd carried by @p flags closes.
    auto const openBlockAt = [&](LineFlags flags) {
        output = {};

        // The line a command was closed on is NEVER hidden, whatever else it carries. A shell emits ;D
        // from its precmd hook, at the cursor, so that line is one of exactly two things: the line the
        // next prompt is about to be printed onto -- the ordinary case, where ;D and ;A land together --
        // or, when the shell has not got that far, the line the cursor is standing on right now. Folding
        // either away would hide the very row the user is about to type into.
        //
        // The price is one visible row for the shell that emits ;D on the last output line and its ;A on
        // the next: that last line stays on screen under a collapsed block. Leaving one row too many is
        // recoverable; folding the cursor away is not.

        // The line may carry the OutputStart of the very command it also ends -- a command whose output
        // fit on the line it started on, or one that printed nothing at all.
        state = flags.contains(LineFlag::OutputStart) ? ScanState::InPrompt : ScanState::InOutput;
    };

    /// Closes the block whose prompt begins at @p index and decides where the walk stands afterwards.
    auto const finalizeBlockAt = [&](size_t index, LineFlags flags) {
        // A block with no whole output line of its own is not foldable: there is nothing to hide, and a
        // marker offering to hide nothing is worse than no marker.
        if (!output.empty())
            ranges.push_back(FoldRange { .headStableId = lines.stableIdAt(index),
                                         .firstStableId = *output.topId,
                                         .lastStableId = *output.bottomId });
        output = {};

        // The line that begins this prompt may ALSO carry the CommandEnd of the block before it, for the
        // back-to-back ;D/;A reason above. Chain straight into that older block rather than resuming the
        // search one line further up, which would step clean over the boundary we are standing on.
        if (flags.contains(LineFlag::CommandEnd))
            openBlockAt(flags);
        else
            state = ScanState::Searching;
    };

    // A single forward pass, asking for each logical line only once and only as far as it needs to go.
    for (auto index = size_t { 0 }; index < maxScanLines && lines.hasLineAt(index); ++index)
    {
        auto const flags = lines.flagsAt(index);

        switch (state)
        {
            case ScanState::Searching:
                if (flags.contains(LineFlag::CommandEnd))
                    openBlockAt(flags);
                break;

            case ScanState::InOutput:
                // A prompt line ends the output whether or not it also carries the OutputStart, and is
                // never itself hidden.
                if (flags.contains(LineFlag::Marked))
                {
                    finalizeBlockAt(index, flags);
                    break;
                }
                hideLineAt(index);
                if (flags.contains(LineFlag::OutputStart))
                    state = ScanState::InPrompt;
                break;

            case ScanState::InPrompt:
                if (flags.contains(LineFlag::Marked))
                    finalizeBlockAt(index, flags);
                break;
        }
    }

    // A walk that ran out of lines mid-block is dropped rather than kept: without the Marked line there is
    // no prompt row to hang the fold off, and hiding output under a head that has scrolled away would
    // leave the user no way to get it back.
    return ranges;
}

void FoldState::collapseAll(std::span<FoldRange const> ranges)
{
    // Through collapse() rather than into the set directly, so the revision is bumped: everything
    // derived from this state -- the projection above all -- keys its cache on that counter, and a
    // sweep that changed the set silently would leave the screen showing what was there before.
    for (auto const& range: ranges)
        collapse(range.headStableId);
}

std::vector<HiddenInterval> hiddenIntervals(std::span<FoldRange const> ranges, FoldState const& state)
{
    auto intervals = std::vector<HiddenInterval> {};
    if (state.empty())
        return intervals;

    // Every range can contribute at most one interval, so this is the exact bound rather than a guess.
    intervals.reserve(ranges.size());

    for (auto const& range: ranges)
        if (state.isCollapsed(range.headStableId))
            intervals.push_back(HiddenInterval { .first = range.firstStableId, .last = range.lastStableId });

    std::ranges::sort(intervals, {}, &HiddenInterval::first);

    // Merge where they touch as well as where they overlap: two blocks whose ranges abut leave no visible
    // line between them, so one interval describes both and the projection's binary search stays a search.
    //
    // In place, over the range just sorted: merging into a second vector would allocate and copy the whole
    // thing again, on a path that runs on every projection rebuild.
    auto out = intervals.begin();
    for (auto const& interval: intervals)
    {
        if (out != intervals.begin() && interval.first <= std::prev(out)->last + 1)
            std::prev(out)->last = std::max(std::prev(out)->last, interval.last);
        else
            *out++ = interval;
    }
    intervals.erase(out, intervals.end());

    return intervals;
}

std::optional<HiddenInterval> hidingInterval(std::span<HiddenInterval const> hidden, int64_t id) noexcept
{
    // The runs are ascending and disjoint, so the only candidate is the last one starting at or before
    // @p id -- one binary search, then one comparison against its end.
    auto const it = std::ranges::upper_bound(hidden, id, {}, &HiddenInterval::first);
    if (it == hidden.begin())
        return std::nullopt;

    auto const& candidate = *std::prev(it);
    if (id > candidate.last)
        return std::nullopt;

    return candidate;
}

int64_t snapToVisibleId(std::span<HiddenInterval const> hidden,
                        int64_t id,
                        VerticalDirection direction) noexcept
{
    auto const run = hidingInterval(hidden, id);
    if (!run)
        return id;

    // One step out is enough: hiddenIntervals() merges runs that merely touch, so the id just past a
    // run's edge can never be inside the next one.
    return direction == VerticalDirection::Up ? run->first - 1 : run->last + 1;
}

int64_t advanceVisibleId(std::span<HiddenInterval const> hidden,
                         int64_t id,
                         int64_t count,
                         VerticalDirection direction,
                         int64_t floorId,
                         int64_t ceilId) noexcept
{
    auto current = std::clamp(snapToVisibleId(hidden, id, direction), floorId, ceilId);
    if (count <= 0)
        return current;

    // Where a walk that runs into the edge of the grid stops. The bound itself is wherever the
    // scrollback happens to end and may well sit inside a collapsed run, so clamping alone would hand
    // back a hidden id -- a row drawn nowhere, which is precisely what counting in visible rows exists
    // to avoid. Snapped INWARD, back the way the walk came, so the motion stops on the last row before
    // the edge that is actually drawn.
    auto const stopAt = [&](int64_t value, VerticalDirection inward) {
        return std::clamp(
            snapToVisibleId(hidden, std::clamp(value, floorId, ceilId), inward), floorId, ceilId);
    };

    auto remaining = count;

    // Walked run by run rather than row by row: the gap before the next run is crossed in one
    // subtraction, so a `1000j` over a collapsed `cat` of a large file costs the same as a `1j`.
    if (direction == VerticalDirection::Down)
    {
        // `current` is visible, so no run contains it and every run from here on begins below it.
        for (auto it = std::ranges::upper_bound(hidden, current, {}, &HiddenInterval::first);
             it != hidden.end() && remaining > 0;
             ++it)
        {
            auto const gap = it->first - current - 1; // the visible ids between `current` and the run
            if (remaining <= gap)
                break; // the target lies in this gap; the clamp below lands on it

            remaining -= gap + 1; // the gap, plus the one step that lands past the run
            current = it->last + 1;
        }
        return stopAt(current + remaining, VerticalDirection::Up);
    }

    // Upwards, the same walk against the run order: ids decrease as the walk climbs.
    auto const firstAtOrBelow = std::ranges::lower_bound(hidden, current, {}, &HiddenInterval::last);
    for (auto it = std::make_reverse_iterator(firstAtOrBelow); it != hidden.rend() && remaining > 0; ++it)
    {
        auto const gap = current - it->last - 1;
        if (remaining <= gap)
            break;

        remaining -= gap + 1;
        current = it->first - 1;
    }
    return stopAt(current - remaining, VerticalDirection::Down);
}

int64_t visibleDistance(std::span<HiddenInterval const> hidden, int64_t first, int64_t last) noexcept
{
    // Symmetric by construction, so no caller has to order its arguments.
    if (last < first)
        return -visibleDistance(hidden, last, first);

    // The span, less whatever the runs overlapping it hide. Only the runs that actually intersect
    // [first, last) are visited, and the binary search finds the first of them.
    auto distance = last - first;
    for (auto it = std::ranges::upper_bound(hidden, first, {}, &HiddenInterval::last);
         it != hidden.end() && it->first < last;
         ++it)
        distance -= std::min(it->last + 1, last) - std::max(it->first, first);

    return distance;
}

std::vector<LineOffset> projectRows(std::span<HiddenInterval const> hidden,
                                    int64_t stableBase,
                                    LineOffset bottom,
                                    LineCount rowCount,
                                    LineOffset top)
{
    auto rows = std::vector<LineOffset> {};
    if (unbox<int>(rowCount) <= 0 || bottom < top)
        return rows;

    rows.reserve(unbox<size_t>(rowCount));
    for (auto line = bottom; line >= top && rows.size() < unbox<size_t>(rowCount); --line)
    {
        auto const id = stableBase + unbox<int64_t>(line);

        // Skip the whole run in one step rather than a line at a time: a collapsed `cat` of a large file
        // hides tens of thousands of rows, and stepping through them per visible row would make the
        // projection quadratic in the size of what the user just hid.
        if (auto const interval = hidingInterval(hidden, id))
        {
            line = LineOffset::cast_from(interval->first - stableBase);
            continue; // the loop's own decrement steps past the top of the run
        }

        rows.push_back(line);
    }

    // Collected bottom-up; the screen reads top-down.
    std::ranges::reverse(rows);
    return rows;
}

} // namespace vtbackend
