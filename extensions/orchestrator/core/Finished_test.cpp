// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Utf8.hpp>

using namespace std::string_view_literals;

namespace
{

using orchestrator::Blocked;
using orchestrator::Counts;
using orchestrator::Done;
using orchestrator::Ended;
using orchestrator::Ending;
using orchestrator::EndStep;
using orchestrator::Fate;
using orchestrator::Finished;
using orchestrator::PrepareStep;
using orchestrator::Preparing;
using orchestrator::Run;
using orchestrator::RunFailure;
using orchestrator::Running;
using orchestrator::RunState;
using orchestrator::Seen;

/// U+202E RIGHT-TO-LEFT OVERRIDE, spelled byte by byte.
constexpr auto RightToLeftOverrideBytes = std::array { '\xe2', '\x80', '\xae' };
constexpr auto RightToLeftOverride =
    std::string_view { RightToLeftOverrideBytes.data(), RightToLeftOverrideBytes.size() };

constexpr auto MsPerDay = uint64_t { 24 } * 60 * 60 * 1000;
constexpr auto NowMs = uint64_t { 1'794'580'000'000 };
constexpr auto Retention30d = std::chrono::milliseconds { 30 * MsPerDay };

/// The states a row's run is in.
enum class Kind : uint8_t
{
    Preparing,
    Running,
    Blocked,
    Done,
    Ending,
    EndingResumedNew,
    EndingResumedOpened,
    RemovedNew,
    RemovedOpened,
    KeptNew,
    KeptOpened,
};

[[nodiscard]] Finished finished(Fate fate, Seen seen, uint64_t endedWallMs)
{
    auto entries = std::vector<std::string> {};
    if (fate == Fate::Kept)
        entries.emplace_back("?? notes.txt");
    return Finished { .ended = Ended::Completed,
                      .fate = fate,
                      .entries = std::move(entries),
                      .seen = seen,
                      .endedWallMs = endedWallMs };
}

[[nodiscard]] RunState stateOf(Kind kind, uint64_t endedWallMs)
{
    switch (kind)
    {
        case Kind::Preparing: return Preparing { .step = PrepareStep::WorktreeAdd };
        case Kind::Running: return Running {};
        case Kind::Blocked: return Blocked {};
        case Kind::Done: return Done {};
        case Kind::Ending:
            return Ending { .step = EndStep::Status,
                            .cause = Ended::EndedByYou,
                            .resumedFrom = std::nullopt,
                            .removal = orchestrator::Removal::NotIssued };
        case Kind::EndingResumedNew:
            return Ending { .step = EndStep::Verify,
                            .cause = Ended::Completed,
                            .resumedFrom = finished(Fate::Kept, Seen::New, endedWallMs),
                            .removal = orchestrator::Removal::NotIssued };
        case Kind::EndingResumedOpened:
            return Ending { .step = EndStep::Verify,
                            .cause = Ended::Completed,
                            .resumedFrom = finished(Fate::Kept, Seen::Opened, endedWallMs),
                            .removal = orchestrator::Removal::NotIssued };
        case Kind::RemovedNew: return finished(Fate::Removed, Seen::New, endedWallMs);
        case Kind::RemovedOpened: return finished(Fate::Removed, Seen::Opened, endedWallMs);
        case Kind::KeptNew: return finished(Fate::Kept, Seen::New, endedWallMs);
        case Kind::KeptOpened: return finished(Fate::Kept, Seen::Opened, endedWallMs);
    }
    return Running {};
}

[[nodiscard]] Run makeRun(std::string id, RunState state)
{
    return Run {
        .id = std::move(id),
        .repo = "contour",
        .repoPath = "/home/u/projects/contour",
        .forge = orchestrator::Forge::GitHub,
        .remote = "origin",
        .base = "master",
        .item = orchestrator::ChangeRequestRef { .number = 2334 },
        .url = "https://github.com/contour-terminal/contour/pull/2334",
        .task = "review",
        .model = "opus",
        .prompt = "Review https://github.com/contour-terminal/contour/pull/2334",
        .session = "123e4567-e89b-12d3-a456-426614174000",
        .branch = "agent/pr-2334-20261113-150847",
        .worktree = "/home/u/worktrees/contour/pr-2334-20261113-150847",
        .pane = std::nullopt,
        .state = std::move(state),
        .lastStatus = std::nullopt,
        .failure = std::nullopt,
        .startedWallMs = NowMs - (40 * MsPerDay),
        .runningSinceMs = 0,
    };
}

[[nodiscard]] Run makeRun(Kind kind, uint64_t endedWallMs = NowMs)
{
    return makeRun("pr-2334-20261113-150847", stateOf(kind, endedWallMs));
}

/// Whether @p text is well-formed UTF-8.
[[nodiscard]] bool isWellFormed(std::string_view text)
{
    for (auto const decoded: orchestrator::Codepoints { text })
    {
        if (decoded.codepoint == orchestrator::ReplacementCharacter)
            return false;
    }
    return true;
}

struct ShortTextRow
{
    Counts counts;
    std::string_view expected;
};

constexpr auto ShortTextRows = std::array {
    ShortTextRow { .counts = { 3, 4, 1, 1, 2 }, .expected = "⬢ agents 3/4 · ✗1 · ●1 blocked · ✓2 new" },
    ShortTextRow { .counts = { 0, 4, 0, 0, 0 }, .expected = "⬢ agents 0/4" },
    ShortTextRow { .counts = { 4, 4, 0, 2, 0 }, .expected = "⬢ agents 4/4 · ●2 blocked" },
    ShortTextRow { .counts = { 1, 4, 3, 0, 0 }, .expected = "⬢ agents 1/4 · ✗3" },
    ShortTextRow { .counts = { 0, 2, 0, 0, 7 }, .expected = "⬢ agents 0/2 · ✓7 new" },
};

/// Whether a row expects its run to be purged.
enum class Purge : uint8_t
{
    Keep,
    Purge,
};

struct PurgeRow
{
    Kind kind;
    uint64_t endedWallMs;
    uint64_t nowWallMs;
    std::chrono::milliseconds retention;
    Purge expected;
};

/// The end time of a run that ended @p days before NowMs.
[[nodiscard]] constexpr uint64_t daysAgo(uint64_t days) noexcept
{
    return NowMs - (days * MsPerDay);
}

constexpr auto MaxRetention = std::chrono::milliseconds { INT64_MAX };

constexpr auto PurgeRows = std::array {
    PurgeRow { Kind::RemovedOpened, daysAgo(31), NowMs, Retention30d, Purge::Purge },
    PurgeRow { Kind::RemovedNew, daysAgo(31), NowMs, Retention30d, Purge::Purge },
    PurgeRow { Kind::RemovedOpened, daysAgo(29), NowMs, Retention30d, Purge::Keep },
    // "Older than" the retention: exactly as old is kept.
    PurgeRow { Kind::RemovedOpened, daysAgo(30), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::KeptOpened, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::KeptNew, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::EndingResumedOpened, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::Ending, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::Running, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::Preparing, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::Blocked, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    PurgeRow { Kind::Done, daysAgo(90), NowMs, Retention30d, Purge::Keep },
    // `finished_retention: 0` never purges, and neither does a negative retention.
    PurgeRow { Kind::RemovedOpened, daysAgo(3650), NowMs, std::chrono::milliseconds { 0 }, Purge::Keep },
    PurgeRow { Kind::RemovedOpened, daysAgo(3650), NowMs, std::chrono::milliseconds { -1 }, Purge::Keep },
    // An end time after now is not purgeable.
    PurgeRow { Kind::RemovedOpened, NowMs + MsPerDay, NowMs, Retention30d, Purge::Keep },
    // The extremes of the clock and of the retention.
    PurgeRow { Kind::RemovedOpened, 0, UINT64_MAX, Retention30d, Purge::Purge },
    PurgeRow { Kind::RemovedOpened, 0, UINT64_MAX, MaxRetention, Purge::Purge },
    PurgeRow { Kind::RemovedOpened, 0, uint64_t { INT64_MAX }, MaxRetention, Purge::Keep },
    PurgeRow { Kind::RemovedOpened, 0, NowMs, MaxRetention, Purge::Keep },
    PurgeRow { Kind::RemovedOpened, UINT64_MAX, UINT64_MAX, std::chrono::milliseconds { 1 }, Purge::Keep },
};

struct KindRow
{
    Kind kind;
    std::size_t newFinished; ///< What newFinishedCount() counts it as.
    uint32_t active;         ///< What countRuns() counts it as active.
    uint32_t blocked;        ///< What countRuns() counts it as blocked.
};

constexpr auto KindRows = std::array {
    KindRow { .kind = Kind::Preparing, .newFinished = 0, .active = 1, .blocked = 0 },
    KindRow { .kind = Kind::Running, .newFinished = 0, .active = 1, .blocked = 0 },
    KindRow { .kind = Kind::Blocked, .newFinished = 0, .active = 1, .blocked = 1 },
    KindRow { .kind = Kind::Done, .newFinished = 0, .active = 1, .blocked = 0 },
    // Ending is unfinished but not active: it takes no slot.
    KindRow { .kind = Kind::Ending, .newFinished = 0, .active = 0, .blocked = 0 },
    // A kept run checked again keeps its mark: still new, or still seen.
    KindRow { .kind = Kind::EndingResumedNew, .newFinished = 1, .active = 0, .blocked = 0 },
    KindRow { .kind = Kind::EndingResumedOpened, .newFinished = 0, .active = 0, .blocked = 0 },
    KindRow { .kind = Kind::RemovedNew, .newFinished = 1, .active = 0, .blocked = 0 },
    KindRow { .kind = Kind::RemovedOpened, .newFinished = 0, .active = 0, .blocked = 0 },
    KindRow { .kind = Kind::KeptNew, .newFinished = 1, .active = 0, .blocked = 0 },
    KindRow { .kind = Kind::KeptOpened, .newFinished = 0, .active = 0, .blocked = 0 },
};

struct LineRow
{
    Kind kind;
    std::string_view expected;
};

constexpr auto LineRows = std::array {
    LineRow { .kind = Kind::Preparing,
              .expected = "pr-2334-20261113-150847 · contour · opus · preparing: worktree" },
    LineRow { .kind = Kind::Running, .expected = "pr-2334-20261113-150847 · contour · opus · running" },
    LineRow { .kind = Kind::Blocked, .expected = "pr-2334-20261113-150847 · contour · opus · blocked" },
    LineRow { .kind = Kind::Done, .expected = "pr-2334-20261113-150847 · contour · opus · done" },
    LineRow { .kind = Kind::Ending, .expected = "pr-2334-20261113-150847 · contour · opus · ending: status" },
    LineRow { .kind = Kind::EndingResumedNew,
              .expected =
                  "pr-2334-20261113-150847 · contour · opus · completed, worktree kept, new, checking "
                  "again: ?? notes.txt" },
    LineRow { .kind = Kind::RemovedNew,
              .expected = "pr-2334-20261113-150847 · contour · opus · completed, worktree removed, new" },
    LineRow { .kind = Kind::RemovedOpened,
              .expected = "pr-2334-20261113-150847 · contour · opus · completed, worktree removed, seen" },
    LineRow { .kind = Kind::KeptOpened,
              .expected =
                  "pr-2334-20261113-150847 · contour · opus · completed, worktree kept, seen: ?? notes.txt" },
};

} // namespace

TEST_CASE("Finished list and summary", "[orchestrator][finished]")
{
    SECTION("short text")
    {
        for (auto const& row: ShortTextRows)
        {
            CAPTURE(row.counts.active,
                    row.counts.max,
                    row.counts.failingCi,
                    row.counts.blocked,
                    row.counts.newFinished);
            CHECK(orchestrator::summarize(row.counts, {}).shortText == row.expected);
        }
    }

    SECTION("isPurgeable")
    {
        for (auto const& row: PurgeRows)
        {
            CAPTURE(row.kind, row.endedWallMs, row.nowWallMs, row.retention.count(), row.expected);
            auto const run = makeRun(row.kind, row.endedWallMs);
            auto const purge =
                orchestrator::isPurgeable(run, row.nowWallMs, row.retention) ? Purge::Purge : Purge::Keep;
            CHECK(purge == row.expected);
        }
    }

    SECTION("newFinishedCount and countRuns, per state")
    {
        for (auto const& row: KindRows)
        {
            CAPTURE(row.kind, row.newFinished, row.active, row.blocked);
            auto const runs = std::array { makeRun(row.kind) };
            CHECK(orchestrator::newFinishedCount(runs) == row.newFinished);
            auto const counts = orchestrator::countRuns(runs, 4, 1);
            CHECK(counts.active == row.active);
            CHECK(counts.blocked == row.blocked);
            CHECK(counts.newFinished == row.newFinished);
            CHECK(counts.max == 4);
            CHECK(counts.failingCi == 1);
        }
    }

    SECTION("newFinishedCount and countRuns, every state at once")
    {
        auto runs = std::vector<Run> {};
        for (auto const& row: KindRows)
            runs.push_back(makeRun(row.kind));
        CHECK(orchestrator::newFinishedCount(runs) == 3);
        auto const counts = orchestrator::countRuns(runs, 4, 0);
        CHECK(counts.active == 4);
        CHECK(counts.blocked == 1);
        CHECK(counts.newFinished == 3);
    }

    SECTION("detail lines")
    {
        for (auto const& row: LineRows)
        {
            CAPTURE(row.kind, row.expected);
            auto const runs = std::array { makeRun(row.kind) };
            auto const summary = orchestrator::summarize(Counts { 0, 4, 0, 0, 0 }, runs);
            CHECK(summary.detail == std::format("⬢ agents 0/4\n{}", row.expected));
        }
    }

    SECTION("a failed preparation says why")
    {
        auto run = makeRun("pr-1-20261113-150847",
                           Finished { .ended = Ended::PreparationFailed,
                                      .fate = Fate::Removed,
                                      .entries = {},
                                      .seen = Seen::New,
                                      .endedWallMs = NowMs });
        run.failure = RunFailure { .step = PrepareStep::Fetch, .why = "fatal: no remote" };
        auto const runs = std::array { std::move(run) };
        CHECK(
            orchestrator::summarize(Counts { 0, 4, 0, 0, 1 }, runs).detail
            == "⬢ agents 0/4 · ✓1 new\npr-1-20261113-150847 · contour · opus · preparation failed, worktree "
               "removed, new: fetch: fatal: no remote");
    }

    SECTION("hostile text is sanitised")
    {
        auto finishedState = finished(Fate::Kept, Seen::New, NowMs);
        finishedState.entries = { std::format("\x1b]8;;evil\x1b\\M src/{}ü.cpp\nnext", RightToLeftOverride) };
        auto const runs = std::array { makeRun("pr-1-20261113-150847", std::move(finishedState)) };
        auto const detail = orchestrator::summarize(Counts { 0, 4, 0, 0, 1 }, runs).detail;
        CAPTURE(detail);
        CHECK_FALSE(detail.contains('\x1b'));
        CHECK_FALSE(detail.contains(RightToLeftOverride));
        CHECK(std::ranges::count(detail, '\n') == 1);
        CHECK(isWellFormed(detail));
    }

    SECTION("a thousand runs stay within the bound")
    {
        auto runs = std::vector<Run> {};
        for (auto const index: std::views::iota(0, 1000))
        {
            auto state = finished(Fate::Kept, Seen::New, NowMs);
            // Each line is cut at MaxSummaryLineChars characters, many of them two or three bytes long.
            state.entries = { std::format("\x1b[31m M {}", std::string(300, 'x')) + "üü€€€\x1b" };
            state.entries.front().insert(std::size_t { 10 },
                                         std::string(static_cast<std::size_t>(index % 7), '\xc3'));
            runs.push_back(makeRun(std::format("pr-{}-20261113-150847", index), std::move(state)));
        }
        auto const detail = orchestrator::summarize(Counts { 0, 4, 0, 0, 1000 }, runs).detail;
        CHECK(detail.size() <= orchestrator::MaxSummaryDetailBytes);
        CHECK_FALSE(detail.contains('\x1b'));
        CHECK(isWellFormed(detail));
        auto const lastLine = std::string_view { detail }.substr(detail.rfind('\n') + 1);
        CAPTURE(lastLine);
        CHECK(lastLine.starts_with("… "));
        CHECK(lastLine.ends_with(" more"));
        auto const listed = static_cast<std::size_t>(std::ranges::count(detail, '\n')) - 1;
        CHECK(lastLine == std::format("… {} more", 1000 - listed));
    }
}
