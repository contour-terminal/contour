// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/HostPaths.hpp>

/// What Engine.cpp, EngineDispatch.cpp and EnginePoll.cpp share; nothing else includes this header.
namespace orchestrator::engine::detail
{

/// What a finished command's outcome amounts to.
enum class Verdict : uint8_t
{
    Ok,         ///< It exited 0 with its whole output.
    Cut,        ///< It exited 0, but its output was cut at the host's limit.
    Failed,     ///< It was refused, timed out, or exited non-zero.
    CwdMissing, ///< It could not start: its working directory is gone.
};

/// A command's outcome, judged.
struct Judgement
{
    Verdict verdict = Verdict::Ok; ///< What it amounts to.
    std::string why;               ///< Why it failed, sanitised, one line; empty when it did not.
};

/// Judges a command's outcome.
/// @param outcome What became of the command.
/// @return Its verdict and, for a failure, the reason.
[[nodiscard]] Judgement judge(ProcessOutcome const& outcome);

/// The time from @p then to @p now, which never runs backwards.
/// @param now The later time, in milliseconds.
/// @param then The earlier time, in milliseconds.
/// @return `now - then`, or 0 when @p then is later.
[[nodiscard]] constexpr uint64_t elapsed(uint64_t now, uint64_t then) noexcept
{
    return now >= then ? now - then : 0;
}

/// @param base A time, in milliseconds.
/// @param step A duration, in milliseconds.
/// @return `base + step`, saturating instead of wrapping.
[[nodiscard]] constexpr uint64_t later(uint64_t base, uint64_t step) noexcept
{
    return base > UINT64_MAX - step ? UINT64_MAX : base + step;
}

/// The time to the next poll.
/// @param pollEvery The configured interval, in milliseconds.
/// @param failures The polls in a row that failed.
/// @return @p pollEvery doubled once per failure up to MaxPollBackoff, never below @p pollEvery.
[[nodiscard]] constexpr uint64_t pollInterval(uint64_t pollEvery, uint32_t failures) noexcept
{
    auto const most = pollEvery > static_cast<uint64_t>(MaxPollBackoff.count())
                          ? pollEvery
                          : static_cast<uint64_t>(MaxPollBackoff.count());
    auto interval = pollEvery;
    for ([[maybe_unused]] auto const failure: std::views::iota(uint32_t { 0 }, failures))
    {
        if (interval >= most)
            break;
        interval = interval > most / 2 ? most : interval * 2;
    }
    return interval;
}

/// A file directly inside a run's worktree, such as its `.git` or `STATUS.md`. The separator is the one the
/// worktree's own spelling names: a stored run is read before, or without, a configuration.
/// @param run The run.
/// @param name The file's name.
/// @return Its path, such as `/home/u/wt/contour/<id>/STATUS.md` or `C:\wt\contour\<id>\STATUS.md`.
[[nodiscard]] inline std::string inWorktree(Run const& run, std::string_view name)
{
    return joinPath(run.worktree, name, pathStyleOf(run.worktree).value_or(PathStyle::Posix));
}

/// The suffix of the tag a status board is read with until one is found (the threshold read).
inline constexpr std::string_view BoardTagSuffix = "/board";

/// The suffix of the tag a status board a tab shows is read with: `<run>#board-view`.
inline constexpr std::string_view BoardViewTagSuffix = "#board-view";

/// The suffix of the tag a run's transcript page is read with: `<run>#transcript`.
inline constexpr std::string_view TranscriptTagSuffix = "#transcript";

/// The suffix of the tag a run's statistics are read with: `<run>#stats`.
inline constexpr std::string_view StatsTagSuffix = "#stats";

/// The suffix of the tag a finished run is appended to the file of finished runs with: `<run>#record`.
inline constexpr std::string_view RecordTagSuffix = "#record";

/// What every poll tag starts with.
inline constexpr std::string_view PollTagPrefix = "poll/";

} // namespace orchestrator::engine::detail
