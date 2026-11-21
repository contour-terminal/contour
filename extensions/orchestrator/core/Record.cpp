// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Record.hpp>

#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Overloaded.hpp>

namespace orchestrator::record::detail
{

/// Format 1 of a finished run's line, its keys in their contract order. Glaze reflects only types with
/// linkage.
struct JsonRecord
{
    uint32_t format = RecordFormat;
    std::string run;
    std::string repo;
    std::string item;
    std::string url;
    std::string task;
    std::string model;
    std::string session;
    uint64_t dispatchedMs = 0;
    uint64_t endedMs = 0;
    std::string ended;
    std::string worktree;
    std::optional<uint64_t> wallMs;
    uint64_t waitingMs = 0;
    std::vector<StateChange> history;
    std::optional<RunStats> stats;
};

} // namespace orchestrator::record::detail

namespace orchestrator
{

namespace
{
    /// Format 1 writes what is not known as null, never by leaving the key out.
    constexpr auto WriteOptions = glz::opts { .skip_null_members = false };

    [[nodiscard]] std::string itemText(ItemRef const& item)
    {
        return std::visit(Overloaded {
                              [](ChangeRequestRef const& ref) { return std::format("pr#{}", ref.number); },
                              [](IssueRef const& ref) { return std::format("issue#{}", ref.number); },
                              [](JiraRef const& ref) { return std::format("jira:{}", ref.key); },
                          },
                          item);
    }

    [[nodiscard]] std::string written(record::detail::JsonRecord const& record)
    {
        auto line = std::string {};
        if (glz::write<WriteOptions>(record, line))
            return {};
        return line;
    }
} // namespace

std::string recordLine(Run const& run)
{
    auto const* const finished = std::get_if<Finished>(&run.state);
    auto const times = timesOf(run.history);
    auto record = record::detail::JsonRecord {
        .format = RecordFormat,
        .run = run.id,
        .repo = run.repo,
        .item = itemText(run.item),
        .url = run.url,
        .task = run.task,
        .model = run.model,
        .session = run.session,
        .dispatchedMs = run.startedWallMs,
        .endedMs = finished != nullptr ? finished->endedWallMs : 0,
        .ended = finished != nullptr ? std::string { endedName(finished->ended) } : std::string {},
        .worktree = finished != nullptr && finished->fate == Fate::Kept ? "kept" : "removed",
        .wallMs = times.wallMs,
        .waitingMs = times.waitingMs,
        .history = run.history,
        .stats = run.stats,
    };
    // What does not fit a declared command's argument is left out, the least telling first.
    auto line = written(record);
    if (line.size() > MaxTextBytes)
    {
        record.history.clear();
        line = written(record);
    }
    if (line.size() > MaxTextBytes && record.stats)
    {
        record.stats->tools.clear();
        record.stats->pullRequests.clear();
        line = written(record);
    }
    if (line.size() > MaxTextBytes)
    {
        record.stats.reset();
        line = written(record);
    }
    return line;
}

} // namespace orchestrator
