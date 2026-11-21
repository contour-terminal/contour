// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Record.hpp>
#include <orchestrator/core/Stats.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using engine::detail::judge;
    using engine::detail::later;
    using engine::detail::RecordTagSuffix;
    using engine::detail::StatsTagSuffix;
    using engine::detail::Verdict;

    /// The declared command a run's statistics are read with.
    constexpr auto StatsCommand = "agent-stats"sv;

    /// The declared command a finished run is appended to the file of finished runs with.
    constexpr auto RecordCommand = "agent-record"sv;

    /// @return Whether @p tag is a background call's: a statistics read or a record.
    [[nodiscard]] bool isBackground(std::string_view tag) noexcept
    {
        return tag.ends_with(StatsTagSuffix) || tag.ends_with(RecordTagSuffix);
    }

    /// @return Whether @p run's agent has a session to read: its tab opened, and it has not finished.
    [[nodiscard]] bool isLive(Run const& run) noexcept
    {
        return std::holds_alternative<Running>(run.state) || std::holds_alternative<Blocked>(run.state)
               || std::holds_alternative<Done>(run.state);
    }

    /// @return Whether @p run finished after its agent had a session: one whose preparation failed had none.
    [[nodiscard]] bool hadSession(Run const& run) noexcept
    {
        auto const* const finished = std::get_if<Finished>(&run.state);
        return finished != nullptr && finished->ended != Ended::PreparationFailed;
    }
} // namespace

void Engine::readDueStats(std::vector<Effect>& out)
{
    if (!_config)
        return;
    auto const inside = std::span(_runs).first(_insideCount);
    // A run that is no longer live is due no more.
    std::erase_if(_statsDueMs, [&](auto const& due) {
        auto const run = std::ranges::find(inside, due.first, &Run::id);
        return run == inside.end() || !isLive(*run);
    });
    // One call at a time: the statistics and the record, never worth a slot a dashboard is waiting for.
    if (std::ranges::any_of(_inFlight, [](InFlightCall const& call) { return isBackground(call.tag); })
        || std::ranges::any_of(_queue, [](Queued const& queued) { return isBackground(queued.command.tag); }))
        return;
    for (auto const& run: inside)
    {
        if (isLive(run))
        {
            // The first read waits as long as the next: an agent that just started has spent nothing yet.
            auto const due =
                _statsDueMs.try_emplace(run.id, later(_monoMs, static_cast<uint64_t>(StatsEvery.count())))
                    .first;
            if (due->second > _monoMs)
                continue;
            due->second = later(_monoMs, static_cast<uint64_t>(StatsEvery.count()));
            issueStats(run, out);
            return;
        }
        else if (hadSession(run) && (!run.stats || !run.stats->cost)
                 && !std::ranges::contains(_statsFinalAsked, run.id))
        {
            // Once a run finished its agent has exited, and Claude Code recorded the cost: read it once more.
            _statsFinalAsked.push_back(run.id);
            issueStats(run, out);
            return;
        }
        else if (std::holds_alternative<Finished>(run.state) && run.ledger == Ledger::Pending
                 && !std::ranges::contains(_recordAsked, run.id))
        {
            // Its statistics as complete as they get, the run goes to the file of finished runs, once.
            _recordAsked.push_back(run.id);
            issueRecord(run, out);
            return;
        }
    }
}

std::optional<uint64_t> Engine::nextStatsDue() const
{
    auto next = std::optional<uint64_t> {};
    for (auto const& [run, due]: _statsDueMs)
        if (due > _monoMs)
            next = next ? std::min(*next, due) : due;
    return next;
}

void Engine::issueStats(Run const& run, std::vector<Effect>& out)
{
    // In the worktree root: the run's own worktree may be gone, its log is not.
    issue(
        RunCommand { .tag = std::format("{}{}", run.id, StatsTagSuffix),
                     .command = StatsCommand,
                     .arguments = { Argument { .name = "session", .value = run.session },
                                    Argument { .name = "path", .value = run.worktree },
                                    Argument { .name = "format", .value = std::format("{}", StatsFormat) } },
                     .cwd = _config->worktreeRoot() },
        Priority::Poll,
        out);
}

void Engine::onStatsResult(std::string_view runId, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto* const run = findRun(runId);
    if (run == nullptr || judge(outcome).verdict != Verdict::Ok)
        return;
    // A report that cannot be read leaves the last one standing: the statistics are a record, not a state.
    auto stats = parseStats(outcome.stdoutText);
    if (!stats || run->stats == *stats)
        return;
    run->stats = std::move(*stats);
    ++_revision;
    out.emplace_back(StoreSet { .key = runKey(run->id), .value = toJson(*run) });
}

void Engine::issueRecord(Run const& run, std::vector<Effect>& out)
{
    issue(
        RunCommand { .tag = std::format("{}{}", run.id, RecordTagSuffix),
                     .command = RecordCommand,
                     .arguments = { Argument { .name = "record", .value = recordLine(run) },
                                    Argument { .name = "format", .value = std::format("{}", RecordFormat) } },
                     .cwd = _config->worktreeRoot() },
        Priority::Poll,
        out);
}

void Engine::onRecordResult(std::string_view runId, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto* const run = findRun(runId);
    // A record contour refused is asked for again by the next instance: the file is the user's, not ours.
    if (run == nullptr || judge(outcome).verdict != Verdict::Ok)
        return;
    run->ledger = Ledger::Written;
    out.emplace_back(StoreSet { .key = runKey(run->id), .value = toJson(*run) });
}

} // namespace orchestrator
