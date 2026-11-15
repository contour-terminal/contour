// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/RunChanges.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Finished.hpp>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    /// What precedes the first state of a run that was not there before.
    constexpr auto Appeared = "dispatched"sv;

    /// What follows the last state of a run that is no longer there.
    constexpr auto Gone = "off the list"sv;

    /// How far a kept worktree's further reasons are indented.
    constexpr auto ReasonIndent = "    "sv;

    [[nodiscard]] std::vector<std::string> moreKeptOf(Run const& run)
    {
        auto const* const finished = std::get_if<Finished>(&run.state);
        if (finished == nullptr || finished->fate != Fate::Kept || finished->entries.size() < 2)
            return {};
        auto more = std::vector<std::string>(std::next(finished->entries.begin()), finished->entries.end());
        return more;
    }
} // namespace

std::vector<RunSnapshot> snapshotRuns(std::span<Run const> runs)
{
    auto snapshots = std::vector<RunSnapshot> {};
    snapshots.reserve(runs.size());
    for (auto const& run: runs)
        snapshots.push_back(
            RunSnapshot { .id = run.id, .state = runStateText(run), .moreKept = moreKeptOf(run) });
    return snapshots;
}

std::vector<std::string> changeLines(std::span<RunSnapshot const> before, std::span<RunSnapshot const> after)
{
    auto lines = std::vector<std::string> {};
    for (auto const& now: after)
    {
        auto const then = std::ranges::find(before, now.id, &RunSnapshot::id);
        auto const old = then != before.end() ? std::string_view { then->state } : Appeared;
        if (then != before.end() && then->state == now.state)
            continue;
        lines.push_back(std::format("{}: {} → {}", now.id, old, now.state));
        for (auto const& reason: now.moreKept)
            lines.push_back(std::format("{}{}", ReasonIndent, reason));
    }
    for (auto const& then: before)
        if (!std::ranges::contains(after, then.id, &RunSnapshot::id))
            lines.push_back(std::format("{}: {} → {}", then.id, then.state, Gone));
    return lines;
}

} // namespace orchestrator::proto
