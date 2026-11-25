// SPDX-License-Identifier: Apache-2.0
//
// The Fleet: the runs, grouped by what they need from the user, and how each one looks in its row.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/core/Run.hpp>
#include <orchestrator/view/BoardMarkdown.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

/// What the Focus shows of the selected run.
enum class FocusTab : uint8_t
{
    Board = 0,  ///< Its status board.
    Transcript, ///< Its transcript, following the agent while it runs.
    Details,    ///< Its details: where it works, what it spent, its permissions.
};

/// Whether something changed since the user last looked at it (SeenLedger).
enum class News : uint8_t
{
    None = 0, ///< Nothing changed, or the user looks at it.
    Unseen,   ///< It changed since: a `•` in the accent colour marks it.
};

/// The Fleet's groups, in the order they are listed.
enum class FleetGroup : uint8_t
{
    NeedsYou = 0, ///< Asking, waiting for input, or stopped by an error.
    Working,      ///< Working, or starting (its tab is open, no report yet), or ending.
    ToReview,     ///< Its agent reported done, and the run is still open.
    Finished,     ///< Every recorded run not dismissed.
};

/// Whether a group lists all of its runs by default.
enum class FoldDefault : uint8_t
{
    Open = 0, ///< It lists every run until the user folds it.
    Folded,   ///< It lists FinishedFoldedTo runs until the user unfolds it.
};

/// One group of the Fleet.
struct FleetGroupRow
{
    FleetGroup group = FleetGroup::NeedsYou;     ///< The group.
    std::string_view key = {};                   ///< What the engine keeps its fold under.
    std::string_view name = {};                  ///< Its heading.
    Role role = Role::Text;                      ///< How its heading is drawn.
    FocusTab opensOn = FocusTab::Board;          ///< The Focus tab a run of it opens on.
    FoldDefault foldDefault = FoldDefault::Open; ///< Whether it lists all of its runs by default.
};

/// How many runs a folded Finished group lists: its newest.
inline constexpr std::size_t FinishedFoldedTo = 5;

/// @return The groups, in the order they are listed, indexed by FleetGroup.
[[nodiscard]] std::span<FleetGroupRow const> fleetGroups() noexcept;

/// @param group A group.
/// @return Its row of fleetGroups().
[[nodiscard]] FleetGroupRow const& fleetGroupRow(FleetGroup group) noexcept;

/// @param run A run.
/// @return The group it is listed in: blocked, idle and error in Needs you; no report yet, working and ending
///         in Working; done in To review; finished in Finished.
[[nodiscard]] FleetGroup fleetGroupOf(Run const& run) noexcept;

/// How a run looks in its row and the Focus header: a glyph paired with a colour and a word, so that nothing
/// is told apart by hue alone.
struct RunLook
{
    std::string_view mark = {}; ///< One cell: `?`, `…`, `✗`, `⠋` (a spinner's frame), `◌`, `✓`, `·` or `⚑`.
    Role role = Role::Text;     ///< How the mark and the word are drawn.
    std::string_view word = {}; ///< Such as `asking`, `working` or `kept`.
};

/// @param run A run.
/// @return How it looks.
[[nodiscard]] RunLook runLookOf(Run const& run) noexcept;

/// What a row of the Fleet is, as far as the actions it offers go (actionsFor()).
enum class RowState : uint8_t
{
    None = 0,    ///< Nothing to act on: a notice, or no row.
    Asking,      ///< Its agent asks for a permission.
    Waiting,     ///< Its agent waits for input: its turn ended without reporting done.
    Errored,     ///< Its agent stopped by an error.
    Starting,    ///< Being prepared, or its agent has not reported yet.
    Working,     ///< Its agent works.
    Done,        ///< Its agent reported done, and the run is still open.
    Ending,      ///< Being ended: its worktree is checked and removed.
    Finished,    ///< Over, its worktree removed or gone.
    Kept,        ///< Over, its worktree kept: what kept it is listed.
    NewTask,     ///< `+ new task`.
    GroupHeader, ///< A group's heading.
    Leftover,    ///< A worktree no run accounts for.
};

/// @param run A run.
/// @return Its state: from what it is, and what its agent last reported.
[[nodiscard]] RowState rowStateOf(Run const& run) noexcept;

/// @param run A run.
/// @return Since when it waited in its group, in wall-clock milliseconds: when it entered its state, as its
///         history says, else when it was dispatched. The Fleet lists the longest waiting first.
[[nodiscard]] uint64_t waitingSinceOf(Run const& run) noexcept;

/// What a board says of its agent's progress, from its task boxes.
struct BoardProgress
{
    std::size_t done = 0;  ///< The boxes ticked (`☑`).
    std::size_t total = 0; ///< Every box.
    std::string current;   ///< The first open box's text, without its box; empty: none is open.
    [[nodiscard]] bool operator==(BoardProgress const&) const = default;
};

/// @param lines A board's lines, rendered (renderBoardMarkdown()).
/// @return What its task boxes say; std::nullopt when it has none.
[[nodiscard]] std::optional<BoardProgress> boardProgressOf(std::span<BoardLine const> lines);

/// @param progress A board's progress.
/// @param cells The cells the meter takes.
/// @return A meter of @p cells: `▰` for the share done, `▱` for the rest.
[[nodiscard]] std::string meterOf(BoardProgress const& progress, int cells);

} // namespace orchestrator::view
