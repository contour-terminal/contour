// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/ContextMenu.h>

#include <QtCore/QVariantList>

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

} // namespace contour
