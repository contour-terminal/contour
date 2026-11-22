// SPDX-License-Identifier: Apache-2.0
//
// Attention: what tells the user where to look. Which runs entered Needs you, and what changed since the user
// last looked at a run or a Focus tab.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Sections.hpp>

namespace orchestrator::view
{

/// What a run is, as far as the Fleet's news goes: compared with what it was when the user last looked.
struct RunMarks
{
    RowState state = RowState::None; ///< Its row state.
    uint64_t changedWallMs = 0;      ///< When its state last changed (waitingSinceOf()).
    std::string log = {};            ///< Its transcript's last timestamp, as its statistics last said; empty:
                                     ///< not read yet.
    [[nodiscard]] bool operator==(RunMarks const&) const = default;
};

/// What the selected run's Focus tabs show, as far as their news goes. 0 is not known yet: never news, and
/// never what was seen.
struct TabMarks
{
    uint64_t board = 0;      ///< Its board's modification time, in wall-clock milliseconds.
    uint64_t transcript = 0; ///< Its transcript's size: its pages, and the entries of its last page.
};

/// @param run A run.
/// @return What its row in the Fleet compares.
[[nodiscard]] RunMarks runMarksOf(Run const& run);

/// @param engine The engine.
/// @param tab The dashboard's tab, whose transcript page the engine keeps.
/// @param run The run's id.
/// @return What its tabs show now: the board a tab shows, the transcript page @p tab last read when it is
///         the last one.
[[nodiscard]] TabMarks tabMarksOf(Engine const& engine, std::string_view tab, std::string_view run);

/// What the user has seen of each run, as one dashboard showed it. Kept in memory: a run first met is seen
/// as it is, so a dashboard that opens marks nothing.
class SeenLedger
{
  public:
    /// The user looks at a run, on @p shown: its marks are seen, and so is the shown tab's content. A tab not
    /// shown is seen only the first time its content is known.
    /// @param run The run's id.
    /// @param marks Its marks now.
    /// @param tabs Its tabs' marks now.
    /// @param shown The Focus tab shown.
    void look(std::string_view run, RunMarks const& marks, TabMarks const& tabs, FocusTab shown);

    /// @param run The run's id.
    /// @param marks Its marks now.
    /// @return Whether it changed since the user last looked at it; a run first met is seen as it is.
    [[nodiscard]] News newsOf(std::string_view run, RunMarks const& marks);

    /// @param run The run's id.
    /// @param tabs Its tabs' marks now.
    /// @param tab A Focus tab.
    /// @return Whether @p tab's content changed since it was last shown; Details never has news.
    [[nodiscard]] News tabNewsOf(std::string_view run, TabMarks const& tabs, FocusTab tab) const;

  private:
    /// What the user saw of a run.
    struct Seen
    {
        RunMarks run;  ///< When it was last looked at.
        TabMarks tabs; ///< Each tab's when it was last shown.
    };

    std::map<std::string, Seen, std::less<>> _seen; ///< By run id.
};

/// @param rows The Fleet's rows.
/// @return The runs listed in Needs you, sorted.
[[nodiscard]] std::vector<std::string> needingRuns(std::span<Row const> rows);

/// @param before The runs in Needs you before, sorted.
/// @param now The runs in Needs you now, sorted.
/// @return The runs that entered it: in @p now, not in @p before.
[[nodiscard]] std::vector<std::string> needsYouEntries(std::span<std::string const> before,
                                                       std::span<std::string const> now);

} // namespace orchestrator::view
