// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Command.hpp>
#include <contour/command/MenuTree.hpp>

#include <algorithm>

namespace contour::command
{

namespace
{
    struct Walk
    {
        ChordIndex const& chords;
        ShortcutStyle style;
        MenuTree& tree;
        std::vector<ShortcutChord> shown {};

        [[nodiscard]] std::vector<MenuNode> nodes(std::span<ContextMenuEntry const> entries)
        {
            auto result = std::vector<MenuNode> {};
            result.reserve(entries.size());
            for (auto const& entry: entries)
                result.push_back(node(entry));
            return result;
        }

        [[nodiscard]] MenuNode node(ContextMenuEntry const& entry)
        {
            auto result = MenuNode {
                .kind = entry.kind,
                .title = entry.title,
                .availability = entry.availability,
                .check = entry.check,
                .role = entry.role,
                .swatch = entry.swatch,
                .scope = entry.scope,
            };
            switch (entry.kind)
            {
                case ContextMenuEntryKind::Separator: break;
                case ContextMenuEntryKind::Submenu: result.children = nodes(entry.children); break;
                case ContextMenuEntryKind::Command:
                    result.actionIndex = tree.actions.size();
                    tree.actions.push_back(entry.action);
                    attachChord(result, entry);
                    break;
            }
            return result;
        }

        void attachChord(MenuNode& result, ContextMenuEntry const& entry)
        {
            auto const found = chords.find(commandId(entry.action));
            if (found == chords.end() || std::ranges::contains(shown, found->second))
                return;
            shown.push_back(found->second);
            result.chord = found->second;
            result.shortcut = shortcutText(found->second, style);
        }
    };
} // namespace

MenuTree buildMenuTree(std::span<ContextMenuEntry const> entries,
                       ChordIndex const& chords,
                       ShortcutStyle style)
{
    auto tree = MenuTree {};
    auto walk = Walk { .chords = chords, .style = style, .tree = tree };
    tree.nodes = walk.nodes(entries);
    return tree;
}

} // namespace contour::command
