// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Run.hpp>

namespace orchestrator::proto
{

/// Whether a run's kept worktree is being checked again.
enum class Rechecking : uint8_t
{
    No,  ///< It is not.
    Yes, ///< It is: the run is ending, and is the kept entry again once the check is done.
};

/// Where one run stood, as the user reads it, so that a later look can say what changed.
struct RunSnapshot
{
    std::string id;                    ///< The run's id.
    std::string state;                 ///< runStateText(): its state, step, and a finished run's outcome.
    std::vector<std::string> moreKept; ///< A kept worktree's reasons after the first, which @c state holds.
    Rechecking rechecking = Rechecking::No; ///< Whether its kept worktree is being checked again.

    [[nodiscard]] bool operator==(RunSnapshot const&) const = default;
};

/// Where each run stands.
/// @param runs The engine's runs.
/// @return One snapshot per run, in the runs' order.
[[nodiscard]] std::vector<RunSnapshot> snapshotRuns(std::span<Run const> runs);

/// What changed between two looks, one line per change: `<id>: <old> → <new>`, then the kept worktree's
/// further reasons indented beneath it. A run that appeared reads `<id>: dispatched → <new>`, one that went
/// `<id>: <old> → off the list`.
/// @param before The earlier look.
/// @param after The later look.
/// @return The lines, in the order of @p after, then the runs gone; empty when nothing changed.
[[nodiscard]] std::vector<std::string> changeLines(std::span<RunSnapshot const> before,
                                                   std::span<RunSnapshot const> after);

/// Who asked for a kept worktree to be checked again.
enum class Asker : uint8_t
{
    Poll, ///< A poll: only an outcome that differs is said.
    User, ///< `retry`: the outcome is said even when it is the same, so that the user sees the answer.
};

/// Says what changed, look after look. A kept worktree checked again is said by its outcome, never step by
/// step: the steps of a check that ends where it started are no news.
class ChangeReport
{
  public:
    /// The user asked for @p runId's kept worktree to be checked again; the next look says its outcome.
    /// @param runId The run.
    void requested(std::string runId);

    /// What changed between two looks, as changeLines() says it, but a re-check only by its outcome.
    /// @param before The earlier look.
    /// @param after The later look.
    /// @return The lines; empty when nothing worth saying changed.
    [[nodiscard]] std::vector<std::string> lines(std::span<RunSnapshot const> before,
                                                 std::span<RunSnapshot const> after);

  private:
    /// A re-check under way: where the run stood before it, and who asked for it.
    struct Recheck
    {
        RunSnapshot from;
        Asker asker = Asker::Poll;
    };

    [[nodiscard]] Asker takeRequest(std::string_view runId);

    std::vector<std::string> _requested; ///< The runs `retry` was given, whose outcome is not said yet.
    std::vector<Recheck> _rechecks;      ///< The re-checks under way.
};

} // namespace orchestrator::proto
