// SPDX-License-Identifier: Apache-2.0
//
// How each sidebar look wears the selection is one table (row x focus -> fill, ring, pill), asserted whole.

#include <contour/config/ChromeTokenRows.hpp>
#include <contour/window/SidebarSelection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::config::SidebarLook;
using namespace contour::window;

namespace
{
constexpr auto Foci = std::array { SidebarFocus::Elsewhere, SidebarFocus::InTree, SidebarFocus::OnRow };
} // namespace

TEST_CASE("SidebarSelection.macOS is accent while the tree has the keyboard, grey otherwise", "[sidebar]")
{
    auto const paint = [](SidebarFocus focus) {
        return sidebarSelectionPaint(SidebarLook::SourceList, RowSelection::Selected, focus);
    };
    CHECK(paint(SidebarFocus::Elsewhere)
          == SidebarSelectionPaint { SelectionFill::Quiet, FocusRing::Hidden, AccentPill::Hidden });
    CHECK(paint(SidebarFocus::InTree)
          == SidebarSelectionPaint { SelectionFill::Emphasized, FocusRing::Hidden, AccentPill::Hidden });
    CHECK(paint(SidebarFocus::OnRow)
          == SidebarSelectionPaint { SelectionFill::Emphasized, FocusRing::Shown, AccentPill::Hidden });
}

TEST_CASE("SidebarSelection.Linux is a neutral fill, focused or not, and the keyboard adds the ring",
          "[sidebar]")
{
    for (auto const focus: Foci)
    {
        auto const paint = sidebarSelectionPaint(SidebarLook::HeaderPane, RowSelection::Selected, focus);
        CHECK(paint.fill == SelectionFill::Quiet);
        CHECK(paint.pill == AccentPill::Hidden);
        CHECK(paint.ring == (focus == SidebarFocus::OnRow ? FocusRing::Shown : FocusRing::Hidden));
    }
}

TEST_CASE("SidebarSelection.Fluent is a grey fill with the accent pill, focused or not", "[sidebar]")
{
    for (auto const focus: Foci)
    {
        auto const paint = sidebarSelectionPaint(SidebarLook::NavigationPane, RowSelection::Selected, focus);
        CHECK(paint.fill == SelectionFill::Quiet);
        CHECK(paint.pill == AccentPill::Shown);
        CHECK(paint.ring == (focus == SidebarFocus::OnRow ? FocusRing::Shown : FocusRing::Hidden));
    }
}

TEST_CASE("SidebarSelection.Terminal keeps inverse video while the tree has the keyboard", "[sidebar]")
{
    CHECK(sidebarSelectionPaint(SidebarLook::CellTree, RowSelection::Selected, SidebarFocus::InTree).fill
          == SelectionFill::Emphasized);
    CHECK(sidebarSelectionPaint(SidebarLook::CellTree, RowSelection::Selected, SidebarFocus::Elsewhere).fill
          == SelectionFill::Quiet);
}

TEST_CASE("SidebarSelection.a folded workspace is never more than the quiet fill, an unselected row has none",
          "[sidebar]")
{
    for (auto const look: { SidebarLook::SourceList,
                            SidebarLook::NavigationPane,
                            SidebarLook::HeaderPane,
                            SidebarLook::CellTree })
        for (auto const focus: Foci)
        {
            auto const folded = sidebarSelectionPaint(look, RowSelection::ContainsSelected, focus);
            CHECK(folded.fill == SelectionFill::Quiet);
            CHECK(folded.pill == AccentPill::Hidden);
            auto const plain = sidebarSelectionPaint(look, RowSelection::None, focus);
            CHECK(plain.fill == SelectionFill::None);
            CHECK(plain.pill == AccentPill::Hidden);
        }
}

TEST_CASE("SidebarSelection.the pill rule agrees with the pill's width token", "[sidebar]")
{
    using namespace contour::config;
    for (auto const style:
         { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        auto const tokens = resolveChromeTokens(style, AccessibilityPreferences {});
        auto const paint =
            sidebarSelectionPaint(tokens.layout.sidebar, RowSelection::Selected, SidebarFocus::Elsewhere);
        CAPTURE(static_cast<int>(style));
        CHECK((tokens.sidebar.accentPillWidth > 0) == (paint.pill == AccentPill::Shown));
    }
}

TEST_CASE("SidebarSelection.the rows that never emphasize take no accent for their quiet fill", "[sidebar]")
{
    using namespace contour::config;
    for (auto const style: { ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        auto const tokens = resolveChromeTokens(style, AccessibilityPreferences {});
        CAPTURE(static_cast<int>(style));
        CHECK(tokens.colors[static_cast<size_t>(ChromeColorRole::SidebarSelectionFillInactive)].kind
              != ColorSourceKind::PaletteHighlight);
    }
}
