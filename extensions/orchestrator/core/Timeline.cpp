// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Timeline.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// The states in which a run's agent is active: its tab is open and it has not ended.
    [[nodiscard]] bool isActiveState(std::string_view state) noexcept
    {
        return state == "running"sv || state == "blocked"sv || state == "done"sv;
    }

    /// When a run's agent was active: from the first active state it entered to the first state after that
    /// is not one; until now while it still is.
    struct Interval
    {
        uint64_t from = 0;
        uint64_t to = 0;
    };

    /// @return Whether @p run's agent is active now, whatever its history says.
    [[nodiscard]] bool isLive(Run const& run) noexcept
    {
        return std::holds_alternative<Running>(run.state) || std::holds_alternative<Blocked>(run.state)
               || std::holds_alternative<Done>(run.state);
    }

    [[nodiscard]] std::optional<Interval> activeIntervalOf(Run const& run, uint64_t nowWallMs)
    {
        auto const first = std::ranges::find_if(run.history, isActiveState, &StateChange::state);
        // A live run stored before runs kept a history: active from its dispatch, as near as is known.
        if (first == run.history.end())
            return isLive(run) ? std::optional { Interval { .from = run.startedWallMs,
                                                            .to = std::max(nowWallMs, run.startedWallMs) } }
                               : std::nullopt;
        auto const last =
            std::ranges::find_if(std::ranges::subrange(first, run.history.end()),
                                 [](StateChange const& change) { return !isActiveState(change.state); });
        auto const to = last != run.history.end() ? last->wallMs : nowWallMs;
        return Interval { .from = first->wallMs, .to = std::max(to, first->wallMs) };
    }

    /// @return The bucket @p wallMs falls in; nothing outside the window.
    [[nodiscard]] std::optional<std::size_t> bucketOf(uint64_t wallMs,
                                                      uint64_t windowStart,
                                                      uint64_t nowWallMs)
    {
        if (wallMs < windowStart || wallMs > nowWallMs)
            return std::nullopt;
        return std::min<std::size_t>((wallMs - windowStart) / TimelineBucketMs, TimelineBuckets - 1);
    }
} // namespace

Timeline timelineOf(std::span<Run const> runs, uint64_t nowWallMs)
{
    auto timeline = Timeline {};
    auto const span = TimelineBucketMs * TimelineBuckets;
    auto const windowStart = nowWallMs > span ? nowWallMs - span : 0;
    for (auto const& run: runs)
    {
        if (auto const bucket = bucketOf(run.startedWallMs, windowStart, nowWallMs))
            ++timeline.started[*bucket];
        if (auto const* const finished = std::get_if<Finished>(&run.state))
            if (auto const bucket = bucketOf(finished->endedWallMs, windowStart, nowWallMs))
                ++timeline.ended[*bucket];

        auto const interval = activeIntervalOf(run, nowWallMs);
        if (!interval || interval->to < windowStart)
            continue;
        if (isLive(run))
            ++timeline.activeNow;
        auto const tokens = run.stats ? run.stats->tokens.output + run.stats->subagents.tokens.output : 0;
        auto const length = interval->to - interval->from;
        for (auto const index: std::views::iota(std::size_t { 0 }, TimelineBuckets))
        {
            auto const bucketFrom = windowStart + (index * TimelineBucketMs);
            auto const bucketTo = bucketFrom + TimelineBucketMs;
            auto const overlapFrom = std::max(bucketFrom, interval->from);
            auto const overlapTo = std::min(bucketTo, interval->to);
            // An agent active for an instant still counts in its hour.
            if (overlapFrom > overlapTo || (overlapFrom == overlapTo && length != 0))
                continue;
            ++timeline.active[index];
            timeline.tokens[index] += length == 0 ? tokens : tokens * (overlapTo - overlapFrom) / length;
        }
    }
    return timeline;
}

} // namespace orchestrator
