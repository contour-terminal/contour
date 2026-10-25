// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuTree.hpp>
#include <contour/config/Actions.hpp>

#include <QtCore/QVariantList>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace contour::window
{

/// A resolved menu, as QML consumes it.
///
/// Turns the tree into nested QVariantMaps -- { kind, title, enabled, checkable, checked, actionId, children
/// }
/// -- where `actionId` is the node's MenuNode::actionIndex into MenuTree::actions. Carrying the index rather
/// than a command NAME is what lets a click run the exact action the row was built with: nothing is looked
/// up by name at click time, and nothing depends on some other registry having been populated first.
///
/// A run of consecutive command nodes that carry a swatch becomes one row
/// { kind: "swatches", swatches: [ { title, color, actionId, enabled, checked } ] }, so QML can lay the
/// colours out side by side instead of one per line.
///
/// Kept out of ContextMenu.hpp so the decision layer stays Qt-free and headlessly testable -- the same
/// split as Command.hpp (pure) and CommandPaletteModel.hpp (Qt).
///
/// @param nodes The rows, as buildMenuTree() resolved them.
/// @return The rows, in menu order.
[[nodiscard]] QVariantList toContextMenuModel(std::span<command::MenuNode const> nodes);

/// A published context menu: the rows QML renders, plus the actions those rows carry.
///
/// One per menu SURFACE. Publishing a second menu into the first's model would rebuild -- and re-target
/// -- a menu the user may still have open, so every menu surface holds one of these rather than sharing.
struct PublishedContextMenu
{
    QVariantList model {};                   ///< What QML renders.
    std::vector<actions::Action> actions {}; ///< What the rows run, indexed by their `actionId`.

    /// Replaces the published rows with @p tree.
    /// @param tree The resolved menu; its actions are taken over.
    void publish(command::MenuTree tree)
    {
        model = toContextMenuModel(tree.nodes);
        actions = std::move(tree.actions);
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

} // namespace contour::window
