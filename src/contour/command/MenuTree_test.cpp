// SPDX-License-Identifier: Apache-2.0
//
// MenuTree resolves a menu into rows, chords and actions with no window and no Qt.

#include <contour/command/MenuTree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>
#include <vector>

using namespace contour::command;
namespace actions = contour::actions;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{
[[nodiscard]] ContextMenuEntry commandRow(std::string title, actions::Action action)
{
    return ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                              .title = std::move(title),
                              .action = std::move(action) };
}

[[nodiscard]] ContextMenuEntry submenuRow(std::string title, std::vector<ContextMenuEntry> children)
{
    return ContextMenuEntry { .kind = ContextMenuEntryKind::Submenu,
                              .title = std::move(title),
                              .children = std::move(children) };
}

[[nodiscard]] ContextMenuEntry separatorRow()
{
    return ContextMenuEntry { .kind = ContextMenuEntryKind::Separator };
}

[[nodiscard]] ShortcutChord ctrlShiftT()
{
    return ShortcutChord { .modifiers = Modifiers { Modifier::Control, Modifier::Shift }, .input = U'T' };
}

[[nodiscard]] std::size_t countCommands(std::vector<MenuNode> const& nodes)
{
    auto count = std::size_t { 0 };
    for (auto const& node: nodes)
    {
        if (node.kind == ContextMenuEntryKind::Command)
            ++count;
        count += countCommands(node.children);
    }
    return count;
}
} // namespace

TEST_CASE("menuTree.actionsInWalkOrder", "[menutree]")
{
    auto const entries = std::vector {
        commandRow("A", actions::CloseTab {}),
        submenuRow(
            "S", { commandRow("B", actions::MoveTabToLeft {}), commandRow("C", actions::MoveTabToRight {}) }),
        commandRow("D", actions::CreateNewTab {}),
    };
    auto const tree = buildMenuTree(entries, ChordIndex {}, ShortcutStyle::Text);

    REQUIRE(tree.actions.size() == 4);
    CHECK(std::holds_alternative<actions::CloseTab>(tree.actions[0]));
    CHECK(std::holds_alternative<actions::MoveTabToLeft>(tree.actions[1]));
    CHECK(std::holds_alternative<actions::MoveTabToRight>(tree.actions[2]));
    CHECK(std::holds_alternative<actions::CreateNewTab>(tree.actions[3]));
    CHECK(tree.nodes[0].actionIndex == 0);
    CHECK(tree.nodes[1].children[0].actionIndex == 1);
    CHECK(tree.nodes[1].children[1].actionIndex == 2);
    CHECK(tree.nodes[2].actionIndex == 3);
}

TEST_CASE("menuTree.copiesEveryColumn", "[menutree]")
{
    auto entry = commandRow("Colour", actions::CloseTab {});
    entry.availability = RowAvailability::Disabled;
    entry.check = RowCheck::Checked;
    entry.role = MenuItemRole::Quit;
    entry.swatch = vtbackend::RGBColor { 1, 2, 3 };
    auto const entries = std::vector { entry };

    auto const tree = buildMenuTree(entries, ChordIndex {}, ShortcutStyle::Text);

    REQUIRE(tree.nodes.size() == 1);
    auto const& node = tree.nodes.front();
    CHECK(node.title == "Colour");
    CHECK(node.availability == RowAvailability::Disabled);
    CHECK(node.check == RowCheck::Checked);
    CHECK(node.role == MenuItemRole::Quit);
    CHECK(node.swatch == std::optional<vtbackend::RGBColor> { vtbackend::RGBColor { 1, 2, 3 } });
}

TEST_CASE("menuTree.chordFromTheIndex", "[menutree]")
{
    auto const chords = ChordIndex { { "CreateNewTab", ctrlShiftT() } };
    auto const entries = std::vector {
        commandRow("New Tab", actions::CreateNewTab {}),
        commandRow("Close", actions::CloseTab {}),
    };

    auto const text = buildMenuTree(entries, chords, ShortcutStyle::Text);
    CHECK(text.nodes[0].chord == std::optional<ShortcutChord> { ctrlShiftT() });
    CHECK(text.nodes[0].shortcut == "Ctrl+Shift+T");
    CHECK_FALSE(text.nodes[1].chord.has_value());
    CHECK(text.nodes[1].shortcut.empty());

    auto const glyphs = buildMenuTree(entries, chords, ShortcutStyle::MacGlyphs);
    CHECK(glyphs.nodes[0].shortcut == "⌃⇧T");
}

TEST_CASE("menuTree.aChordIsShownOnce", "[menutree]")
{
    auto const chords = ChordIndex { { "CreateNewTab", ctrlShiftT() }, { "CloseTab", ctrlShiftT() } };
    auto const entries = std::vector {
        submenuRow("S", { commandRow("New Tab", actions::CreateNewTab {}) }),
        commandRow("Close", actions::CloseTab {}),
    };

    auto const tree = buildMenuTree(entries, chords, ShortcutStyle::Text);

    CHECK(tree.nodes[0].children[0].chord.has_value());
    CHECK_FALSE(tree.nodes[1].chord.has_value());
    CHECK(tree.nodes[1].shortcut.empty());
}

TEST_CASE("menuTree.separatorsAndSubmenusCarryNoAction", "[menutree]")
{
    auto const entries = std::vector {
        commandRow("A", actions::CloseTab {}),
        separatorRow(),
        submenuRow("S", { commandRow("B", actions::MoveTabToLeft {}), separatorRow() }),
    };

    auto const tree = buildMenuTree(entries, ChordIndex {}, ShortcutStyle::Text);

    CHECK(tree.actions.size() == countCommands(tree.nodes));
    CHECK(tree.actions.size() == 2);
}
