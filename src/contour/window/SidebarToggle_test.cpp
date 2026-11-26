// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/window/HeaderBarLayout.hpp>
#include <contour/window/SidebarControls.hpp>
#include <contour/window/SidebarToggle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <string>

using namespace contour::window;
using contour::config::SidebarSpan;
using contour::config::SidebarVisibility;

namespace
{

struct ToggleCase
{
    SidebarToggleInputs inputs;
    SidebarToggleHost expected = SidebarToggleHost::None;
};

constexpr auto Beside = SidebarSpan::BesideBand;
constexpr auto Under = SidebarSpan::UnderBand;
constexpr auto Shown = SidebarVisibility::Shown;
constexpr auto Hidden = SidebarVisibility::Hidden;

constexpr auto ToggleCases = std::array {
    // Under the band (Fluent, ContourLinux, Terminal): the band's leading end, shown or hidden.
    ToggleCase { { .span = Under, .visibility = Shown }, SidebarToggleHost::Band },
    ToggleCase { { .span = Under, .visibility = Hidden }, SidebarToggleHost::Band },
    // Beside the band (macOS): the sidebar takes the band's leading end, and its title row holds the toggle
    // at the spot the band holds it while the sidebar is hidden.
    ToggleCase { { .span = Beside, .visibility = Shown }, SidebarToggleHost::SidebarTitleRow },
    ToggleCase { { .span = Beside, .visibility = Hidden }, SidebarToggleHost::Band },
    // A band at the bottom stands no sidebar beside it.
    ToggleCase { { .span = Beside, .visibility = Shown, .bandEdge = BarEdge::Bottom },
                 SidebarToggleHost::Band },
    // A pinned band rests on screen as a shown one does.
    ToggleCase { { .span = Under, .visibility = Shown, .band = BarPresence::Pinned },
                 SidebarToggleHost::Band },
    // A band that only reveals at the screen's edge: the shown sidebar's own row is the steady place; with
    // the sidebar hidden, the band is the only one, once revealed.
    ToggleCase { { .span = Under, .visibility = Shown, .band = BarPresence::RevealOnEdge },
                 SidebarToggleHost::SidebarTitleRow },
    ToggleCase { { .span = Under, .visibility = Hidden, .band = BarPresence::RevealOnEdge },
                 SidebarToggleHost::Band },
    // No band at all: the shown sidebar's row, else nowhere.
    ToggleCase { { .span = Under, .visibility = Shown, .band = BarPresence::Hidden },
                 SidebarToggleHost::SidebarTitleRow },
    ToggleCase { { .span = Under, .visibility = Hidden, .band = BarPresence::Hidden },
                 SidebarToggleHost::None },
};

} // namespace

TEST_CASE("the sidebar toggle stays at the band's leading end unless the sidebar takes it",
          "[contour][window][sidebar]")
{
    for (auto const& row: ToggleCases)
    {
        INFO("span " << static_cast<int>(row.inputs.span) << " visibility "
                     << static_cast<int>(row.inputs.visibility) << " band "
                     << static_cast<int>(row.inputs.band) << " edge "
                     << static_cast<int>(row.inputs.bandEdge));
        CHECK(sidebarToggleOf(row.inputs).host == row.expected);
    }
}

TEST_CASE("the sidebar toggle says what clicking it does", "[contour][window][sidebar]")
{
    CHECK(sidebarToggleOf({ .visibility = Shown }).label == "Hide sidebar");
    CHECK(sidebarToggleOf({ .visibility = Hidden }).label == "Show sidebar");
    CHECK(sidebarToggleHostName(SidebarToggleHost::Band) == "band");
    CHECK(sidebarToggleHostName(SidebarToggleHost::SidebarTitleRow) == "sidebar");
    CHECK(sidebarToggleHostName(SidebarToggleHost::None) == "none");
}

TEST_CASE("SidebarToggle: with no toggle on screen, the pane's menu is the way back",
          "[sidebar][sidebartoggle]")
{
    using contour::command::SidebarRecovery;
    struct Case
    {
        SidebarVisibility visibility;
        SidebarToggleHost host;
        SidebarRecovery expected;
    };
    for (auto const& [visibility, host, expected]: std::array {
             Case { Hidden, SidebarToggleHost::None, SidebarRecovery::Offered },
             Case { Hidden, SidebarToggleHost::Band, SidebarRecovery::None },
             Case { Shown, SidebarToggleHost::None, SidebarRecovery::None },
             Case { Shown, SidebarToggleHost::SidebarTitleRow, SidebarRecovery::None },
         })
        CHECK(sidebarRecoveryOf(visibility, host) == expected);

    // Every host reads back from the name QML reports it by; anything else is none.
    for (auto const host:
         { SidebarToggleHost::None, SidebarToggleHost::Band, SidebarToggleHost::SidebarTitleRow })
        CHECK(sidebarToggleHostNamed(sidebarToggleHostName(host)) == host);
    CHECK(sidebarToggleHostNamed("elsewhere") == SidebarToggleHost::None);
}

TEST_CASE("SidebarControls: one glyph means one thing in a bar", "[sidebar][sidebarcontrols]")
{
    using contour::config::SidebarLook;
    // The header bar's own items keep their icons to themselves: the sidebar's toggle is not the main menu's
    // hamburger, and its new workspace is not the new tab's "+".
    auto const& header = SidebarControlRows[static_cast<size_t>(SidebarLook::HeaderPane)];
    for (auto const& face: header.faces)
    {
        CHECK_FALSE(face.iconName.empty()); // themed, as the bar's other items are
        CHECK(face.glyph.empty());          // never a text glyph beside the bar's icons
        for (auto const& item: HeaderBarItems)
        {
            CHECK(face.iconName != item.iconName);
            CHECK(face.bundledIcon != item.bundledIcon);
        }
    }
    // Each look's controls look different from one another, and share no icon or glyph.
    for (auto const& row: SidebarControlRows)
        for (auto const a: std::views::iota(size_t { 0 }, SidebarControlCount))
            for (auto const b: std::views::iota(a + 1, SidebarControlCount))
            {
                INFO("look " << static_cast<int>(row.look) << ", controls " << a << " and " << b);
                auto const& first = row.faces[a];
                auto const& second = row.faces[b];
                CHECK(first != second);
                if (!first.bundledIcon.empty())
                    CHECK(first.bundledIcon != second.bundledIcon);
                if (!first.iconName.empty())
                    CHECK(first.iconName != second.iconName);
                if (!first.glyph.empty())
                    CHECK(first.glyph != second.glyph);
            }
    CHECK(sidebarControlName(SidebarControl::NewWorkspace) == "newWorkspace");
    CHECK(sidebarControlName(SidebarControl::NeedsMe) == "needsMe");
    CHECK(sidebarControlName(SidebarControl::Switcher) == "switcher");
}

TEST_CASE("the switcher and needs-me controls have a face on every look, none shared with another control in "
          "the row",
          "[sidebar][sidebarcontrols]")
{
    for (auto const& row: SidebarControlRows)
        for (auto const control: { SidebarControl::Switcher, SidebarControl::NeedsMe })
        {
            INFO("look " << static_cast<int>(row.look) << ", control " << sidebarControlName(control));
            auto const& face = row.faces[static_cast<size_t>(control)];
            CHECK((!face.bundledIcon.empty() || !face.glyph.empty()));
            // Never a text glyph beside the icons of a look that draws icons.
            CHECK((face.glyph.empty() || row.look == contour::config::SidebarLook::CellTree));
            for (auto const other: std::views::iota(size_t { 0 }, SidebarControlCount))
            {
                if (other == static_cast<size_t>(control))
                    continue;
                auto const& rival = row.faces[other];
                CHECK(face != rival);
                if (!face.bundledIcon.empty())
                    CHECK(face.bundledIcon != rival.bundledIcon);
                if (!face.glyph.empty())
                    CHECK(face.glyph != rival.glyph);
            }
            // Nor any of the header bar's own items.
            for (auto const& item: HeaderBarItems)
            {
                if (!face.iconName.empty())
                    CHECK(face.iconName != item.iconName);
                if (!face.bundledIcon.empty())
                    CHECK(face.bundledIcon != item.bundledIcon);
            }
        }
}

TEST_CASE("narrow sidebars drop needs-me, then the switcher", "[sidebar][sidebarcontrols]")
{
    constexpr auto Each = 30.0;
    constexpr auto Shown = size_t { 5 };
    CHECK(sidebarControlFitOf(Each * Shown, Each, Shown) == SidebarControlFit::All);
    CHECK(sidebarControlFitOf((Each * Shown) - 1, Each, Shown) == SidebarControlFit::NoNeedsMe);
    CHECK(sidebarControlFitOf(Each * (Shown - 1), Each, Shown) == SidebarControlFit::NoNeedsMe);
    CHECK(sidebarControlFitOf((Each * (Shown - 1)) - 1, Each, Shown) == SidebarControlFit::NoSwitcher);
    CHECK(sidebarControlFitOf(0, Each, Shown) == SidebarControlFit::NoSwitcher);
    // At the narrowest sidebar (160 px) with the toolbar button's 28 px: the four fit, the fifth goes.
    CHECK(sidebarControlFitOf(160, 34, 5) == SidebarControlFit::NoNeedsMe);
    CHECK(sidebarControlFitOf(160, 34, 4) == SidebarControlFit::All);
}

TEST_CASE("SidebarControls: the needs-me control never draws the rows' waiting mark",
          "[sidebar][sidebarcontrols]")
{
    // `!` already means "waiting" on a row of the tree beside it.
    for (auto const& row: SidebarControlRows)
        CHECK(row.faces[static_cast<size_t>(SidebarControl::NeedsMe)].glyph != "!");
}

TEST_CASE("SidebarControls: only the new-workspace control takes a dropped pane or tab, on every look",
          "[sidebar][sidebarcontrols]")
{
    for (auto const& row: SidebarControlRows)
        for (auto const control: std::views::iota(size_t { 0 }, SidebarControlCount)
                                     | std::views::transform([](size_t i) { return SidebarControl(i); }))
            CHECK((row.faces[static_cast<size_t>(control)].drop == SidebarControlDrop::NewWorkspace)
                  == (control == SidebarControl::NewWorkspace));
}

TEST_CASE("SidebarControls: each control runs an action of the catalog, whatever the look",
          "[sidebar][sidebarcontrols]")
{
    CHECK(sidebarControlCommand(SidebarControl::Toggle) == "ToggleSidebar");
    CHECK(sidebarControlCommand(SidebarControl::NewWorkspace) == "NewWorkspace");
    CHECK(sidebarControlCommand(SidebarControl::Filter) == "FilterWorkspaces");
    CHECK(sidebarControlCommand(SidebarControl::Switcher) == "ShowSwitcher");
    CHECK(sidebarControlCommand(SidebarControl::NeedsMe) == "ToggleSidebarNeedsMe");
    for (auto const command: SidebarControlCommands)
    {
        INFO(command);
        CHECK(contour::actions::fromString(std::string { command }).has_value());
    }
}

TEST_CASE("the switcher control is a swap on every look", "[sidebar][controls]")
{
    // One metaphor for "go to another daemon or workspace": two opposed arrows, never a jump, redo or share
    // arrow, nor a ">" that echoes the sidebar's toggle.
    using contour::config::SidebarLook;
    for (auto const index: std::views::iota(std::size_t { 0 }, contour::window::SidebarControlRows.size()))
    {
        auto const look = static_cast<SidebarLook>(index);
        INFO("look " << index);
        auto const& face =
            contour::window::sidebarControlFace(look, contour::window::SidebarControl::Switcher);
        if (look == SidebarLook::CellTree)
            CHECK(face.glyph == "⇄");
        else
            CHECK(face.bundledIcon == "qrc:/contour/icons/view-swap-symbolic.svg");
    }
}
