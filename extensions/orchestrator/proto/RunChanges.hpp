// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string>
#include <vector>

#include <orchestrator/core/Run.hpp>

namespace orchestrator::proto
{

/// Where one run stood, as the user reads it, so that a later look can say what changed.
struct RunSnapshot
{
    std::string id;                    ///< The run's id.
    std::string state;                 ///< runStateText(): its state, step, and a finished run's outcome.
    std::vector<std::string> moreKept; ///< A kept worktree's reasons after the first, which @c state holds.

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

} // namespace orchestrator::proto
