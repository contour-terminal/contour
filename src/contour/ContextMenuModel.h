// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/ContextMenu.h>

#include <QtCore/QVariantList>

#include <optional>
#include <utility>
#include <vector>

namespace contour
{

/// The context menu, as QML consumes it.
///
/// Turns the entry tree into nested QVariantMaps — { kind, title, enabled, checkable, checked, actionId,
/// children } — and lifts every command's action out into @p actions, which the row then refers to by
/// index. Carrying the index rather than a command NAME is what lets a click run the exact action the row
/// was built with: nothing is looked up by name at click time, and nothing depends on some other registry
/// having been populated first.
///
/// Kept out of ContextMenu.h so the decision layer stays Qt-free and headlessly testable — the same split
/// as Command.h (pure) and CommandPaletteModel.h (Qt).
///
/// @param entries The menu, as buildContextMenu() produced it.
/// @param actions Out: the actions the rows run, indexed by their `actionId`. Appended to.
/// @return The rows, in menu order.
[[nodiscard]] QVariantList toContextMenuModel(std::vector<ContextMenuEntry> const& entries,
                                              std::vector<actions::Action>& actions);

/// A published context menu: the rows QML renders, plus the actions those rows carry.
///
/// One per menu SURFACE. Publishing a second menu into the first's model would rebuild -- and re-target
/// -- a menu the user may still have open, so the terminal pane and the title bar hold one of these
/// each rather than sharing.
struct PublishedContextMenu
{
    QVariantList model;                   ///< What QML renders.
    std::vector<actions::Action> actions; ///< What the rows run, indexed by their `actionId`.

    /// Replaces the published rows with @p entries.
    void publish(std::vector<ContextMenuEntry> const& entries)
    {
        actions.clear();
        model = toContextMenuModel(entries, actions);
    }

    /// The action row @p actionId carries.
    ///
    /// @return The action, or nullopt when the id names no row -- which happens when a click arrives
    ///         against a menu that has since been rebuilt.
    [[nodiscard]] std::optional<actions::Action> actionAt(int actionId) const
    {
        if (actionId < 0 || std::cmp_greater_equal(actionId, actions.size()))
            return std::nullopt;
        return actions[static_cast<size_t>(actionId)];
    }
};

} // namespace contour
