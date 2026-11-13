// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChip.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::window;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;

TEST_CASE("locationChip.daemonName", "[LocationChip]")
{
    auto const target = [](SwitchDaemonKind kind, std::string daemon) {
        return SwitchTarget { .kind = kind, .daemon = std::move(daemon) };
    };
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "default"), "") == "local");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "work"), "") == "work");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "local"), "") == "local");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Same, ""), "") == "daemon");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Host, "starfive"), "starfive") == "starfive");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Host, "sf"), "me@starfive") == "me@starfive");
}

TEST_CASE("locationChip.daemonDot", "[LocationChip]")
{
    CHECK(daemonDotOf(DaemonLocality::Remote, HostChipState::Connected) == DaemonDot::Shown);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Connected) == DaemonDot::Hidden);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Reconnecting) == DaemonDot::Shown);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Disconnected) == DaemonDot::Shown);
}

TEST_CASE("locationChip.fit", "[LocationChip]")
{
    constexpr auto widths = LocationChipWidths { 300, 220, 160, 110 };
    CHECK(locationChipFit(widths, 400) == LocationChipFit::Full);
    CHECK(locationChipFit(widths, 300) == LocationChipFit::Full);
    CHECK(locationChipFit(widths, 250) == LocationChipFit::NoStateText);
    CHECK(locationChipFit(widths, 200) == LocationChipFit::DaemonElided);
    CHECK(locationChipFit(widths, 120) == LocationChipFit::DotOnly);
    CHECK(locationChipFit(widths, 90) == LocationChipFit::WorkspaceElided);
    CHECK(locationChipFit(widths, 10) == LocationChipFit::WorkspaceElided);
}

TEST_CASE("locationChip.accessibleName", "[LocationChip]")
{
    auto summary = contour::window::SidebarSummary { .currentWorkspace = "billing" };
    CHECK(locationChipAccessibleName("starfive", summary) == "Workspace billing on starfive");
    CHECK(locationChipAccessibleName("", summary) == workspaceChipAccessibleName(summary));
    CHECK(locationChipAccessibleName("", summary) == "Workspace billing");
    summary.otherWorkspaces = contour::window::Urgency::NeedsYou;
    CHECK(locationChipAccessibleName("starfive", summary)
          == "Workspace billing on starfive, another workspace needs you");
    CHECK(locationChipAccessibleName("", summary) == workspaceChipAccessibleName(summary));
}

TEST_CASE("locationChip.accessibleDescription", "[LocationChip]")
{
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, "collapsed") == "collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Reconnecting, "collapsed")
          == "Reconnecting, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::NeedsAttention, "collapsed")
          == "Reconnecting, needs your attention, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Disconnected, "collapsed")
          == "Disconnected, collapsed");
}

TEST_CASE("locationChip.stateGlyph", "[LocationChip]")
{
    CHECK(daemonStateGlyph(HostChipState::Connected) == "●");
    CHECK(daemonStateGlyph(HostChipState::Reconnecting) == "◑");
    CHECK(daemonStateGlyph(HostChipState::Disconnected) == "✕");
    CHECK(daemonStateGlyph(HostChipState::NeedsAttention) == "!");
}

TEST_CASE("locationChip.toolTip", "[LocationChip]")
{
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::NoStateText, "starfive")
          == "Reconnecting to starfive · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::NeedsAttention, LocationChipFit::DotOnly, "starfive")
          == "Reconnecting to starfive needs your attention · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Disconnected, LocationChipFit::DaemonElided, "starfive")
          == "Disconnected from starfive · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::Full, "starfive")
          == "Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Connected, LocationChipFit::DotOnly, "local")
          == "Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::DotOnly, "")
          == "Switch workspace");
}

TEST_CASE("locationChip.place", "[LocationChip]")
{
    constexpr auto bands = std::array {
        BarPresence::Hidden, BarPresence::Shown, BarPresence::RevealOnEdge, BarPresence::Pinned
    };
    constexpr auto centres = std::array {
        BarCentre::Empty, BarCentre::Tabs, BarCentre::Title, BarCentre::Breadcrumb, BarCentre::ChipAndTabs
    };
    for (auto const band: bands)
        for (auto const centre: centres)
        {
            auto const daemon = locationChipPlace(band, centre, DaemonHosting::Daemon);
            auto const inProcess = locationChipPlace(band, centre, DaemonHosting::InProcess);
            auto expectedDaemon = LocationChipPlace::Band;
            if (band == BarPresence::Hidden)
                expectedDaemon = LocationChipPlace::None;
            else if (centre == BarCentre::Breadcrumb)
                expectedDaemon = LocationChipPlace::Sidebar;
            auto const expectedInProcess = band != BarPresence::Hidden && centre == BarCentre::ChipAndTabs
                                               ? LocationChipPlace::Band
                                               : LocationChipPlace::None;
            CHECK(daemon == expectedDaemon);
            CHECK(inProcess == expectedInProcess);
        }
}

TEST_CASE("locationChip.placeName", "[LocationChip]")
{
    CHECK(locationChipPlaceName(LocationChipPlace::None) == "none");
    CHECK(locationChipPlaceName(LocationChipPlace::Band) == "band");
    CHECK(locationChipPlaceName(LocationChipPlace::Sidebar) == "sidebar");
}
