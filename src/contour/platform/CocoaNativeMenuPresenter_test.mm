// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa presenter builds an NSMenu from a MenuTree. Popping one up is modal, so these tests build the
// menu exactly as present() would and read each NSMenuItem back through outlineNativeMenu(); no display
// is needed. The one path that would pop is skipped under the cocoa platform plugin.

#include <contour/command/MenuTree.hpp>
#include <contour/command/TabMenus.hpp>
#include <contour/platform/CocoaNativeMenu.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <ranges>
#include <vector>

using namespace contour::command;
using contour::platform::MenuAnchor;
using contour::platform::MenuPresentError;
using contour::platform::NativeMenuOutline;
using contour::platform::SwatchImage;
using vtbackend::RGBColor;

namespace
{
[[nodiscard]] TabContextMenuState tabs(int count)
{
    return TabContextMenuState {
        .tabIndex = 0,
        .tabCount = count,
        .palette = { RGBColor { 255, 0, 0 }, RGBColor { 0, 255, 0 }, RGBColor { 0, 0, 255 } },
        .color = RGBColor { 0, 255, 0 },
        .userColor = UserColorPresence::Absent,
    };
}

[[nodiscard]] MenuTree tabMenuTree(int count)
{
    return buildMenuTree(buildTabContextMenu(tabs(count)), {}, ShortcutStyle::MacGlyphs);
}

/// An NSMenuItem has no "no check column" state: Unchecked and None both read back as None.
[[nodiscard]] RowCheck shownCheck(RowCheck check)
{
    return check == RowCheck::Checked ? RowCheck::Checked : RowCheck::None;
}

void checkMirrors(std::vector<MenuNode> const& nodes, std::vector<NativeMenuOutline> const& outline)
{
    REQUIRE(outline.size() == nodes.size());
    for (auto const& [node, item]: std::views::zip(nodes, outline))
    {
        CHECK(item.kind == node.kind);
        CHECK(item.title == node.title);
        CHECK(item.check == shownCheck(node.check));
        CHECK(item.image == (node.swatch ? SwatchImage::Present : SwatchImage::Absent));
        if (node.kind != ContextMenuEntryKind::Separator)
            CHECK(item.availability == node.availability);
        if (node.kind == ContextMenuEntryKind::Command)
            CHECK(item.tag == static_cast<long>(node.actionIndex));
        else
            CHECK(item.tag == -1);
        checkMirrors(node.children, item.children);
    }
}

[[nodiscard]] std::size_t swatchCount(std::vector<NativeMenuOutline> const& outline)
{
    auto count = std::size_t { 0 };
    for (auto const& item: outline)
        count += (item.image == SwatchImage::Present ? 1 : 0) + swatchCount(item.children);
    return count;
}
} // namespace

TEST_CASE("the outline mirrors the tree", "[nativemenu][cocoa]")
{
    auto const tree = tabMenuTree(3);
    auto const outline = contour::platform::outlineNativeMenu(tree);
    checkMirrors(tree.nodes, outline);
    CHECK(swatchCount(outline) == 3);

    // The tab's own colour is the ticked swatch.
    auto const& colors = outline.at(1);
    REQUIRE(colors.kind == ContextMenuEntryKind::Submenu);
    REQUIRE(colors.children.size() >= 3);
    CHECK(colors.children[0].check == RowCheck::None);
    CHECK(colors.children[1].check == RowCheck::Checked);
    CHECK(colors.children[2].check == RowCheck::None);
}

TEST_CASE("a disabled row stays disabled", "[nativemenu][cocoa]")
{
    auto const tree = tabMenuTree(1);
    auto const outline = contour::platform::outlineNativeMenu(tree);
    checkMirrors(tree.nodes, outline);

    auto disabled = std::size_t { 0 };
    for (auto const& [node, item]: std::views::zip(tree.nodes, outline))
        if (node.availability == RowAvailability::Disabled)
        {
            CHECK(item.availability == RowAvailability::Disabled);
            ++disabled;
        }
    // "Move Tab to New Window", "Close Other Tabs" and "Close Tabs to the Right" with a single tab.
    CHECK(disabled == 3);
}

TEST_CASE("the factory is native on Apple", "[nativemenu][cocoa]")
{
    auto const presenter = contour::platform::makeNativeMenuPresenter();
    REQUIRE(presenter != nullptr);
    CHECK(dynamic_cast<contour::platform::NullNativeMenuPresenter const*>(presenter.get()) == nullptr);
}

TEST_CASE("a position needs a window", "[nativemenu][cocoa]")
{
    auto const presenter = contour::platform::makeNativeMenuPresenter();
    auto const anchor = MenuAnchor { .window = nullptr, .position = QPointF { 1, 1 } };
    auto const result = presenter->present(tabMenuTree(3), anchor, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MenuPresentError::NoWindow);
}

TEST_CASE("a position needs a created window", "[nativemenu][cocoa]")
{
    // Never create()d, so it has no platform window; present() must not make one through winId().
    auto window = QWindow {};
    REQUIRE(window.handle() == nullptr);
    auto const presenter = contour::platform::makeNativeMenuPresenter();
    auto const anchor = MenuAnchor { .window = &window, .position = QPointF { 1, 1 } };
    auto const result = presenter->present(tabMenuTree(3), anchor, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MenuPresentError::NoWindow);
    CHECK(window.handle() == nullptr);
}

TEST_CASE("off the cocoa plugin it is Unsupported", "[nativemenu][cocoa]")
{
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        SKIP("Under the cocoa platform plugin this would pop a modal menu.");
    auto const presenter = contour::platform::makeNativeMenuPresenter();
    auto const result =
        presenter->present(tabMenuTree(3), MenuAnchor { .window = nullptr, .position = std::nullopt }, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MenuPresentError::Unsupported);
}
