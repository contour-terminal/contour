// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/window/HeaderBarLayout.hpp>
#include <contour/window/SidebarControls.hpp>
#include <contour/window/SidebarToggle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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
    // Each look's controls look different from one another.
    for (auto const& row: SidebarControlRows)
    {
        CHECK(row.faces[0] != row.faces[1]);
        CHECK(row.faces[1] != row.faces[2]);
        CHECK(row.faces[0] != row.faces[2]);
    }
    CHECK(sidebarControlName(SidebarControl::NewWorkspace) == "newWorkspace");
}

TEST_CASE("SidebarControls: only the new-workspace control takes a dropped pane or tab, on every look",
          "[sidebar][sidebarcontrols]")
{
    for (auto const& row: SidebarControlRows)
        for (auto const control:
             { SidebarControl::Toggle, SidebarControl::NewWorkspace, SidebarControl::Filter })
            CHECK((row.faces[static_cast<size_t>(control)].drop == SidebarControlDrop::NewWorkspace)
                  == (control == SidebarControl::NewWorkspace));
}

TEST_CASE("SidebarControls: each control runs an action of the catalog, whatever the look",
          "[sidebar][sidebarcontrols]")
{
    CHECK(sidebarControlCommand(SidebarControl::Toggle) == "ToggleSidebar");
    CHECK(sidebarControlCommand(SidebarControl::NewWorkspace) == "NewWorkspace");
    CHECK(sidebarControlCommand(SidebarControl::Filter) == "FilterWorkspaces");
    for (auto const command: SidebarControlCommands)
    {
        INFO(command);
        CHECK(contour::actions::fromString(std::string { command }).has_value());
    }
}
