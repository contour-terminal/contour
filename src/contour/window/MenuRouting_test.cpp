// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Command.hpp>
#include <contour/test/MenuFixtures.hpp>
#include <contour/window/MenuRouting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <ranges>

using contour::config::MenuPresentation;
using contour::platform::MenuAnchor;
using contour::platform::MenuPresentError;
using contour::window::MenuRoute;
using contour::window::presentMenu;

namespace
{
contour::command::MenuTree sampleTree()
{
    using contour::command::MenuNode;
    return contour::command::MenuTree {
        .nodes = { MenuNode { .kind = contour::command::ContextMenuEntryKind::Command,
                              .title = "New Tab",
                              .availability = contour::command::RowAvailability::Enabled,
                              .check = contour::command::RowCheck::None,
                              .role = contour::command::MenuItemRole::None,
                              .swatch = std::nullopt,
                              .chord = std::nullopt,
                              .shortcut = {},
                              .actionIndex = 0,
                              .children = {} },
                   MenuNode { .kind = contour::command::ContextMenuEntryKind::Command,
                              .title = "Copy",
                              .availability = contour::command::RowAvailability::Enabled,
                              .check = contour::command::RowCheck::None,
                              .role = contour::command::MenuItemRole::None,
                              .swatch = std::nullopt,
                              .chord = std::nullopt,
                              .shortcut = {},
                              .actionIndex = 1,
                              .children = {} } },
        .actions = { contour::actions::NewTerminal {}, contour::actions::CopySelection {} },
    };
}
} // namespace

TEST_CASE("presentMenu keeps an in-scene menu in the scene", "[menurouting]")
{
    auto presenter = contour::test::RecordingMenuPresenter {};
    auto const route =
        presentMenu(MenuPresentation::InScene, presenter, sampleTree(), MenuAnchor {}, [](std::size_t) {});
    CHECK(route == MenuRoute::InScene);
    CHECK(presenter.trees.empty());
}

TEST_CASE("presentMenu hands a native menu to the presenter", "[menurouting]")
{
    auto presenter = contour::test::RecordingMenuPresenter {};
    auto const tree = sampleTree();
    auto const anchor = MenuAnchor { .window = nullptr, .position = QPointF { 12.0, 34.0 } };
    auto const route = presentMenu(MenuPresentation::Native, presenter, tree, anchor, [](std::size_t) {});
    CHECK(route == MenuRoute::Native);
    REQUIRE(presenter.trees.size() == 1);
    CHECK(presenter.trees.front().nodes == tree.nodes);
    auto const& actions = presenter.trees.front().actions;
    REQUIRE(actions.size() == tree.actions.size());
    for (auto const i: std::views::iota(std::size_t { 0 }, actions.size()))
        CHECK(contour::command::commandId(actions[i]) == contour::command::commandId(tree.actions[i]));
    REQUIRE(presenter.anchors.size() == 1);
    CHECK(presenter.anchors.front().window == anchor.window);
    CHECK(presenter.anchors.front().position == anchor.position);
}

TEST_CASE("presentMenu falls back to the scene when the presenter cannot show it", "[menurouting]")
{
    auto presenter = contour::test::RecordingMenuPresenter {};
    presenter.result = std::unexpected(MenuPresentError::Unsupported);
    auto const route =
        presentMenu(MenuPresentation::Native, presenter, sampleTree(), MenuAnchor {}, [](std::size_t) {});
    CHECK(route == MenuRoute::InScene);
    CHECK(presenter.trees.size() == 1);
}

TEST_CASE("presentMenu passes the pick callback through", "[menurouting]")
{
    auto presenter = contour::test::RecordingMenuPresenter {};
    auto picked = std::optional<std::size_t> {};
    auto const route =
        presentMenu(MenuPresentation::Native, presenter, sampleTree(), MenuAnchor {}, [&](std::size_t index) {
            picked = index;
        });
    CHECK(route == MenuRoute::Native);
    presenter.pick(2);
    CHECK(picked == std::size_t { 2 });
}
