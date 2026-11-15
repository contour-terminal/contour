// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/RunChanges.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

    [[nodiscard]] Rechecking recheckingOf(Run const& run) noexcept
    {
        auto const* const ending = std::get_if<Ending>(&run.state);
        return ending != nullptr && ending->resumedFrom ? Rechecking::Yes : Rechecking::No;
    }

    /// `<id>: <old> → <new>`, then the kept worktree's further reasons beneath it.
    void say(std::vector<std::string>& lines, std::string_view old, RunSnapshot const& now)
    {
        lines.push_back(std::format("{}: {} → {}", now.id, old, now.state));
        for (auto const& reason: now.moreKept)
            lines.push_back(std::format("{}{}", ReasonIndent, reason));
    }

    /// A re-check the user asked for that ended where it started.
    void sayUnchanged(std::vector<std::string>& lines, RunSnapshot const& now)
    {
        lines.push_back(std::format("{}: checked again, unchanged: {}", now.id, now.state));
        for (auto const& reason: now.moreKept)
            lines.push_back(std::format("{}{}", ReasonIndent, reason));
    }

    [[nodiscard]] bool sameOutcome(RunSnapshot const& a, RunSnapshot const& b) noexcept
    {
        return a.state == b.state && a.moreKept == b.moreKept;
    }
} // namespace

std::vector<RunSnapshot> snapshotRuns(std::span<Run const> runs)
{
    auto snapshots = std::vector<RunSnapshot> {};
    snapshots.reserve(runs.size());
    for (auto const& run: runs)
        snapshots.push_back(RunSnapshot { .id = run.id,
                                          .state = runStateText(run),
                                          .moreKept = moreKeptOf(run),
                                          .rechecking = recheckingOf(run) });
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
        say(lines, old, now);
    }
    for (auto const& then: before)
        if (!std::ranges::contains(after, then.id, &RunSnapshot::id))
            lines.push_back(std::format("{}: {} → {}", then.id, then.state, Gone));
    return lines;
}

void ChangeReport::requested(std::string runId)
{
    if (!std::ranges::contains(_requested, runId))
        _requested.push_back(std::move(runId));
}

Asker ChangeReport::takeRequest(std::string_view runId)
{
    auto const erased = std::erase(_requested, runId);
    return erased > 0 ? Asker::User : Asker::Poll;
}

std::vector<std::string> ChangeReport::lines(std::span<RunSnapshot const> before,
                                             std::span<RunSnapshot const> after)
{
    auto lines = std::vector<std::string> {};
    for (auto const& now: after)
    {
        auto const then = std::ranges::find(before, now.id, &RunSnapshot::id);
        auto const recheck =
            std::ranges::find(_rechecks, now.id, [](Recheck const& each) { return each.from.id; });
        if (now.rechecking == Rechecking::Yes)
        {
            // Said once it is done, compared with where it started.
            if (recheck == _rechecks.end())
                _rechecks.push_back(
                    Recheck { .from = then != before.end() ? *then : now, .asker = takeRequest(now.id) });
            continue;
        }
        auto from = std::optional<RunSnapshot> {};
        auto asker = Asker::Poll;
        if (recheck != _rechecks.end())
        {
            from = recheck->from;
            asker = recheck->asker;
            _rechecks.erase(recheck);
        }
        else if (then != before.end() && takeRequest(now.id) == Asker::User)
        {
            // Asked for, but the check never started: the user still gets the answer.
            from = *then;
            asker = Asker::User;
        }
        if (from)
        {
            if (!sameOutcome(*from, now))
                say(lines, from->state, now);
            else if (asker == Asker::User)
                sayUnchanged(lines, now);
            continue;
        }
        if (then != before.end() && sameOutcome(*then, now))
            continue;
        say(lines, then != before.end() ? std::string_view { then->state } : Appeared, now);
    }
    for (auto const& then: before)
        if (!std::ranges::contains(after, then.id, &RunSnapshot::id))
        {
            std::erase_if(_rechecks, [&](Recheck const& each) { return each.from.id == then.id; });
            std::erase(_requested, then.id);
            lines.push_back(std::format("{}: {} → {}", then.id, then.state, Gone));
        }
    return lines;
}

} // namespace orchestrator::proto
