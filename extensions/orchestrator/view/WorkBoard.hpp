// SPDX-License-Identifier: Apache-2.0
//
// The Work view: the user's merge requests and tickets, in the groups core/WorkItems puts them in, folding,
// sortable and filtered; its keys, and the component that draws it.
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Component.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/Keys.hpp>

namespace orchestrator::view
{

/// What a line of the Work view is.
enum class WorkLineKind : uint8_t
{
    Heading = 0, ///< A group's heading: `▾ Needs you  3`.
    Item,        ///< An item's row.
};

/// One line of the Work view.
struct WorkLine
{
    WorkLineKind kind = WorkLineKind::Heading;   ///< What it is.
    WorkGroup group = WorkGroup::EverythingElse; ///< Its group: the heading's, or the item's.
    std::size_t item = 0;                        ///< An item line's index into the items; 0 for a heading.
    [[nodiscard]] bool operator==(WorkLine const&) const = default;
};

/// The engine's wall clock, and when the board saw it.
struct WallSample
{
    uint64_t wallMs = 0; ///< Engine::wallMs() as it was seen.
    uint64_t monoMs = 0; ///< When, in monotonic milliseconds.
};

/// What the Work view draws from, kept by the dashboard across frames: the items are built again only when
/// the engine's revision changed, and the lines whenever the interaction or the folds may have moved them.
struct WorkBoardData
{
    std::vector<WorkItem> items = {};                ///< Engine::workItems().
    std::vector<std::size_t> order = {};             ///< orderOf() the items, by the engine's sort.
    std::vector<WorkLine> lines = {};                ///< workLines() of them.
    std::optional<uint64_t> revision = std::nullopt; ///< The engine's revision they were built at.
    std::size_t listRows = 0;                        ///< How many lines the list showed, as last drawn.
    /// The engine's wall clock as the board last saw it change, and when, in monotonic milliseconds: the ages
    /// are measured against it moved on by the time since, so that they move on between the engine's wakes.
    std::optional<WallSample> wall = std::nullopt;
    /// When an age the last frame drew changes next, in monotonic milliseconds; std::nullopt: none does.
    std::optional<uint64_t> ageChangeMs = std::nullopt;
};

/// @param engine The engine: the fold keys it holds.
/// @param group A group.
/// @return Whether @p group is folded: its default, flipped when the engine holds its fold key.
[[nodiscard]] bool isGroupFolded(Engine const& engine, WorkGroup group) noexcept;

/// @param engine The engine: the fold keys it holds.
/// @return The sort the engine's fold keys hold: LastUpdate while it holds SortByUpdateKey.
[[nodiscard]] WorkSort sortOf(Engine const& engine) noexcept;

/// @param items The items.
/// @param order Indices into @p items, ordered by orderOf().
/// @param engine The engine: its folds, its sort and its repositories (for the repository chip).
/// @param state The filter and the repository chip.
/// @return The lines drawn: Importance → per non-empty group a heading then its items unless folded;
///         LastUpdate → items only. Items the filter or the repository chip hide are left out, and a group
///         left empty by them loses its heading.
[[nodiscard]] std::vector<WorkLine> workLines(std::span<WorkItem const> items,
                                              std::span<std::size_t const> order,
                                              Engine const& engine,
                                              WorkState const& state);

/// @param line A line.
/// @param items The items it indexes.
/// @return Its identity across rebuilds: workItemKey() of an item, `group:<foldKey>` of a heading.
[[nodiscard]] std::string workLineKey(WorkLine const& line, std::span<WorkItem const> items);

/// Keeps the selection on its line across a rebuild of the lines, by WorkState::selectedKey; when that line
/// is gone, on the nearest one; with nothing chosen yet, on the first item. Then scrolls it into view.
/// @param state The Work view's interaction.
/// @param lines The lines, as built now.
/// @param items The items they index.
/// @param pageRows How many lines the list shows; 0: not known yet, the scroll is left alone.
void keepWorkSelection(WorkState& state,
                       std::span<WorkLine const> lines,
                       std::span<WorkItem const> items,
                       std::size_t pageRows);

/// @param engine The engine: the forge of the item's repository.
/// @param item An item.
/// @return An MR row's number as its forge spells it, `!812` or `#2375`; empty for a ticket row.
[[nodiscard]] std::string workMrLabel(Engine const& engine, WorkItem const& item);

/// Folds or unfolds @p group, and selects its heading once the lines are built again.
/// @param state The Work view's interaction: its selection moves to the heading.
/// @param group The group.
/// @return The ToggleFoldAction of the group's fold key.
[[nodiscard]] UiAction foldWorkGroup(WorkState& state, WorkGroup group);

/// Selects a line, as a click on it does, and scrolls it into view.
/// @param state The Work view's interaction.
/// @param data The lines as drawn, and how many the list shows.
/// @param index The line, into @p data's lines; kept within them.
void selectWorkLine(WorkState& state, WorkBoardData const& data, std::size_t index);

/// Scrolls the list, as the wheel does: within its ends; the selection stays on its line while that line is
/// shown, else moves to the nearest line shown.
/// @param state The Work view's interaction.
/// @param data The lines as drawn, and how many the list shows.
/// @param lines How far: negative is up.
void scrollWorkList(WorkState& state, WorkBoardData const& data, std::ptrdiff_t lines);

/// Scrolls the list to where a click or a drag on the scrollbar points: its top cell is the list's start, its
/// bottom cell its end. The selection is kept shown as scrollWorkList() keeps it.
/// @param state The Work view's interaction.
/// @param data The lines as drawn, and how many the list shows.
/// @param row The scrollbar's cell pointed at, 0 at its top; past its bottom counts as its bottom.
void jumpWorkList(WorkState& state, WorkBoardData const& data, std::size_t row);

/// @param ms How long ago, in milliseconds.
/// @return `45s`, `12m`, `3h`, `2d`: the age of a last update, rounded down.
[[nodiscard]] std::string updateAgeText(uint64_t ms);

/// @param ms How long ago, in milliseconds.
/// @return How many milliseconds later updateAgeText() says something else: the rest of its unit.
[[nodiscard]] uint64_t untilAgeChanges(uint64_t ms) noexcept;

/// Applies one Work-view command. Fold on a line, or Enter on a heading, folds its group by ToggleFoldAction
/// of the group's key and selects the heading; Sort toggles SortByUpdateKey; Enter on an item opens the start
/// dialog on it (its session id left for the caller to mint), or says on the action bar why it cannot.
/// RowMenu opens the row menu on a selected item, its place left for the caller to set. SwitchView, NewTask
/// and Palette act as in the Agents view (dispatch()).
/// @param command The command (KeyCommand).
/// @param interaction Where the user is; its WorkState changed.
/// @param lines The lines as drawn.
/// @param items The items they index.
/// @param engine The engine: its repositories and their forges.
/// @param pageRows How many lines the list shows: what a page key moves by.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> applyWorkCommand(KeyCommand command,
                                                       Interaction& interaction,
                                                       std::span<WorkLine const> lines,
                                                       std::span<WorkItem const> items,
                                                       Engine const& engine,
                                                       std::size_t pageRows);

/// Applies one key in the Work view: to its filter while it is typed (Enter or Esc give the list the keys
/// again), else by workKeyRows().
/// @param key A key the terminal sent.
/// @param interaction Where the user is; changed by the key.
/// @param lines The lines as drawn.
/// @param items The items they index.
/// @param engine The engine.
/// @param pageRows How many lines the list shows.
/// @return What the engine is asked, if anything.
[[nodiscard]] std::optional<UiAction> applyWorkKey(core::tui::KeyEvent const& key,
                                                   Interaction& interaction,
                                                   std::span<WorkLine const> lines,
                                                   std::span<WorkItem const> items,
                                                   Engine const& engine,
                                                   std::size_t pageRows);

/// The Work view: the filter bar (the filter, the repository chip, the sort chip), the lines that fit, the
/// scrollbar on the list's right edge, and the details line of the selected one; the hovered item line
/// carries a `▏`. Records the hits of its chips (WorkChip), of each drawn line (WorkRow, WorkHeading), of
/// each item's MR number and ticket key (WorkMrLink, WorkTicketLink), and of the list's right edge
/// (WorkScrollbar).
class WorkBoardView final: public core::tui::Component
{
  public:
    /// @param engine The engine: its repositories, forges and wall clock.
    /// @param interaction Where the user is: its scroll is kept so that the selection stays in view.
    /// @param data What it draws: its listRows, wall sample and next age change are set as it draws.
    /// @param hits Where it records what a click lands on.
    /// @param clock What the ages move on with between the engine's wakes; it must outlive the view.
    WorkBoardView(Engine const& engine,
                  Interaction& interaction,
                  WorkBoardData& data,
                  HitMap& hits,
                  core::IClock const& clock CORE_LIFETIMEBOUND);

    void render(core::tui::Canvas& canvas) override;

  private:
    Engine const& _engine;
    Interaction& _interaction;
    WorkBoardData& _data;
    HitMap& _hits;
    core::IClock const& _clock;
};

} // namespace orchestrator::view
