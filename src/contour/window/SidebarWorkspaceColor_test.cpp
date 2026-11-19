// SPDX-License-Identifier: Apache-2.0
//
// What a workspace's colour paints on each sidebar row, by style, row kind and state: one table, asserted
// whole, and the tokens the rail and the swatch are drawn at.

#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/window/SidebarWorkspaceColor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>

using contour::config::SidebarWorkspaceColorStyle;
using namespace contour::window;
using vtbackend::RGBColor;

namespace
{
constexpr auto Styles = std::array { SidebarWorkspaceColorStyle::Tint,
                                     SidebarWorkspaceColorStyle::Group,
                                     SidebarWorkspaceColorStyle::Swatch };
constexpr auto Kinds = std::array { SidebarRowKind::Workspace, SidebarRowKind::Tab, SidebarRowKind::Pane };
constexpr auto States = std::array { TabVisualState::Active,
                                     TabVisualState::Pressed,
                                     TabVisualState::Hover,
                                     TabVisualState::Inactive,
                                     TabVisualState::InactiveWindowUnfocused };

constexpr auto Magenta = RGBColor { 0xC2, 0x54, 0x8F };
constexpr auto RowFill = RGBColor { 0x20, 0x20, 0x20 };

/// The paint each style gives, spelled out per style and kind, for a coloured workspace.
SidebarWorkspaceColorPaint expectedPaint(SidebarWorkspaceColorStyle style,
                                         SidebarRowKind kind,
                                         TabVisualState state)
{
    auto paint = SidebarWorkspaceColorPaint {};
    switch (style)
    {
        case SidebarWorkspaceColorStyle::Tint:
            if (kind == SidebarRowKind::Workspace)
            {
                paint.fill = tabBackgroundColor(Magenta, RowFill, state);
                paint.ink = contrastingTextColor(*paint.fill);
            }
            break;
        case SidebarWorkspaceColorStyle::Group:
            paint.rail = Magenta;
            paint.wash = vtbackend::mixColor(RowFill, Magenta, WorkspaceWashFraction);
            break;
        case SidebarWorkspaceColorStyle::Swatch:
            if (kind == SidebarRowKind::Workspace)
                paint.swatch = Magenta;
            break;
    }
    return paint;
}
} // namespace

TEST_CASE("sidebarWorkspaceColorPaint.each style paints its own kind on its own rows", "[sidebar]")
{
    for (auto const style: Styles)
        for (auto const kind: Kinds)
            for (auto const state: States)
            {
                CAPTURE(static_cast<int>(style), static_cast<int>(kind), static_cast<int>(state));
                CHECK(sidebarWorkspaceColorPaint(style, kind, std::nullopt, RowFill, state)
                      == SidebarWorkspaceColorPaint {});
                CHECK(sidebarWorkspaceColorPaint(style, kind, Magenta, RowFill, state)
                      == expectedPaint(style, kind, state));
            }
}

TEST_CASE("sidebarWorkspaceColorPaint.an uncoloured workspace paints nothing in any style", "[sidebar]")
{
    for (auto const style: Styles)
        for (auto const kind: Kinds)
            for (auto const state: States)
            {
                CAPTURE(static_cast<int>(style), static_cast<int>(kind), static_cast<int>(state));
                CHECK(sidebarWorkspaceColorPaint(style, kind, std::nullopt, RowFill, state)
                      == SidebarWorkspaceColorPaint {});
            }
}

TEST_CASE("sidebarWorkspaceColorPaint.the tinted row fades with its state as a tinted tab does", "[sidebar]")
{
    auto const active = sidebarWorkspaceColorPaint(SidebarWorkspaceColorStyle::Tint,
                                                   SidebarRowKind::Workspace,
                                                   Magenta,
                                                   RowFill,
                                                   TabVisualState::Active);
    auto const resting = sidebarWorkspaceColorPaint(SidebarWorkspaceColorStyle::Tint,
                                                    SidebarRowKind::Workspace,
                                                    Magenta,
                                                    RowFill,
                                                    TabVisualState::Inactive);
    REQUIRE(active.fill);
    REQUIRE(resting.fill);
    CHECK(*active.fill != *resting.fill);
    CHECK(active.ink == contrastingTextColor(*active.fill));
}

TEST_CASE("sidebarWorkspaceColorPaint.a status mark on a tinted workspace row stays legible", "[sidebar]")
{
    // A magenta mark on the magenta tint does not stand off it and takes the tint's ink; a mark that does
    // keeps its own colour; an untinted row keeps every mark's colour.
    auto const tinted = sidebarWorkspaceColorPaint(SidebarWorkspaceColorStyle::Tint,
                                                   SidebarRowKind::Workspace,
                                                   Magenta,
                                                   RowFill,
                                                   TabVisualState::Active);
    REQUIRE(tinted.fill);
    REQUIRE(tinted.ink);
    CHECK(sidebarMarkInk(Magenta, tinted.tint()) == *tinted.ink);
    auto const contrasting = *tinted.ink;
    CHECK(sidebarMarkInk(contrasting, tinted.tint()) == contrasting);

    auto const swatch = sidebarWorkspaceColorPaint(SidebarWorkspaceColorStyle::Swatch,
                                                   SidebarRowKind::Workspace,
                                                   Magenta,
                                                   RowFill,
                                                   TabVisualState::Active);
    CHECK(sidebarMarkInk(Magenta, swatch.tint()) == Magenta);
}

TEST_CASE("sidebarWorkspaceColorPaint.the group wash is fainter than a resting tint", "[sidebar]")
{
    // The wash only says which rows belong together: it must read as a faint band, never as every row being
    // tinted like a resting coloured tab.
    auto const distance = [](RGBColor a, RGBColor b) {
        return std::abs(a.red - b.red) + std::abs(a.green - b.green) + std::abs(a.blue - b.blue);
    };
    for (auto const background: { RowFill, RGBColor { 0xF5, 0xF5, 0xF5 } })
    {
        CAPTURE(background.red);
        auto const paint = sidebarWorkspaceColorPaint(SidebarWorkspaceColorStyle::Group,
                                                      SidebarRowKind::Tab,
                                                      Magenta,
                                                      background,
                                                      TabVisualState::Active);
        REQUIRE(paint.wash);
        auto const resting = tabBackgroundColor(Magenta, background, TabVisualState::Inactive);
        CHECK(*paint.wash != background);
        CHECK(distance(*paint.wash, background) < distance(resting, background));
    }
    CHECK(WorkspaceWashFraction < tabColorFraction(TabVisualState::Inactive));
}

TEST_CASE("sidebarWorkspaceColorPaint.every chrome row draws a rail and a swatch", "[sidebar]")
{
    using namespace contour::config;
    for (auto const style:
         { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        CAPTURE(static_cast<int>(style));
        auto const& sidebar = ChromeTokenRows[static_cast<size_t>(style)].sidebar;
        CHECK(sidebar.workspaceRailWidth > 0);
        CHECK(sidebar.workspaceSwatchSize > 0);
    }
}
