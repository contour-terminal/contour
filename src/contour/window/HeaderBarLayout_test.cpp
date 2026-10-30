// SPDX-License-Identifier: Apache-2.0
#include <contour/window/HeaderBarLayout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>

using namespace contour::window;
using contour::config::HeaderBarArrangement;
using contour::config::TabShape;

namespace
{

struct HeaderBarCase
{
    HeaderBarInputs inputs;
    HeaderBar expected;
};

constexpr auto Band = HeaderBarArrangement::Band;
constexpr auto Unified = HeaderBarArrangement::Unified;
constexpr auto Pill = TabShape::Pill;
constexpr auto Flat = TabShape::Flat;
constexpr auto ClientBand = WindowFrame::ClientBand;
constexpr auto NativeFrame = WindowFrame::NativeFrame;
constexpr auto NoControls = ControlsPresence::None;
constexpr auto Controls = ControlsPresence::Present;
constexpr auto Hidden = HeaderBarSlot::Hidden;
constexpr auto Leading = HeaderBarSlot::Leading;
constexpr auto Trailing = HeaderBarSlot::Trailing;
constexpr auto NoSeparator = ControlsSeparator::None;
constexpr auto Separator = ControlsSeparator::Shown;

constexpr auto HeaderBarCases = std::array {
    // The canvas: new tab leading, search and menu trailing, the window buttons trailing past a separator.
    HeaderBarCase { { Unified, Pill, ClientBand, NoControls, Controls },
                    { Leading, Trailing, Trailing, NoSeparator, Separator } },
    // Buttons on the left (Review Focus 1): the separator sits between them and the new-tab button.
    HeaderBarCase { { Unified, Pill, ClientBand, Controls, NoControls },
                    { Leading, Trailing, Trailing, Separator, NoSeparator } },
    HeaderBarCase { { Unified, Pill, ClientBand, Controls, Controls },
                    { Leading, Trailing, Trailing, Separator, Separator } },
    // Server-side decorations (Review Focus 4): the items stay, the window manager draws the buttons.
    HeaderBarCase { { Unified, Pill, NativeFrame, NoControls, NoControls },
                    { Leading, Trailing, Trailing, NoSeparator, NoSeparator } },
    // A native frame never shows a separator, whatever the provider says about controls.
    HeaderBarCase { { Unified, Pill, NativeFrame, Controls, Controls },
                    { Leading, Trailing, Trailing, NoSeparator, NoSeparator } },
    // No buttons wanted (Review Focus 5, a desktop layout of ":").
    HeaderBarCase { { Unified, Pill, ClientBand, NoControls, NoControls },
                    { Leading, Trailing, Trailing, NoSeparator, NoSeparator } },
    // A Unified row matches whatever its tab shape.
    HeaderBarCase { { Unified, Flat, ClientBand, NoControls, Controls },
                    { Leading, Trailing, Trailing, NoSeparator, Separator } },
    // macOS, unchanged: the pill row's "+" trails the tabs; a Band row never separates.
    HeaderBarCase { { Band, Pill, ClientBand, Controls, NoControls },
                    { Trailing, Hidden, Hidden, NoSeparator, NoSeparator } },
    // Fluent and Terminal, unchanged: the "+" stays inside the strip.
    HeaderBarCase { { Band, Flat, ClientBand, NoControls, Controls },
                    { Hidden, Hidden, Hidden, NoSeparator, NoSeparator } },
};

} // namespace

TEST_CASE("headerBar.rules", "[contour][headerbar]")
{
    for (auto const& [inputs, expected]: HeaderBarCases)
    {
        INFO("arrangement " << static_cast<int>(inputs.arrangement) << " shape "
                            << static_cast<int>(inputs.shape) << " frame " << static_cast<int>(inputs.frame)
                            << " leading " << static_cast<int>(inputs.leading) << " trailing "
                            << static_cast<int>(inputs.trailing));
        CHECK(resolveHeaderBar(inputs) == expected);
    }
}

TEST_CASE("headerBar.rules: every rule names a row the resolver reaches", "[contour][headerbar]")
{
    // Each rule must be the first match for its own inputs, or a later row is dead.
    for (auto const& rule: HeaderBarRules)
    {
        auto const shape = rule.shape.value_or(TabShape::Flat);
        auto const bar = resolveHeaderBar(HeaderBarInputs { .arrangement = rule.arrangement,
                                                            .shape = shape,
                                                            .frame = NativeFrame,
                                                            .leading = NoControls,
                                                            .trailing = NoControls });
        CHECK(bar.newTab == rule.newTab);
        CHECK(bar.search == rule.search);
        CHECK(bar.mainMenu == rule.mainMenu);
    }
}

TEST_CASE("headerBar.items", "[contour][headerbar]")
{
    STATIC_REQUIRE(HeaderBarItems.size() == HeaderBarItemCount);
    CHECK(HeaderBarItems.size() == static_cast<std::size_t>(HeaderBarItem::MainMenu) + 1);

    auto objectNames = std::set<std::string_view> {};
    auto accessibleNames = std::set<std::string_view> {};
    auto iconNames = std::set<std::string_view> {};
    auto bundledIcons = std::set<std::string_view> {};
    for (auto const& row: HeaderBarItems)
    {
        CHECK_FALSE(row.objectName.empty());
        CHECK_FALSE(row.accessibleName.empty());
        CHECK_FALSE(row.iconName.empty());
        CHECK(row.bundledIcon.starts_with("qrc:/contour/icons/"));
        objectNames.insert(row.objectName);
        accessibleNames.insert(row.accessibleName);
        iconNames.insert(row.iconName);
        bundledIcons.insert(row.bundledIcon);
    }
    CHECK(objectNames.size() == HeaderBarItems.size());
    CHECK(accessibleNames.size() == HeaderBarItems.size());
    CHECK(iconNames.size() == HeaderBarItems.size());
    CHECK(bundledIcons.size() == HeaderBarItems.size());
    // Each menu is popped below one item at most; search pops none.
    for (auto const menu: { HeaderBarMenu::NewTab, HeaderBarMenu::Main })
        CHECK(std::ranges::count(HeaderBarItems, menu, &HeaderBarItemRow::menu) == 1);
    CHECK(headerBarItemRow(HeaderBarItem::Search).menu == HeaderBarMenu::None);

    // The canvas's accessible names.
    CHECK(headerBarItemRow(HeaderBarItem::NewTab).accessibleName == "New tab");
    CHECK(headerBarItemRow(HeaderBarItem::Search).accessibleName == "Search");
    CHECK(headerBarItemRow(HeaderBarItem::MainMenu).accessibleName == "Main menu");
}

TEST_CASE("headerBar.slotNames", "[contour][headerbar]")
{
    auto names = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, HeaderBarSlotCount))
        names.insert(headerBarSlotName(static_cast<HeaderBarSlot>(index)));
    CHECK(names == std::set<std::string_view> { "hidden", "leading", "trailing" });
}
