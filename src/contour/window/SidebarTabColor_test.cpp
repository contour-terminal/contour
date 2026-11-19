// SPDX-License-Identifier: Apache-2.0
//
// Which visual state a sidebar row's tab colour is drawn in, and what each style paints from it: two tables,
// asserted whole.

#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/window/SidebarTabColor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>

using contour::config::SidebarTabColorStyle;
using namespace contour::window;
using vtbackend::RGBColor;

namespace
{
constexpr auto Selections =
    std::array { RowSelection::None, RowSelection::Selected, RowSelection::ContainsSelected };
constexpr auto Pointers = std::array { RowPointer::Away, RowPointer::Hovering };
constexpr auto Foci = std::array { WindowFocus::Inactive, WindowFocus::Active };
constexpr auto Styles =
    std::array { SidebarTabColorStyle::Tint, SidebarTabColorStyle::Bar, SidebarTabColorStyle::Name };
constexpr auto States = std::array { TabVisualState::Active,
                                     TabVisualState::Hover,
                                     TabVisualState::Inactive,
                                     TabVisualState::InactiveWindowUnfocused };

constexpr auto Orange = RGBColor { 0xE8, 0x85, 0x3A };
constexpr auto RowFill = RGBColor { 0x20, 0x20, 0x20 };
} // namespace

TEST_CASE("sidebarTabVisualState.the selected row keeps the colour, others follow the pointer and the window",
          "[sidebar]")
{
    using enum TabVisualState;
    struct Expectation
    {
        RowSelection selection;
        RowPointer pointer;
        WindowFocus focus;
        TabVisualState state;
    };
    constexpr auto Table = std::array {
        Expectation { RowSelection::None, RowPointer::Away, WindowFocus::Inactive, InactiveWindowUnfocused },
        Expectation { RowSelection::None, RowPointer::Away, WindowFocus::Active, Inactive },
        Expectation { RowSelection::None, RowPointer::Hovering, WindowFocus::Inactive, Hover },
        Expectation { RowSelection::None, RowPointer::Hovering, WindowFocus::Active, Hover },
        Expectation { RowSelection::Selected, RowPointer::Away, WindowFocus::Inactive, Active },
        Expectation { RowSelection::Selected, RowPointer::Away, WindowFocus::Active, Active },
        Expectation { RowSelection::Selected, RowPointer::Hovering, WindowFocus::Inactive, Active },
        Expectation { RowSelection::Selected, RowPointer::Hovering, WindowFocus::Active, Active },
        Expectation { RowSelection::ContainsSelected, RowPointer::Away, WindowFocus::Inactive, Active },
        Expectation { RowSelection::ContainsSelected, RowPointer::Away, WindowFocus::Active, Active },
        Expectation { RowSelection::ContainsSelected, RowPointer::Hovering, WindowFocus::Inactive, Active },
        Expectation { RowSelection::ContainsSelected, RowPointer::Hovering, WindowFocus::Active, Active },
    };
    static_assert(Table.size() == Selections.size() * Pointers.size() * Foci.size());

    for (auto const& row: Table)
    {
        CAPTURE(static_cast<int>(row.selection), static_cast<int>(row.pointer), static_cast<int>(row.focus));
        CHECK(sidebarTabVisualState(row.selection, row.pointer, row.focus) == row.state);
    }
}

TEST_CASE("sidebarTabVisualState.in an inactive window the selected row keeps more colour than a resting one",
          "[sidebar]")
{
    auto const fractionOf = [](RowSelection selection) {
        return tabColorFraction(sidebarTabVisualState(selection, RowPointer::Away, WindowFocus::Inactive));
    };
    CHECK(fractionOf(RowSelection::Selected) > fractionOf(RowSelection::None));
    CHECK(fractionOf(RowSelection::ContainsSelected) > fractionOf(RowSelection::None));
}

TEST_CASE("sidebarMarkInk.a mark keeps its colour unless it would vanish into the tint", "[sidebar]")
{
    constexpr auto Red = RGBColor { 0xE0, 0x1B, 0x24 };
    for (auto const style: Styles)
        for (auto const state: States)
        {
            CAPTURE(static_cast<int>(style), static_cast<int>(state));
            auto const paint = sidebarTabColorPaint(style, Orange, RowFill, state);
            auto const ink = sidebarMarkInk(Orange, paint);
            if (!paint.fill)
            {
                // Bar and name lay no tint under the mark: it is drawn on the sidebar as ever.
                CHECK(ink == Orange);
                continue;
            }
            CHECK(vtbackend::contrastRatio(ink, *paint.fill) >= vtbackend::MinimumMarkContrast);
            if (vtbackend::contrastRatio(Orange, *paint.fill) >= vtbackend::MinimumMarkContrast)
                CHECK(ink == Orange);
            else
                CHECK(ink == *paint.ink);
        }

    // The worst case: a mark the very colour of the selected row's full tint takes the tint's ink.
    auto const selected =
        sidebarTabColorPaint(SidebarTabColorStyle::Tint, Orange, RowFill, TabVisualState::Active);
    CHECK(sidebarMarkInk(Orange, selected) == *selected.ink);
    // A mark that stands off the tint keeps its own colour; an uncoloured row changes nothing.
    auto const blueTint = sidebarTabColorPaint(
        SidebarTabColorStyle::Tint, RGBColor { 0x1C, 0x2A, 0x6B }, RowFill, TabVisualState::Active);
    CHECK(sidebarMarkInk(Orange, blueTint) == Orange);
    CHECK(sidebarMarkInk(Red, SidebarTabColorPaint {}) == Red);
}

TEST_CASE("sidebarTabColorPaint.each style paints exactly its own kind", "[sidebar]")
{
    for (auto const style: Styles)
        for (auto const state: States)
        {
            CAPTURE(static_cast<int>(style), static_cast<int>(state));
            CHECK(sidebarTabColorPaint(style, std::nullopt, RowFill, state) == SidebarTabColorPaint {});

            auto const coloured = sidebarTabColorPaint(style, Orange, RowFill, state);
            switch (style)
            {
                case SidebarTabColorStyle::Tint: {
                    auto const fill = tabBackgroundColor(Orange, RowFill, state);
                    CHECK(coloured
                          == SidebarTabColorPaint {
                              .fill = fill, .bar = std::nullopt, .ink = contrastingTextColor(fill) });
                    break;
                }
                case SidebarTabColorStyle::Bar:
                    CHECK(
                        coloured
                        == SidebarTabColorPaint { .fill = std::nullopt, .bar = Orange, .ink = std::nullopt });
                    break;
                case SidebarTabColorStyle::Name:
                    CHECK(
                        coloured
                        == SidebarTabColorPaint { .fill = std::nullopt, .bar = std::nullopt, .ink = Orange });
                    break;
            }
        }
}

TEST_CASE("sidebarTabColorPaint.an uncoloured tab paints the same in every style", "[sidebar]")
{
    for (auto const state: States)
    {
        CAPTURE(static_cast<int>(state));
        auto const tint = sidebarTabColorPaint(SidebarTabColorStyle::Tint, std::nullopt, RowFill, state);
        CHECK(tint == sidebarTabColorPaint(SidebarTabColorStyle::Bar, std::nullopt, RowFill, state));
        CHECK(tint == sidebarTabColorPaint(SidebarTabColorStyle::Name, std::nullopt, RowFill, state));
    }
}

TEST_CASE("sidebarTabColorPaint.every row draws the bar, as wide as Fluent's pill on the native rows",
          "[sidebar]")
{
    using namespace contour::config;
    auto const fluentPill = ChromeTokenRows[static_cast<size_t>(ChromeStyle::Fluent)].sidebar.accentPillWidth;
    for (auto const style: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        CAPTURE(static_cast<int>(style));
        CHECK(ChromeTokenRows[static_cast<size_t>(style)].sidebar.tabColorBarWidth == fluentPill);
    }
    CHECK(ChromeTokenRows[static_cast<size_t>(ChromeStyle::Terminal)].sidebar.tabColorBarWidth == 1);
}

TEST_CASE("sidebarTabColorPaint.a tinted row's ink contrasts with its fill", "[sidebar]")
{
    constexpr auto White = RGBColor { 0xFF, 0xFF, 0xFF };
    constexpr auto Black = RGBColor { 0x00, 0x00, 0x00 };
    auto const dark = sidebarTabColorPaint(
        SidebarTabColorStyle::Tint, RGBColor { 0x1C, 0x2A, 0x6B }, RowFill, TabVisualState::Active);
    auto const light = sidebarTabColorPaint(
        SidebarTabColorStyle::Tint, RGBColor { 0xF2, 0xE0, 0x8A }, RowFill, TabVisualState::Active);
    CHECK(dark.ink == White);
    CHECK(light.ink == Black);
}
