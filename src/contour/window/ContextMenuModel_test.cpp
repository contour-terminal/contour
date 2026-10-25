// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/window/ContextMenuModel.hpp>

#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtGui/QColor>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{

/// The one command row built from @p availability and @p check, as QML receives it.
[[nodiscard]] QVariantMap modelRowFor(RowAvailability availability, RowCheck check)
{
    auto const nodes = std::vector<MenuNode> { MenuNode {
        .kind = ContextMenuEntryKind::Command,
        .title = "Row",
        .availability = availability,
        .check = check,
        .role = MenuItemRole::None,
        .swatch = std::nullopt,
        .chord = std::nullopt,
        .shortcut = {},
        .actionIndex = 0,
        .children = {},
    } };
    auto const model = window::toContextMenuModel(nodes);
    REQUIRE(model.size() == 1);
    return model.front().toMap();
}

/// A command node titled @p title, with @p swatch, running action @p actionIndex.
[[nodiscard]] MenuNode commandNode(std::string title,
                                   std::size_t actionIndex,
                                   std::optional<vtbackend::RGBColor> swatch = std::nullopt,
                                   RowCheck check = RowCheck::None)
{
    return MenuNode {
        .kind = ContextMenuEntryKind::Command,
        .title = std::move(title),
        .availability = RowAvailability::Enabled,
        .check = check,
        .role = MenuItemRole::None,
        .swatch = swatch,
        .chord = std::nullopt,
        .shortcut = {},
        .actionIndex = actionIndex,
        .children = {},
    };
}

} // namespace

TEST_CASE("ContextMenuModel.booleansAtTheQmlBoundary", "[contextmenumodel]")
{
    SECTION("enabled, no check column")
    {
        auto const row = modelRowFor(RowAvailability::Enabled, RowCheck::None);
        CHECK(row["enabled"].toBool());
        CHECK_FALSE(row["checkable"].toBool());
        CHECK_FALSE(row["checked"].toBool());
    }

    SECTION("disabled, unchecked")
    {
        auto const row = modelRowFor(RowAvailability::Disabled, RowCheck::Unchecked);
        CHECK_FALSE(row["enabled"].toBool());
        CHECK(row["checkable"].toBool());
        CHECK_FALSE(row["checked"].toBool());
    }

    SECTION("enabled, checked")
    {
        auto const row = modelRowFor(RowAvailability::Enabled, RowCheck::Checked);
        CHECK(row["enabled"].toBool());
        CHECK(row["checkable"].toBool());
        CHECK(row["checked"].toBool());
    }
}

TEST_CASE("ContextMenuModel.swatch rows group into one row", "[contextmenumodel]")
{
    using namespace vtbackend;
    auto submenu = MenuNode {
        .kind = ContextMenuEntryKind::Submenu,
        .title = "Tab Color",
        .availability = RowAvailability::Enabled,
        .check = RowCheck::None,
        .role = MenuItemRole::None,
        .swatch = std::nullopt,
        .chord = std::nullopt,
        .shortcut = {},
        .actionIndex = 0,
        .children = {},
    };
    submenu.children = {
        commandNode("#FF0000", 0, 0xFF0000_rgb, RowCheck::Unchecked),
        commandNode("#00FF00", 1, 0x00FF00_rgb, RowCheck::Checked),
        commandNode("#0000FF", 2, 0x0000FF_rgb, RowCheck::Unchecked),
        MenuNode {
            .kind = ContextMenuEntryKind::Separator,
            .title = {},
            .availability = RowAvailability::Enabled,
            .check = RowCheck::None,
            .role = MenuItemRole::None,
            .swatch = std::nullopt,
            .chord = std::nullopt,
            .shortcut = {},
            .actionIndex = 0,
            .children = {},
        },
        commandNode("Custom…", 3),
        commandNode("Reset", 4),
    };
    auto const nodes = std::vector<MenuNode> { submenu };

    auto const model = window::toContextMenuModel(nodes);
    REQUIRE(model.size() == 1);
    auto const submenuRow = model.front().toMap();
    CHECK(submenuRow["kind"].toString() == "submenu");
    CHECK(submenuRow["enabled"].toBool());

    auto const children = submenuRow["children"].toList();
    auto kinds = QStringList {};
    for (auto const& child: children)
        kinds.push_back(child.toMap()["kind"].toString());
    CHECK(kinds == QStringList { "swatches", "separator", "command", "command" });

    auto const swatches = children.front().toMap()["swatches"].toList();
    REQUIRE(swatches.size() == 3);
    auto const expected =
        std::array { QColor(0xFF, 0x00, 0x00), QColor(0x00, 0xFF, 0x00), QColor(0x00, 0x00, 0xFF) };
    for (auto const i: std::views::iota(0, 3))
    {
        auto const swatch = swatches[i].toMap();
        CHECK(swatch["actionId"].toInt() == i);
        CHECK(swatch["color"].value<QColor>() == expected[static_cast<std::size_t>(i)]);
        CHECK(swatch["checked"].toBool() == (i == 1));
        CHECK(swatch["enabled"].toBool());
    }
    CHECK(children[2].toMap()["actionId"].toInt() == 3);
    CHECK(children[3].toMap()["actionId"].toInt() == 4);
}
