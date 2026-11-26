// SPDX-License-Identifier: Apache-2.0
//
// "Run a task on this group": the palette opens it on the selected Work line's group; ←/→ choose the task,
// each item's suggested one or one for all; ⏎ starts it on every item of the group no agent works on, as free
// slots allow (RunOnItemsAction); esc closes it.
#pragma once

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkGroups.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/HitMap.hpp>

namespace orchestrator::view
{

/// An open "run a task on this group" dialog.
struct BulkDialogState
{
    WorkGroup group = WorkGroup::NeedsYou; ///< The group it was opened on.
    std::vector<std::string> items = {};   ///< Its items no agent works on, by workItemKey(), as shown.
    std::size_t choice = 0;                ///< Into bulkChoices(): 0 is each item's suggested task.
    [[nodiscard]] bool operator==(BulkDialogState const&) const = default;
};

/// @param engine The engine: its tasks.
/// @param group The group.
/// @param shown The group's items, as the board shows them.
/// @return The dialog on the items of @p shown no agent works on; std::nullopt when there is none.
[[nodiscard]] std::optional<BulkDialogState> openBulkDialog(Engine const& engine,
                                                            WorkGroup group,
                                                            std::span<WorkItem const> shown);

/// @param engine The engine: its tasks and its items.
/// @param dialog The dialog.
/// @return What ←/→ choose among: an empty id for each item's suggested task, then the id of every task that
///         applies to some item of the dialog, in configuration order.
[[nodiscard]] std::vector<std::string> bulkChoices(Engine const& engine, BulkDialogState const& dialog);

/// @param engine The engine.
/// @param dialog The dialog.
/// @return The items the chosen task starts on: those still listed and free that it applies to (the suggested
///         task: those some task is suggested for), in the dialog's order.
[[nodiscard]] std::vector<std::string> bulkItems(Engine const& engine, BulkDialogState const& dialog);

/// @param engine The engine.
/// @param dialog The dialog.
/// @return How many items ⏎ starts on: bulkItems()' size.
[[nodiscard]] std::size_t bulkCount(Engine const& engine, BulkDialogState const& dialog);

/// @param engine The engine.
/// @param dialog The dialog.
/// @return What the dialog says: `Start <task> on <n> <merge requests|tickets|items>` (or that no item takes
///         that task), then where each runs, then its keys.
[[nodiscard]] std::vector<std::string> bulkLines(Engine const& engine, BulkDialogState const& dialog);

/// The dialog, in a rounded box over the dashboard.
class BulkDialogView final: public core::tui::Component
{
  public:
    /// @param engine The engine whose tasks it names.
    /// @param dialog The open dialog, read at every frame; outlives the view.
    /// @param hits Where it records the area it covers.
    BulkDialogView(Engine const& engine, std::optional<BulkDialogState> const& dialog, HitMap& hits):
        _engine { engine }, _dialog { dialog }, _hits { hits }
    {
    }
    void render(core::tui::Canvas& canvas) override;
    /// @return The size it needs: its lines and its box.
    [[nodiscard]] core::tui::Size preferredSize() const override;

  private:
    Engine const& _engine;
    std::optional<BulkDialogState> const& _dialog;
    HitMap& _hits;
};

} // namespace orchestrator::view
