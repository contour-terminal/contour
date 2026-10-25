// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenu.hpp>
#include <contour/window/ContextMenuModel.hpp>

#include <QtCore/QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{

/// The one command row built from @p availability and @p check, as QML receives it.
[[nodiscard]] QVariantMap modelRowFor(RowAvailability availability, RowCheck check)
{
    auto const entries =
        std::vector<ContextMenuEntry> { ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                                           .title = "Row",
                                                           .action = actions::Quit {},
                                                           .availability = availability,
                                                           .check = check,
                                                           .role = MenuItemRole::None,
                                                           .swatch = std::nullopt,
                                                           .children = {} } };
    auto routed = std::vector<actions::Action> {};
    auto const model = window::toContextMenuModel(entries, routed);
    REQUIRE(model.size() == 1);
    return model.front().toMap();
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
