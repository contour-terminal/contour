// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>

#include <vtbackend/core/Color.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace contour::command
{

/// One row of a resolved menu: what every presenter draws.
struct MenuNode
{
    ContextMenuEntryKind kind = ContextMenuEntryKind::Command;
    std::string title {};
    RowAvailability availability = RowAvailability::Enabled;
    RowCheck check = RowCheck::None;
    MenuItemRole role = MenuItemRole::None;
    std::optional<vtbackend::RGBColor> swatch {};
    std::optional<ShortcutChord> chord {};   ///< Command rows: the chord that runs it, if shown.
    std::string shortcut {};                 ///< @ref chord in the tree's style; empty without one.
    std::size_t actionIndex = 0;             ///< Command rows: index into MenuTree::actions.
    std::vector<MenuNode> children {};       ///< Submenu rows.
    ActionScope scope = ActionScope::Window; ///< Command rows: what the row acts on.
    [[nodiscard]] bool operator==(MenuNode const&) const = default;
};

/// A resolved menu and the actions its command rows run, by MenuNode::actionIndex.
struct MenuTree
{
    std::vector<MenuNode> nodes {};
    std::vector<actions::Action> actions {};
};

/// Resolves @p entries into a tree of rows, chords and actions.
///
/// Actions are numbered in a depth-first pre-order walk. A chord is attached to a command row only if no
/// earlier row in that walk already shows it, so a chord appears on one row.
/// @param entries The menu rows.
/// @param chords  Command id -> chord, see chordIndex().
/// @param style   How to spell the chords.
/// @return The resolved menu.
[[nodiscard]] MenuTree buildMenuTree(std::span<ContextMenuEntry const> entries,
                                     ChordIndex const& chords,
                                     ShortcutStyle style);

} // namespace contour::command
